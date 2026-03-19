/**
 * Packet Cache — кэш для NACK retransmit
 */

#include "pkt_cache.h"
#include "node/node.h"
#include "radio/radio.h"
#include "neighbors/neighbors.h"
#include "async_queues.h"
#include <string.h>

#define CACHE_SIZE 8

struct CachedPkt {
  uint8_t to[protocol::NODE_ID_LEN];
  uint16_t pktId;
  uint8_t pkt[PACKET_BUF_SIZE];
  uint16_t len;
  bool inUse;
};

static CachedPkt s_cache[CACHE_SIZE];
static bool s_inited = false;

namespace pkt_cache {

void init() {
  if (s_inited) return;
  for (int i = 0; i < CACHE_SIZE; i++) s_cache[i].inUse = false;
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

}  // namespace pkt_cache
