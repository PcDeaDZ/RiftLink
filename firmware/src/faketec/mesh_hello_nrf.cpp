/**
 * HELLO / POLL / handshake quiet — порт логики из firmware/src/main.cpp для nRF (без radioSchedulerTask).
 */

#include "mesh_hello_nrf.h"

#include "async_tasks.h"
#include "beacon_sync/beacon_sync.h"
#include "log.h"
#include "neighbors/neighbors.h"
#include "node/node.h"
#include "protocol/packet.h"
#include "radio/radio.h"
#include "x25519_keys/x25519_keys.h"
#include "nrf_wdt_feed.h"

#include <Arduino.h>
#include <atomic>
#include <cstring>

extern "C" __attribute__((weak)) size_t xPortGetFreeHeapSize(void) {
  return 0U;
}

namespace {

#define HELLO_INTERVAL_MS 36000
#define HELLO_INTERVAL_AGGRESSIVE_MS 12000
#define HELLO_INTERVAL_ONE_NEIGH_MS 24000
#define HELLO_JITTER_MS 3000
#define HELLO_JITTER_ZERO_MS 500
#define HELLO_HARD_MIN_INTERVAL_MS 10000
#define HELLO_DROP_RETRY_MS 350
#define HELLO_QUIET_AFTER_KEY_MS 2500
#define HANDSHAKE_TRAFFIC_QUIET_MS 3000

#define POLL_INTERVAL_MS 5000
#define POLL_JITTER_BASE_MS 20
#define POLL_JITTER_SPAN_MS 220

#define AUTO_POLL_ENABLED 1
#define AUTO_TELEMETRY_ENABLED 1

static const uint8_t SF_DEFAULT = 7;

static uint32_t s_lastHelloAirMs = 0;
static uint32_t s_nextHelloAfterDropMs = 0;
static uint32_t s_handshakeQuietUntilMs = 0;
static uint32_t s_lastObservedKeyTxMs = 0;
static std::atomic<bool> s_pendingDiscoveryHello{false};

static uint32_t s_nextHelloDueMs = 0;
static int s_helloPlannerLastN = -999;
static uint32_t s_zeroNeighSince = 0;
static uint32_t s_oneNeighSince = 0;

static uint32_t lastPoll = 0;

static inline uint32_t nrf_rand_u32() {
  return (uint32_t)random(0x7fffffff);
}

static inline bool is_handshake_quiet_active() {
  return (int32_t)(s_handshakeQuietUntilMs - millis()) > 0;
}

static inline uint8_t hello_tx_free_slots() {
  return asyncTxQueueFree();
}

static inline uint8_t hello_tx_waiting_slots() {
  return asyncTxQueueWaiting();
}

static uint8_t get_discovery_sf() {
  uint8_t sf = radio::getSpreadingFactor();
  if (sf < 7 || sf > 12) sf = SF_DEFAULT;
  return sf;
}

static uint16_t id_hash16(const uint8_t* id) {
  if (!id) return 0;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    h ^= id[i];
    h *= 16777619u;
  }
  return (uint16_t)((h >> 16) ^ (h & 0xFFFFu));
}

static uint32_t compute_poll_jitter_ms() {
  const uint8_t* self = node::getId();
  uint16_t sid = id_hash16(self);
  uint32_t salt = (uint32_t)(self[2] ^ self[5] ^ (sid & 0xFF));
  return POLL_JITTER_BASE_MS + (salt % POLL_JITTER_SPAN_MS);
}

struct HelloPlan {
  uint32_t intervalMs;
  uint32_t jitterMs;
  uint32_t phaseOffset;
};

static HelloPlan compute_hello_plan() {
  HelloPlan p;
  p.intervalMs = HELLO_INTERVAL_MS;
  p.jitterMs = HELLO_JITTER_MS;
  p.phaseOffset = 0;
  int nNeigh = neighbors::getCount();
  if (nNeigh == 0) {
    uint32_t zeroElapsed = (s_zeroNeighSince == 0) ? 0 : (millis() - s_zeroNeighSince);
    if (zeroElapsed >= 300000)
      p.intervalMs = 18000;
    else if (zeroElapsed >= 120000)
      p.intervalMs = 15000;
    else
      p.intervalMs = HELLO_INTERVAL_AGGRESSIVE_MS;
    p.jitterMs = HELLO_JITTER_ZERO_MS;
    p.phaseOffset = beacon_sync::getSlotFor(node::getId()) * 400u;
  } else if (nNeigh == 1) {
    p.intervalMs = HELLO_INTERVAL_ONE_NEIGH_MS;
    p.phaseOffset = beacon_sync::getSlotFor(node::getId()) * 240u;
  } else if (nNeigh >= 6) {
    p.intervalMs = 30000;
    p.phaseOffset = beacon_sync::getSlotFor(node::getId()) * 140u;
  } else {
    p.phaseOffset = beacon_sync::getSlotFor(node::getId()) * 180u;
  }
  return p;
}

