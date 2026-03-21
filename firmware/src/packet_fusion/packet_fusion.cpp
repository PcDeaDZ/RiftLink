/**
 * Packet Fusion — батч 2–4 MSG для одного получателя
 */

#include "packet_fusion.h"
#include "node/node.h"
#include "crypto/crypto.h"
#include "compress/compress.h"
#include "radio/radio.h"
#include "neighbors/neighbors.h"
#include "pkt_cache/pkt_cache.h"
#include "bls_n/bls_n.h"
#include "esp_now_slots/esp_now_slots.h"
#include "async_tasks.h"
#include <string.h>

#define FUSION_BUF_SIZE 4
struct FusionEntry {
  uint8_t to[protocol::NODE_ID_LEN];
  uint8_t plain[4][120];  // ttl?+msgId+text
  size_t plainLen[4];
  uint32_t msgId[4];
  bool compressed[4];
  uint16_t pktId[4];
  uint8_t count;
  uint32_t firstAddTime;
  bool inUse;
};

#define FUSION_SLOTS 1
static FusionEntry s_fusion[FUSION_SLOTS];
static bool s_inited = false;
static void (*s_onBatchSent)(const uint8_t* to, const uint32_t* msgIds, int count) = nullptr;
static bool (*s_onSingleFlush)(const uint8_t* to, uint32_t msgId, const uint8_t* pkt, size_t pktLen, uint8_t txSf) = nullptr;

namespace packet_fusion {

void init() {
  memset(s_fusion, 0, sizeof(s_fusion));
  s_inited = true;
}

bool offer(const uint8_t* to, const uint8_t* plainBuf, size_t plainLen,
    uint32_t msgId, bool useCompressed) {
  if (!s_inited || !to || !plainBuf || plainLen > 110) return false;
  if (node::isBroadcast(to)) return false;

  uint32_t now = millis();
  int slot = -1;
  for (int i = 0; i < FUSION_SLOTS; i++) {
    if (!s_fusion[i].inUse) {
      slot = i;
      break;
    }
    if (memcmp(s_fusion[i].to, to, protocol::NODE_ID_LEN) == 0 &&
        s_fusion[i].count < MAX_BATCH &&
        (now - s_fusion[i].firstAddTime) < BATCH_WINDOW_MS) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return false;

  FusionEntry* e = &s_fusion[slot];
  if (!e->inUse) {
    memcpy(e->to, to, protocol::NODE_ID_LEN);
    e->count = 0;
    e->firstAddTime = now;
    e->inUse = true;
  }

  int idx = e->count++;
  memcpy(e->plain[idx], plainBuf, plainLen);
  e->plainLen[idx] = plainLen;
  e->msgId[idx] = msgId;
  e->compressed[idx] = useCompressed;
  e->pktId[idx] = (uint16_t)(msgId & 0xFFFF);
  return true;
}

static void flushEntry(FusionEntry* e) {
  if (e->count == 0) return;

  uint8_t txSf = neighbors::rssiToSfOrthogonal(e->to);
  if (txSf == 0) txSf = 12;

  if (e->count == 1) {
    uint8_t encBuf[protocol::MAX_PAYLOAD + crypto::OVERHEAD];
    size_t encLen = sizeof(encBuf);
    if (!crypto::encryptFor(e->to, e->plain[0], e->plainLen[0], encBuf, &encLen)) return;
    uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
    size_t pktLen = protocol::buildPacket(pkt, sizeof(pkt),
        node::getId(), e->to, 31, protocol::OP_MSG,
        encBuf, encLen, true, true, e->compressed[0], protocol::CHANNEL_DEFAULT, e->pktId[0]);
    if (pktLen > 0) {
      pkt_cache::add(e->to, e->pktId[0], pkt, pktLen);
      if (bls_n::shouldDeferTx(e->to) || esp_now_slots::shouldDeferTx(e->to)) return;  // retry next flush
      if (s_onSingleFlush) s_onSingleFlush(e->to, e->msgId[0], pkt, pktLen, txSf);
      if (bls_n::sendRtsBeforeLora(e->to, pktLen)) delay(50);
      if (esp_now_slots::sendRtsBeforeLora(e->to, pktLen)) delay(50);
      (void)queueTxPacket(pkt, pktLen, txSf, false, TxRequestClass::data);
    }
  } else {
    uint8_t batchPayload[protocol::MAX_PAYLOAD];
    size_t off = 0;
    batchPayload[off++] = (uint8_t)e->count;
    for (int i = 0; i < e->count && off + 2 + crypto::OVERHEAD + 100 < (int)sizeof(batchPayload); i++) {
      uint8_t encBuf[protocol::MAX_PAYLOAD + crypto::OVERHEAD];
      size_t encLen = sizeof(encBuf);
      if (!crypto::encryptFor(e->to, e->plain[i], e->plainLen[i], encBuf, &encLen)) continue;
      if (off + 2 + encLen > sizeof(batchPayload)) break;
      batchPayload[off++] = (uint8_t)(encLen & 0xFF);
      batchPayload[off++] = (uint8_t)(encLen >> 8);
      memcpy(batchPayload + off, encBuf, encLen);
      off += encLen;
    }
    if (off > 1) {
      uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD];
      size_t pktLen = protocol::buildPacket(pkt, sizeof(pkt),
          node::getId(), e->to, 31, protocol::OP_MSG_BATCH,
          batchPayload, off, false, false);
      if (pktLen > 0) {
        pkt_cache::addBatch(e->to, e->pktId, e->count, pkt, pktLen);
        if (bls_n::shouldDeferTx(e->to) || esp_now_slots::shouldDeferTx(e->to)) return;  // retry next flush
        if (bls_n::sendRtsBeforeLora(e->to, pktLen)) delay(50);
        if (esp_now_slots::sendRtsBeforeLora(e->to, pktLen)) delay(50);
        (void)queueTxPacket(pkt, pktLen, txSf, false, TxRequestClass::data);
        if (s_onBatchSent) s_onBatchSent(e->to, e->msgId, e->count);
      }
    }
  }
  e->inUse = false;
}

void setOnBatchSent(void (*cb)(const uint8_t* to, const uint32_t* msgIds, int count)) {
  s_onBatchSent = cb;
}

void setOnSingleFlush(bool (*cb)(const uint8_t* to, uint32_t msgId, const uint8_t* pkt, size_t pktLen, uint8_t txSf)) {
  s_onSingleFlush = cb;
}

void flush() {
  if (!s_inited) return;
  uint32_t now = millis();
  for (int i = 0; i < FUSION_SLOTS; i++) {
    if (!s_fusion[i].inUse) continue;
    FusionEntry* e = &s_fusion[i];
    if (e->count >= 2 || (now - e->firstAddTime) >= BATCH_WINDOW_MS) {
      flushEntry(e);
    }
  }
}

}  // namespace packet_fusion
