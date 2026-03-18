/**
 * RiftLink Routing — ROUTE_REQ/REPLY
 */

#include "routing.h"
#include "node/node.h"
#include "radio/radio.h"
#include <Arduino.h>
#include <string.h>

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

namespace routing {

void init() {
  if (s_inited) return;
  memset(s_routes, 0, sizeof(s_routes));
  memset(s_reverse, 0, sizeof(s_reverse));
  memset(s_seen, 0, sizeof(s_seen));
  s_inited = true;
}

static int findRoute(const uint8_t* dest) {
  uint32_t now = millis();
  for (int i = 0; i < ROUTING_MAX_ROUTES; i++) {
    if (s_routes[i].used && memcmp(s_routes[i].dest, dest, protocol::NODE_ID_LEN) == 0) {
      if ((now - s_routes[i].timestamp) < ROUTING_ROUTE_TTL_MS) return i;
      s_routes[i].used = false;
      return -1;
    }
  }
  return -1;
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
  int idx = findRoute(dest);
  if (idx < 0) return false;
  memcpy(nextHopOut, s_routes[idx].nextHop, protocol::NODE_ID_LEN);
  return true;
}

bool getRoute(const uint8_t* dest, uint8_t* nextHopOut, uint8_t* hopsOut, int8_t* rssiOut) {
  if (!dest || !nextHopOut) return false;
  int idx = findRoute(dest);
  if (idx < 0) return false;
  memcpy(nextHopOut, s_routes[idx].nextHop, protocol::NODE_ID_LEN);
  if (hopsOut) *hopsOut = s_routes[idx].hops;
  if (rssiOut) *rssiOut = s_routes[idx].rssi;
  return true;
}

int getRouteCount() {
  uint32_t now = millis();
  int n = 0;
  for (int i = 0; i < ROUTING_MAX_ROUTES; i++) {
    if (s_routes[i].used && (now - s_routes[i].timestamp) < ROUTING_ROUTE_TTL_MS) n++;
  }
  return n;
}

bool getRouteAt(int i, uint8_t* destOut, uint8_t* nextHopOut, uint8_t* hopsOut, int8_t* rssiOut) {
  if (i < 0 || !destOut || !nextHopOut) return false;
  uint32_t now = millis();
  int idx = 0;
  for (int j = 0; j < ROUTING_MAX_ROUTES; j++) {
    if (!s_routes[j].used || (now - s_routes[j].timestamp) >= ROUTING_ROUTE_TTL_MS) continue;
    if (idx == i) {
      memcpy(destOut, s_routes[j].dest, protocol::NODE_ID_LEN);
      memcpy(nextHopOut, s_routes[j].nextHop, protocol::NODE_ID_LEN);
      if (hopsOut) *hopsOut = s_routes[j].hops;
      if (rssiOut) *rssiOut = s_routes[j].rssi;
      return true;
    }
    idx++;
  }
  return false;
}

static void addRoute(const uint8_t* dest, const uint8_t* nextHop, uint8_t hops, int8_t rssi) {
  int idx = findRoute(dest);
  if (idx >= 0) {
    RouteEntry& e = s_routes[idx];
    if (hops > e.hops) return;
    if (hops == e.hops && rssi < e.rssi && e.rssi != 0) return;
  }
  if (idx < 0) idx = findFreeRouteSlot();
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

  uint8_t payload[ROUTE_PAYLOAD_LEN];
  memcpy(payload, target, protocol::NODE_ID_LEN);
  uint32_t reqId = ++s_reqIdCounter;
  memcpy(payload + 8, &reqId, 4);
  payload[12] = 0;
  memcpy(payload + 13, node::getId(), protocol::NODE_ID_LEN);

  uint8_t pkt[protocol::HEADER_LEN + ROUTE_PAYLOAD_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_ROUTE_REQ,
      payload, ROUTE_PAYLOAD_LEN);
  if (len > 0) {
    radio::send(pkt, len);
    Serial.printf("[RiftLink] ROUTE_REQ to %02X%02X reqId=%u\n", target[0], target[1], (unsigned)reqId);
  }
}

bool onRouteReq(const uint8_t* from, const uint8_t* payload, size_t payloadLen) {
  if (payloadLen < ROUTE_PAYLOAD_LEN) return false;

  const uint8_t* target = payload;
  uint32_t reqId;
  memcpy(&reqId, payload + 8, 4);
  uint8_t hops = payload[12];
  const uint8_t* sender = payload + 13;

  if (wasSeen(from, reqId, target)) return true;

  addSeen(from, reqId, target);

  if (node::isForMe(target)) {
    uint8_t replyPayload[ROUTE_PAYLOAD_LEN];
    memcpy(replyPayload, target, protocol::NODE_ID_LEN);
    memcpy(replyPayload + 8, &reqId, 4);
    replyPayload[12] = hops + 1;
    memcpy(replyPayload + 13, from, protocol::NODE_ID_LEN);  // originator для обратного пути

    uint8_t pkt[protocol::HEADER_LEN + ROUTE_PAYLOAD_LEN];
    size_t len = protocol::buildPacket(pkt, sizeof(pkt),
        node::getId(), sender, 31, protocol::OP_ROUTE_REPLY,
        replyPayload, ROUTE_PAYLOAD_LEN);
    if (len > 0) {
      radio::send(pkt, len);
      Serial.printf("[RiftLink] ROUTE_REPLY to %02X%02X (target=me)\n", sender[0], sender[1]);
    }
    return true;
  }

  addReverse(from, reqId, sender);

  uint8_t fwdPayload[ROUTE_PAYLOAD_LEN];
  memcpy(fwdPayload, payload, ROUTE_PAYLOAD_LEN);
  fwdPayload[12] = hops + 1;
  memcpy(fwdPayload + 13, node::getId(), protocol::NODE_ID_LEN);

  uint8_t pkt[protocol::HEADER_LEN + ROUTE_PAYLOAD_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31 - (hops + 1), protocol::OP_ROUTE_REQ,
      fwdPayload, ROUTE_PAYLOAD_LEN);
  if (len > 0) {
    radio::send(pkt, len);
    Serial.printf("[RiftLink] ROUTE_REQ relay hops=%u\n", (unsigned)(hops + 1));
  }
  return true;
}

bool onRouteReply(const uint8_t* from, const uint8_t* to, const uint8_t* payload, size_t payloadLen) {
  if (payloadLen < ROUTE_PAYLOAD_LEN) return false;

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
    return true;
  }

  int revIdx = findReverse(originator, reqId);
  if (revIdx < 0) return true;

  uint8_t pkt[protocol::HEADER_LEN + ROUTE_PAYLOAD_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), s_reverse[revIdx].prevHop, 31, protocol::OP_ROUTE_REPLY,
      payload, payloadLen);
  if (len > 0) {
    radio::send(pkt, len);
    Serial.printf("[RiftLink] ROUTE_REPLY relay to %02X%02X\n", s_reverse[revIdx].prevHop[0], s_reverse[revIdx].prevHop[1]);
  }
  s_reverse[revIdx].used = false;
  return true;
}

void update() {
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
}

}  // namespace routing