static void arm_next_hello_deadline_after_successful_send() {
  HelloPlan pl = compute_hello_plan();
  int32_t jitter = (int32_t)(nrf_rand_u32() % (pl.jitterMs * 2)) - (int32_t)pl.jitterMs;
  int64_t due = (int64_t)millis() + (int64_t)pl.intervalMs + (int64_t)pl.phaseOffset + (int64_t)jitter;
  if (due < 0) due = 0;
  s_nextHelloDueMs = (uint32_t)due;
}

static bool hello_tx_reason_is_cad_defer(const char* sepReason) {
  return sepReason && (strstr(sepReason, "cad_defer") != nullptr || strstr(sepReason, "queue_defer") != nullptr);
}

static inline uint16_t hello_sender_tag16(const uint8_t* nodeId) {
  if (!nodeId) return 0;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    h ^= nodeId[i];
    h *= 16777619u;
  }
  return (uint16_t)((h >> 16) ^ (h & 0xFFFFu));
}

static bool send_hello() {
  uint32_t now = millis();
  int n = neighbors::getCount();
  uint32_t lastKeyTxMs = x25519_keys::getLastKeyTxReadyMs();
  if (lastKeyTxMs != 0 && (now - lastKeyTxMs) < HELLO_QUIET_AFTER_KEY_MS) {
    s_nextHelloDueMs = lastKeyTxMs + HELLO_QUIET_AFTER_KEY_MS;
    RIFTLINK_DIAG("HELLO", "event=HELLO_HOLD cause=recent_key quiet_ms=%lu",
        (unsigned long)(s_nextHelloDueMs - now));
    return false;
  }
  if (now < s_nextHelloAfterDropMs) {
    s_nextHelloDueMs = s_nextHelloAfterDropMs;
    return false;
  }
  if (s_lastHelloAirMs != 0 && (now - s_lastHelloAirMs) < HELLO_HARD_MIN_INTERVAL_MS) {
    s_nextHelloDueMs = s_lastHelloAirMs + HELLO_HARD_MIN_INTERVAL_MS;
    RIFTLINK_DIAG("HELLO", "event=HELLO_HOLD cause=hard_rate_limit quiet_ms=%lu",
        (unsigned long)(s_nextHelloDueMs - now));
    return false;
  }
  if (n > 1) {
    uint8_t txFree = hello_tx_free_slots();
    uint8_t txWaiting = hello_tx_waiting_slots();
    uint8_t congestion = radio::getCongestionLevel();
    bool tightQueue = (txFree <= 2);
    bool cadBusyBurst = (congestion >= 2);
    if (tightQueue && cadBusyBurst) {
      uint32_t quietMs = 220 + (nrf_rand_u32() % 220);
      s_nextHelloDueMs = now + quietMs;
      RIFTLINK_DIAG("HELLO", "event=HELLO_HOLD cause=tx_pressure_cad quiet_ms=%lu tx_free=%u tx_waiting=%u congestion=%u",
          (unsigned long)quietMs, (unsigned)txFree, (unsigned)txWaiting, (unsigned)congestion);
      return false;
    }
    if (txWaiting >= 3 && txFree <= 4) {
      uint32_t quietMs = 120 + (nrf_rand_u32() % 120);
      s_nextHelloDueMs = now + quietMs;
      RIFTLINK_DIAG("HELLO", "event=HELLO_HOLD cause=tx_pressure quiet_ms=%lu tx_free=%u tx_waiting=%u",
          (unsigned long)quietMs, (unsigned)txFree, (unsigned)txWaiting);
      return false;
    }
  }

  uint8_t helloPayload[2];
  uint16_t helloTag = hello_sender_tag16(node::getId());
  helloPayload[0] = (uint8_t)(helloTag & 0xFF);
  helloPayload[1] = (uint8_t)(helloTag >> 8);
  uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN + sizeof(helloPayload)];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_HELLO,
      helloPayload, sizeof(helloPayload));
  if (len == 0) return false;

  bool manyNeighbors = (n >= 6);
  bool priority = manyNeighbors || (n <= 1);
  char helloWhy[56] = {0};
  bool helloSent = false;
  uint8_t txSf = get_discovery_sf();
  if (queueTxPacket(pkt, len, txSf, priority, TxRequestClass::control, helloWhy, sizeof helloWhy)) {
    helloSent = true;
  } else {
    queueDeferredSend(pkt, len, txSf, 60, true);
    strncpy(helloWhy, "queue_defer", sizeof(helloWhy) - 1);
  }

  bool viaCadDefer = false;
  if (!helloSent && hello_tx_reason_is_cad_defer(helloWhy)) {
    helloSent = true;
    viaCadDefer = true;
  }
  const unsigned heapU = (unsigned)xPortGetFreeHeapSize();
  if (helloSent) {
    const char* tag = viaCadDefer ? " (deferred/CAD)" : "";
    RIFTLINK_DIAG("HELLO", "event=HELLO_TX ok=1 via=%s sf=%u neighbors=%d reason=%s heap=%u",
        viaCadDefer ? "cad_defer" : "direct", (unsigned)get_discovery_sf(), n, helloWhy[0] ? helloWhy : "-", heapU);
    Serial.printf("[RiftLink] HELLO sent%s n=%d heap=%u\n", tag, n, heapU);
  } else {
    RIFTLINK_DIAG("HELLO", "event=HELLO_TX ok=0 sf=%u neighbors=%d cause=%s heap=%u", (unsigned)get_discovery_sf(), n,
        helloWhy[0] ? helloWhy : "unknown", heapU);
    Serial.printf("[RiftLink] HELLO drop why=%s n=%d heap=%u\n", helloWhy[0] ? helloWhy : "unknown", n, heapU);
  }
  if (helloSent) {
    s_lastHelloAirMs = millis();
    s_nextHelloAfterDropMs = 0;
    return true;
  }
  s_nextHelloAfterDropMs = now + HELLO_DROP_RETRY_MS;
  return false;
}

