/**
 * RiftLink Routing — ROUTE_REQ/REPLY
 */

#include "routing.h"
#include "node/node.h"
#include "radio/radio.h"
#include "neighbors/neighbors.h"
#include "async_tasks.h"
#include "log.h"
#include <Arduino.h>
#include <string.h>
#include "port/rtos_include.h"

// Payload: target[8], req_id[4], hops[1], sender[8] — sender = кто переслал этот REQ
#define ROUTE_PAYLOAD_LEN 21

struct RouteEntry {
  uint8_t dest[protocol::NODE_ID_LEN];
  uint8_t nextHop[protocol::NODE_ID_LEN];
  uint8_t hops;
  int8_t rssi;  // dBm последнего хопа (0 = неизвестно)
  uint32_t timestamp;
  bool used;
};

struct ReverseEntry {
  uint8_t originator[protocol::NODE_ID_LEN];
  uint32_t reqId;
  uint8_t prevHop[protocol::NODE_ID_LEN];
  uint32_t timestamp;
  bool used;
};

struct SeenEntry {
  uint8_t originator[protocol::NODE_ID_LEN];
  uint32_t reqId;
  uint8_t target[protocol::NODE_ID_LEN];
  uint32_t timestamp;
  bool used;
};

static RouteEntry s_routes[ROUTING_MAX_ROUTES];
static ReverseEntry s_reverse[ROUTING_MAX_REVERSE];
static SeenEntry s_seen[ROUTING_MAX_SEEN];
static uint32_t s_reqIdCounter = 0;
static bool s_inited = false;
static SemaphoreHandle_t s_mutex = nullptr;

