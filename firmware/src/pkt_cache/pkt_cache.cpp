/**
 * Packet Cache — кэш для NACK retransmit
 */

#include "pkt_cache.h"
#include "node/node.h"
#include "radio/radio.h"
#include "neighbors/neighbors.h"
#include "async_queues.h"
#include "log.h"
#include <string.h>

#define CACHE_SIZE 4
#define OVERHEAR_SIZE 2
#define BATCH_CACHE_SIZE 1
#define NACK_GUARD_SIZE 8
#define NACK_GUARD_MS 800

struct CachedPkt {
  uint8_t to[protocol::NODE_ID_LEN];
  uint16_t pktId;
  uint8_t pkt[PACKET_BUF_SIZE];
  uint16_t len;
  bool inUse;
};

struct OverhearEntry {
  uint8_t from[protocol::NODE_ID_LEN];
  uint8_t to[protocol::NODE_ID_LEN];
  uint16_t pktId;
  uint8_t pkt[PACKET_BUF_SIZE];
  uint16_t len;
  bool inUse;
};

struct BatchEntry {
  uint8_t to[protocol::NODE_ID_LEN];
  uint16_t pktIds[4];
  int count;
  uint8_t pkt[PACKET_BUF_SIZE];
  uint16_t len;
  bool inUse;
};

static CachedPkt s_cache[CACHE_SIZE];
static OverhearEntry s_overhear[OVERHEAR_SIZE];
static BatchEntry s_batchCache[BATCH_CACHE_SIZE];
static bool s_inited = false;
struct NackGuardEntry {
  uint8_t peer[protocol::NODE_ID_LEN];
  uint16_t pktId;
  uint32_t ts;
};
static NackGuardEntry s_nackGuard[NACK_GUARD_SIZE];

static bool nackRateLimitHit(const uint8_t* peer, uint16_t pktId) {
  uint32_t now = millis();
  int freeIdx = -1;
  int oldestIdx = 0;
  uint32_t oldestTs = s_nackGuard[0].ts;
  for (int i = 0; i < NACK_GUARD_SIZE; i++) {
    if (s_nackGuard[i].ts == 0) {
      if (freeIdx < 0) freeIdx = i;
      continue;
    }
    if ((now - s_nackGuard[i].ts) > NACK_GUARD_MS) {
      freeIdx = i;
      break;
    }
    if (memcmp(s_nackGuard[i].peer, peer, protocol::NODE_ID_LEN) == 0 &&
        s_nackGuard[i].pktId == pktId) {
      return true;
    }
    if (s_nackGuard[i].ts < oldestTs) {
      oldestTs = s_nackGuard[i].ts;
      oldestIdx = i;
    }
  }
  int idx = (freeIdx >= 0) ? freeIdx : oldestIdx;
  memcpy(s_nackGuard[idx].peer, peer, protocol::NODE_ID_LEN);
  s_nackGuard[idx].pktId = pktId;
  s_nackGuard[idx].ts = now;
  return false;
}

namespace pkt_cache {

void init() {
  if (s_inited) return;
  for (int i = 0; i < CACHE_SIZE; i++) s_cache[i].inUse = false;
  for (int i = 0; i < OVERHEAR_SIZE; i++) s_overhear[i].inUse = false;
  for (int i = 0; i < BATCH_CACHE_SIZE; i++) s_batchCache[i].inUse = false;
  memset(s_nackGuard, 0, sizeof(s_nackGuard));
  s_inited = true;
}

void add(const uint8_t* to, uint16_t pktId, const uint8_t* pkt, size_t len) {
  if (!s_inited || !to || !pkt || len > PACKET_BUF_SIZE) return;
  // Ищем свободный слот или перезаписываем самый старый
  int idx = -1;
  for (int i = 0; i < CACHE_SIZE; i++) {
    if (!s_cache[i].inUse) { idx = i; break; }
    if (memcmp(s_cache[i].to, to, protocol::NODE_ID_LEN) == 0 && s_cache[i].pktId == pktId) {
      idx = i; break;  // обновить существующий
    }
  }
  if (idx < 0) idx = 0;  // перезаписать первый
  memcpy(s_cache[idx].to, to, protocol::NODE_ID_LEN);
  s_cache[idx].pktId = pktId;
  memcpy(s_cache[idx].pkt, pkt, len);
  s_cache[idx].len = (uint16_t)len;
  s_cache[idx].inUse = true;
}

bool retransmitOnNack(const uint8_t* from, uint16_t pktId) {
  if (!s_inited || !from) return false;
  if (node::isInvalidNodeId(from) || node::isBroadcast(from) || node::isForMe(from)) return false;
  if (!neighbors::isOnline(from)) return false;
  if (nackRateLimitHit(from, pktId)) {
    RIFTLINK_DIAG("NACK", "event=NACK_REJECT reason=rate_limit from=%02X%02X pktId=%u",
        from[0], from[1], (unsigned)pktId);
    return false;
  }
  radio::notifyCongestion();  // NACK = коллизия/потеря — увеличить BEB CW
  if (retransmitBatchOnNack(from, pktId)) return true;
  for (int i = 0; i < CACHE_SIZE; i++) {
    if (!s_cache[i].inUse) continue;
    if (memcmp(s_cache[i].to, from, protocol::NODE_ID_LEN) != 0) continue;
    if (s_cache[i].pktId != pktId) continue;
    uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(from));
    bool ok = radio::send(s_cache[i].pkt, s_cache[i].len, txSf, true);  // priority
    s_cache[i].inUse = false;
    return ok;
  }
  return false;
}