static void send_poll() {
  uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN_BROADCAST];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_POLL, nullptr, 0);
  if (len == 0) return;
  uint8_t txSf = get_discovery_sf();
  uint32_t jitterMs = compute_poll_jitter_ms();
  queueDeferredSend(pkt, len, txSf, jitterMs);
  RIFTLINK_DIAG("HELLO", "event=POLL_TX_DEFER sf=%u delay_ms=%lu cause=scheduled_jitter", (unsigned)txSf,
      (unsigned long)jitterMs);
}

}  // namespace

void mesh_hello_nrf_init() {
  s_nextHelloDueMs = 0;
  lastPoll = millis();
}

bool mesh_hello_is_handshake_quiet_active() {
  return is_handshake_quiet_active();
}

void mesh_hello_extend_quiet(const char* cause, uint32_t durMs) {
  uint32_t now = millis();
  uint32_t until = now + durMs;
  if ((int32_t)(until - s_handshakeQuietUntilMs) > 0) {
    s_handshakeQuietUntilMs = until;
  }
  RIFTLINK_DIAG("HELLO", "event=HANDSHAKE_QUIET cause=%s quiet_ms=%lu", cause ? cause : "-",
      (unsigned long)(s_handshakeQuietUntilMs - now));
}

void mesh_hello_request_discovery() {
  s_pendingDiscoveryHello.store(true, std::memory_order_release);
}

void mesh_hello_nrf_loop() {
  if (!radio::isReady()) return;

  int nNeigh = neighbors::getCount();
  uint32_t keyTxMs = x25519_keys::getLastKeyTxReadyMs();
  if (keyTxMs != 0 && keyTxMs != s_lastObservedKeyTxMs) {
    s_lastObservedKeyTxMs = keyTxMs;
    uint32_t now = millis();
    uint32_t until = keyTxMs + HANDSHAKE_TRAFFIC_QUIET_MS;
    if ((int32_t)(until - s_handshakeQuietUntilMs) > 0) {
      s_handshakeQuietUntilMs = until;
      RIFTLINK_DIAG("HELLO", "event=HANDSHAKE_QUIET cause=key_tx quiet_ms=%lu",
          (unsigned long)(s_handshakeQuietUntilMs - now));
    }
  }

  if (nNeigh == 0) {
    if (s_zeroNeighSince == 0) s_zeroNeighSince = millis();
    s_oneNeighSince = 0;
  } else if (nNeigh == 1) {
    s_zeroNeighSince = 0;
    if (s_oneNeighSince == 0) s_oneNeighSince = millis();
  } else {
    s_zeroNeighSince = 0;
    s_oneNeighSince = 0;
  }
  if (nNeigh != s_helloPlannerLastN) s_helloPlannerLastN = nNeigh;

  if (s_pendingDiscoveryHello.exchange(false, std::memory_order_acq_rel)) {
    if (send_hello()) arm_next_hello_deadline_after_successful_send();
  } else if (!is_handshake_quiet_active() && (int32_t)(millis() - s_nextHelloDueMs) >= 0) {
    if (send_hello()) arm_next_hello_deadline_after_successful_send();
  }

#if AUTO_POLL_ENABLED
  if (!is_handshake_quiet_active() && nNeigh > 1 && (uint32_t)(millis() - lastPoll) > POLL_INTERVAL_MS) {
    send_poll();
    lastPoll = millis();
  }
#endif
  
  // Feed watchdog в конце цикла — иначе reset при длительных операциях
  riftlink_wdt_feed();
}