namespace routing {

static int computeTrustScore(const uint8_t* nextHop, int8_t routeRssi) {
  // Простая линейная модель trust score для MVP (0..100).
  int ackPermille = neighbors::getAckRatePermille(nextHop);
  int ackScore = (ackPermille >= 0) ? (ackPermille / 10) : 50;
  int freshnessMs = (int)neighbors::getFreshnessMs(nextHop);
  int freshnessScore = 0;
  if (freshnessMs >= 0 && freshnessMs < 180000) {
    freshnessScore = 100 - (freshnessMs * 100 / 180000);
  }
  int rssi = routeRssi;
  if (rssi == 0) rssi = neighbors::getRssiFor(nextHop);
  int rssiScore = 0;
  if (rssi <= -120) rssiScore = 0;
  else if (rssi >= -50) rssiScore = 100;
  else rssiScore = (rssi + 120) * 100 / 70;
  int batteryMv = neighbors::getBatteryMv(nextHop);
  int batteryScore = 50;
  if (batteryMv >= 3300 && batteryMv <= 4300) batteryScore = (batteryMv - 3300) * 100 / 1000;
  if (batteryMv > 4300) batteryScore = 100;
  int score = (ackScore * 45 + freshnessScore * 20 + rssiScore * 25 + batteryScore * 10) / 100;
  if (score < 0) score = 0;
  if (score > 100) score = 100;
  return score;
}

void init() {
  if (s_inited) return;
  s_mutex = xSemaphoreCreateMutex();
  memset(s_routes, 0, sizeof(s_routes));
  memset(s_reverse, 0, sizeof(s_reverse));
  memset(s_seen, 0, sizeof(s_seen));
  s_inited = true;
}

static bool routeAliveAt(int idx, uint32_t now) {
  if (idx < 0 || idx >= ROUTING_MAX_ROUTES || !s_routes[idx].used) return false;
  if ((now - s_routes[idx].timestamp) >= ROUTING_ROUTE_TTL_MS) {
    s_routes[idx].used = false;
    return false;
  }
  return true;
}

static int findRouteByDestNextHop(const uint8_t* dest, const uint8_t* nextHop) {
  uint32_t now = millis();
  for (int i = 0; i < ROUTING_MAX_ROUTES; i++) {
    if (!routeAliveAt(i, now)) continue;
    if (memcmp(s_routes[i].dest, dest, protocol::NODE_ID_LEN) == 0 &&
        memcmp(s_routes[i].nextHop, nextHop, protocol::NODE_ID_LEN) == 0) {
      return i;
    }
  }
  return -1;
}

static int findBestRoute(const uint8_t* dest) {
  uint32_t now = millis();
  int best = -1;
  int bestScore = -10000;
  int bestOnline = -1;
  uint8_t bestHops = 255;
  for (int i = 0; i < ROUTING_MAX_ROUTES; i++) {
    if (!routeAliveAt(i, now)) continue;
    if (memcmp(s_routes[i].dest, dest, protocol::NODE_ID_LEN) != 0) continue;
    int trust = computeTrustScore(s_routes[i].nextHop, s_routes[i].rssi);
    int online = neighbors::isOnline(s_routes[i].nextHop) ? 1 : 0;
    // Сначала предпочитаем online next-hop, затем trust, затем меньше hops.
    if (online > bestOnline ||
        (online == bestOnline && (trust > bestScore ||
            (trust == bestScore && s_routes[i].hops < bestHops)))) {
      best = i;
      bestOnline = online;
      bestScore = trust;
      bestHops = s_routes[i].hops;
    }
  }
  return best;
}

static int findFreeRouteSlot() {
  uint32_t now = millis();
  for (int i = 0; i < ROUTING_MAX_ROUTES; i++) {
    if (!s_routes[i].used) return i;
    if ((now - s_routes[i].timestamp) >= ROUTING_ROUTE_TTL_MS) {
      s_routes[i].used = false;
      return i;
    }
  }
  int oldest = 0;
  uint32_t oldestMs = s_routes[0].timestamp;
  for (int i = 1; i < ROUTING_MAX_ROUTES; i++) {
    if (s_routes[i].timestamp < oldestMs) {
      oldestMs = s_routes[i].timestamp;
      oldest = i;
    }
  }
  return oldest;
}

bool getNextHop(const uint8_t* dest, uint8_t* nextHopOut) {
  if (!dest || !nextHopOut) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return false;
  int idx = findBestRoute(dest);
  bool ok = (idx >= 0);
  if (ok) memcpy(nextHopOut, s_routes[idx].nextHop, protocol::NODE_ID_LEN);
  xSemaphoreGive(s_mutex);
  return ok;
}

bool getRoute(const uint8_t* dest, uint8_t* nextHopOut, uint8_t* hopsOut, int8_t* rssiOut, int* trustScoreOut) {
  if (!dest || !nextHopOut) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return false;
  int idx = findBestRoute(dest);
  bool ok = (idx >= 0);
  if (ok) {
    memcpy(nextHopOut, s_routes[idx].nextHop, protocol::NODE_ID_LEN);
    if (hopsOut) *hopsOut = s_routes[idx].hops;
    if (rssiOut) *rssiOut = s_routes[idx].rssi;
    if (trustScoreOut) *trustScoreOut = computeTrustScore(s_routes[idx].nextHop, s_routes[idx].rssi);
  }
  xSemaphoreGive(s_mutex);
  return ok;
}

int getRouteCount() {
  if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return 0;
  uint32_t now = millis();
  int n = 0;
  for (int i = 0; i < ROUTING_MAX_ROUTES; i++) {
    if (s_routes[i].used && (now - s_routes[i].timestamp) < ROUTING_ROUTE_TTL_MS) n++;
  }
  xSemaphoreGive(s_mutex);
  return n;
}

bool getRouteAt(int i, uint8_t* destOut, uint8_t* nextHopOut, uint8_t* hopsOut, int8_t* rssiOut, int* trustScoreOut) {
  if (i < 0 || !destOut || !nextHopOut) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return false;
  uint32_t now = millis();
  int idx = 0;
  for (int j = 0; j < ROUTING_MAX_ROUTES; j++) {
    if (!s_routes[j].used || (now - s_routes[j].timestamp) >= ROUTING_ROUTE_TTL_MS) continue;
    if (idx == i) {
      memcpy(destOut, s_routes[j].dest, protocol::NODE_ID_LEN);
      memcpy(nextHopOut, s_routes[j].nextHop, protocol::NODE_ID_LEN);
      if (hopsOut) *hopsOut = s_routes[j].hops;
      if (rssiOut) *rssiOut = s_routes[j].rssi;
      if (trustScoreOut) *trustScoreOut = computeTrustScore(s_routes[j].nextHop, s_routes[j].rssi);
      xSemaphoreGive(s_mutex);
      return true;
    }
    idx++;
  }
  xSemaphoreGive(s_mutex);
  return false;
}

static void addRoute(const uint8_t* dest, const uint8_t* nextHop, uint8_t hops, int8_t rssi) {
  int idx = findRouteByDestNextHop(dest, nextHop);
  if (idx >= 0) {
    RouteEntry& e = s_routes[idx];
    int newScore = computeTrustScore(nextHop, rssi);
    int oldScore = computeTrustScore(e.nextHop, e.rssi);
    // Обновляем существующий кандидат только если путь не стал заметно хуже.
    if (hops > e.hops + 1 && newScore + 8 < oldScore) return;
  } else {
    idx = findFreeRouteSlot();
  }
  memcpy(s_routes[idx].dest, dest, protocol::NODE_ID_LEN);
  memcpy(s_routes[idx].nextHop, nextHop, protocol::NODE_ID_LEN);
  s_routes[idx].hops = hops;
  s_routes[idx].rssi = rssi;
  s_routes[idx].timestamp = millis();
  s_routes[idx].used = true;
}

static int findReverse(const uint8_t* originator, uint32_t reqId) {
  uint32_t now = millis();
  for (int i = 0; i < ROUTING_MAX_REVERSE; i++) {
    if (s_reverse[i].used && memcmp(s_reverse[i].originator, originator, protocol::NODE_ID_LEN) == 0 &&
        s_reverse[i].reqId == reqId) {
      if ((now - s_reverse[i].timestamp) < ROUTING_REVERSE_TTL_MS) return i;
      s_reverse[i].used = false;
      return -1;
    }
  }
  return -1;
}

static int findFreeReverseSlot() {
  uint32_t now = millis();
  for (int i = 0; i < ROUTING_MAX_REVERSE; i++) {
    if (!s_reverse[i].used) return i;
    if ((now - s_reverse[i].timestamp) >= ROUTING_REVERSE_TTL_MS) {
      s_reverse[i].used = false;
      return i;
    }
  }
  return 0;
}

static void addReverse(const uint8_t* originator, uint32_t reqId, const uint8_t* prevHop) {
  int idx = findReverse(originator, reqId);
  if (idx >= 0) return;
  idx = findFreeReverseSlot();
  memcpy(s_reverse[idx].originator, originator, protocol::NODE_ID_LEN);
  s_reverse[idx].reqId = reqId;
  memcpy(s_reverse[idx].prevHop, prevHop, protocol::NODE_ID_LEN);
  s_reverse[idx].timestamp = millis();
  s_reverse[idx].used = true;
}

static bool wasSeen(const uint8_t* originator, uint32_t reqId, const uint8_t* target) {
  uint32_t now = millis();
  for (int i = 0; i < ROUTING_MAX_SEEN; i++) {
    if (s_seen[i].used && memcmp(s_seen[i].originator, originator, protocol::NODE_ID_LEN) == 0 &&
        s_seen[i].reqId == reqId && memcmp(s_seen[i].target, target, protocol::NODE_ID_LEN) == 0) {
      if ((now - s_seen[i].timestamp) < ROUTING_SEEN_TTL_MS) return true;
      s_seen[i].used = false;
      return false;
    }
  }
  return false;
}

static void addSeen(const uint8_t* originator, uint32_t reqId, const uint8_t* target) {
  uint32_t now = millis();
  int freeIdx = -1;
  for (int i = 0; i < ROUTING_MAX_SEEN; i++) {
    if (!s_seen[i].used) { freeIdx = i; break; }
    if ((now - s_seen[i].timestamp) >= ROUTING_SEEN_TTL_MS) { freeIdx = i; break; }
  }
  if (freeIdx < 0) {
    freeIdx = 0;
    uint32_t oldest = s_seen[0].timestamp;
    for (int i = 1; i < ROUTING_MAX_SEEN; i++) {
      if (s_seen[i].timestamp < oldest) { oldest = s_seen[i].timestamp; freeIdx = i; }
    }
  }
  memcpy(s_seen[freeIdx].originator, originator, protocol::NODE_ID_LEN);
  s_seen[freeIdx].reqId = reqId;
  memcpy(s_seen[freeIdx].target, target, protocol::NODE_ID_LEN);
  s_seen[freeIdx].timestamp = now;
  s_seen[freeIdx].used = true;
}

void requestRoute(const uint8_t* target) {
  if (!target || node::isForMe(target)) return;
  if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;

  uint8_t payload[ROUTE_PAYLOAD_LEN];
  memcpy(payload, target, protocol::NODE_ID_LEN);
  uint32_t reqId = ++s_reqIdCounter;
  memcpy(payload + 8, &reqId, 4);
  payload[12] = 0;
  memcpy(payload + 13, node::getId(), protocol::NODE_ID_LEN);

  uint8_t pkt[protocol::PAYLOAD_OFFSET + ROUTE_PAYLOAD_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_ROUTE_REQ,
      payload, ROUTE_PAYLOAD_LEN);
  if (len > 0) {
    uint8_t txSf = neighbors::rssiToSf(neighbors::getMinRssi());
    char reasonBuf[40];
    if (!queueTxPacket(pkt, len, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
      queueDeferredSend(pkt, len, txSf, 60, true);
      RIFTLINK_DIAG("ROUTE", "event=ROUTE_TX_DEFER type=req to=%02X%02X cause=%s",
          target[0], target[1], reasonBuf[0] ? reasonBuf : "?");
    } else {
      RIFTLINK_DIAG("ROUTE", "event=ROUTE_TX_QUEUED type=req to=%02X%02X reqId=%u sf=%u",
          target[0], target[1], (unsigned)reqId, (unsigned)txSf);
    }
    Serial.printf("[RiftLink] ROUTE_REQ to %02X%02X reqId=%u\n", target[0], target[1], (unsigned)reqId);
  }
  xSemaphoreGive(s_mutex);
}

bool onRouteReq(const uint8_t* from, const uint8_t* payload, size_t payloadLen) {
  if (payloadLen < ROUTE_PAYLOAD_LEN) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return false;

  const uint8_t* target = payload;
  uint32_t reqId;
  memcpy(&reqId, payload + 8, 4);
  uint8_t hops = payload[12];
  const uint8_t* sender = payload + 13;

  if (wasSeen(from, reqId, target)) {
    xSemaphoreGive(s_mutex);
    return true;
  }

  addSeen(from, reqId, target);

  if (node::isForMe(target)) {
    uint8_t replyPayload[ROUTE_PAYLOAD_LEN];
    memcpy(replyPayload, target, protocol::NODE_ID_LEN);
    memcpy(replyPayload + 8, &reqId, 4);
    replyPayload[12] = hops + 1;
    memcpy(replyPayload + 13, from, protocol::NODE_ID_LEN);  // originator для обратного пути

    uint8_t pkt[protocol::PAYLOAD_OFFSET + ROUTE_PAYLOAD_LEN];
    size_t len = protocol::buildPacket(pkt, sizeof(pkt),
        node::getId(), sender, 31, protocol::OP_ROUTE_REPLY,
        replyPayload, ROUTE_PAYLOAD_LEN);
    if (len > 0) {
      uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(sender));
      char reasonBuf[40];
      if (!queueTxPacket(pkt, len, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
        queueDeferredSend(pkt, len, txSf, 60, true);
        RIFTLINK_DIAG("ROUTE", "event=ROUTE_TX_DEFER type=reply target=me to=%02X%02X cause=%s",
            sender[0], sender[1], reasonBuf[0] ? reasonBuf : "?");
      }
      Serial.printf("[RiftLink] ROUTE_REPLY to %02X%02X (target=me)\n", sender[0], sender[1]);
    }
    xSemaphoreGive(s_mutex);
    return true;
  }

  addReverse(from, reqId, sender);

  uint8_t fwdPayload[ROUTE_PAYLOAD_LEN];
  memcpy(fwdPayload, payload, ROUTE_PAYLOAD_LEN);
  fwdPayload[12] = hops + 1;
  memcpy(fwdPayload + 13, node::getId(), protocol::NODE_ID_LEN);

  uint8_t pkt[protocol::PAYLOAD_OFFSET + ROUTE_PAYLOAD_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31 - (hops + 1), protocol::OP_ROUTE_REQ,
      fwdPayload, ROUTE_PAYLOAD_LEN);
  if (len > 0) {
    uint8_t txSf = neighbors::rssiToSf(neighbors::getMinRssi());
    char reasonBuf[40];
    if (!queueTxPacket(pkt, len, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
      queueDeferredSend(pkt, len, txSf, 70, true);
      RIFTLINK_DIAG("ROUTE", "event=ROUTE_TX_DEFER type=req_relay hops=%u cause=%s",
          (unsigned)(hops + 1), reasonBuf[0] ? reasonBuf : "?");
    }
    Serial.printf("[RiftLink] ROUTE_REQ relay hops=%u\n", (unsigned)(hops + 1));
  }
  xSemaphoreGive(s_mutex);
  return true;
}

bool onRouteReply(const uint8_t* from, const uint8_t* to, const uint8_t* payload, size_t payloadLen) {
  if (payloadLen < ROUTE_PAYLOAD_LEN) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return false;

  const uint8_t* target = payload;
  uint32_t reqId;
  memcpy(&reqId, payload + 8, 4);
  uint8_t hops = payload[12];
  const uint8_t* originator = payload + 13;

  if (node::isForMe(to)) {
    int rssi = radio::getLastRssi();
    int8_t rssi8 = (rssi >= -128 && rssi <= 0) ? (int8_t)rssi : 0;
    addRoute(target, from, hops, rssi8);
    Serial.printf("[RiftLink] ROUTE_REPLY: route to %02X%02X via %02X%02X (%u hops, rssi=%d)\n",
        target[0], target[1], from[0], from[1], (unsigned)hops, rssi);
    xSemaphoreGive(s_mutex);
    return true;
  }

  int revIdx = findReverse(originator, reqId);
  if (revIdx < 0) {
    xSemaphoreGive(s_mutex);
    return true;
  }

  uint8_t pkt[protocol::PAYLOAD_OFFSET + ROUTE_PAYLOAD_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), s_reverse[revIdx].prevHop, 31, protocol::OP_ROUTE_REPLY,
      payload, payloadLen);
  if (len > 0) {
    uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(s_reverse[revIdx].prevHop));
    char reasonBuf[40];
    if (!queueTxPacket(pkt, len, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
      queueDeferredSend(pkt, len, txSf, 60, true);
      RIFTLINK_DIAG("ROUTE", "event=ROUTE_TX_DEFER type=reply_relay to=%02X%02X cause=%s",
          s_reverse[revIdx].prevHop[0], s_reverse[revIdx].prevHop[1], reasonBuf[0] ? reasonBuf : "?");
    }
    Serial.printf("[RiftLink] ROUTE_REPLY relay to %02X%02X\n", s_reverse[revIdx].prevHop[0], s_reverse[revIdx].prevHop[1]);
  }
  s_reverse[revIdx].used = false;
  xSemaphoreGive(s_mutex);
  return true;
}

void update() {
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  uint32_t now = millis();
  for (int i = 0; i < ROUTING_MAX_ROUTES; i++) {
    if (s_routes[i].used && (now - s_routes[i].timestamp) >= ROUTING_ROUTE_TTL_MS) {
      s_routes[i].used = false;
    }
  }
  for (int i = 0; i < ROUTING_MAX_REVERSE; i++) {
    if (s_reverse[i].used && (now - s_reverse[i].timestamp) >= ROUTING_REVERSE_TTL_MS) {
      s_reverse[i].used = false;
    }
  }
  xSemaphoreGive(s_mutex);
}

}  // namespace routing