void addOverheard(const uint8_t* from, const uint8_t* to, uint16_t pktId, const uint8_t* pkt, size_t len) {
  if (!s_inited || !from || !to || !pkt || len > PACKET_BUF_SIZE || pktId == 0) return;
  int idx = -1;
  for (int i = 0; i < OVERHEAR_SIZE; i++) {
    if (!s_overhear[i].inUse) { idx = i; break; }
    if (memcmp(s_overhear[i].from, from, protocol::NODE_ID_LEN) == 0 &&
        memcmp(s_overhear[i].to, to, protocol::NODE_ID_LEN) == 0 && s_overhear[i].pktId == pktId) {
      idx = i; break;
    }
  }
  if (idx < 0) idx = 0;
  memcpy(s_overhear[idx].from, from, protocol::NODE_ID_LEN);
  memcpy(s_overhear[idx].to, to, protocol::NODE_ID_LEN);
  s_overhear[idx].pktId = pktId;
  memcpy(s_overhear[idx].pkt, pkt, len);
  s_overhear[idx].len = (uint16_t)len;
  s_overhear[idx].inUse = true;
}

bool retransmitOverheard(const uint8_t* nackFrom, const uint8_t* nackTo, uint16_t pktId) {
  if (!s_inited || !nackFrom || !nackTo || pktId == 0) return false;
  if (!neighbors::isOnline(nackFrom)) return false;
  if (node::isInvalidNodeId(nackTo) || node::isBroadcast(nackTo) || node::isForMe(nackTo)) return false;
  if (nackRateLimitHit(nackFrom, pktId)) {
    RIFTLINK_DIAG("NACK", "event=NACK_REJECT reason=overhear_rate_limit from=%02X%02X pktId=%u",
        nackFrom[0], nackFrom[1], (unsigned)pktId);
    return false;
  }
  for (int i = 0; i < OVERHEAR_SIZE; i++) {
    if (!s_overhear[i].inUse) continue;
    if (memcmp(s_overhear[i].from, nackTo, protocol::NODE_ID_LEN) != 0) continue;
    if (memcmp(s_overhear[i].to, nackFrom, protocol::NODE_ID_LEN) != 0) continue;
    if (s_overhear[i].pktId != pktId) continue;
    uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(nackFrom));
    if (txSf == 0) txSf = 12;
    bool ok = radio::send(s_overhear[i].pkt, s_overhear[i].len, txSf, true);
    s_overhear[i].inUse = false;
    return ok;
  }
  return false;
}

void addBatch(const uint8_t* to, const uint16_t* pktIds, int count, const uint8_t* pkt, size_t len) {
  if (!s_inited || !to || !pktIds || count < 1 || count > 4 || !pkt || len > PACKET_BUF_SIZE) return;
  int idx = 0;
  for (int i = 0; i < BATCH_CACHE_SIZE; i++) {
    if (!s_batchCache[i].inUse) { idx = i; break; }
  }
  memcpy(s_batchCache[idx].to, to, protocol::NODE_ID_LEN);
  for (int i = 0; i < count; i++) s_batchCache[idx].pktIds[i] = pktIds[i];
  s_batchCache[idx].count = count;
  memcpy(s_batchCache[idx].pkt, pkt, len);
  s_batchCache[idx].len = (uint16_t)len;
  s_batchCache[idx].inUse = true;
}

bool retransmitBatchOnNack(const uint8_t* from, uint16_t pktId) {
  if (!s_inited || !from) return false;
  for (int i = 0; i < BATCH_CACHE_SIZE; i++) {
    if (!s_batchCache[i].inUse) continue;
    if (memcmp(s_batchCache[i].to, from, protocol::NODE_ID_LEN) != 0) continue;
    for (int j = 0; j < s_batchCache[i].count; j++) {
      if (s_batchCache[i].pktIds[j] == pktId) {
        uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(from));
        if (txSf == 0) txSf = 12;
        bool ok = radio::send(s_batchCache[i].pkt, s_batchCache[i].len, txSf, true);
        return ok;
      }
    }
  }
  return false;
}

}  // namespace pkt_cache
