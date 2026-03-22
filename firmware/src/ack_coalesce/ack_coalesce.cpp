/**
 * ACK coalescing — буфер ACK по отправителю, flush как OP_ACK_BATCH
 */

#include "ack_coalesce.h"
#include "protocol/packet.h"
#include "node/node.h"
#include "neighbors/neighbors.h"
#include "msg_queue/msg_queue.h"
#include "async_tasks.h"
#include "async_queues.h"
#include "crypto/crypto.h"
#include "x25519_keys/x25519_keys.h"
#include "radio/radio.h"
#include "log.h"
#include <string.h>
#include <Arduino.h>

#define ACK_COALESCE_ENTRIES 4
#define ACK_MSGIDS_MAX 8
#define COALESCE_WINDOW_MS 50

struct AckEntry {
  uint8_t from[protocol::NODE_ID_LEN];
  uint32_t msgIds[ACK_MSGIDS_MAX];
  uint8_t count;
  uint32_t firstAddTime;
  uint8_t txSf;
  bool inUse;
};

static AckEntry s_entries[ACK_COALESCE_ENTRIES];
static bool s_inited = false;

namespace ack_coalesce {

void init() {
  memset(s_entries, 0, sizeof(s_entries));
  s_inited = true;
}

static AckEntry* findOrCreate(const uint8_t* from) {
  for (int i = 0; i < ACK_COALESCE_ENTRIES; i++) {
    if (s_entries[i].inUse && memcmp(s_entries[i].from, from, protocol::NODE_ID_LEN) == 0)
      return &s_entries[i];
  }
  for (int i = 0; i < ACK_COALESCE_ENTRIES; i++) {
    if (!s_entries[i].inUse) {
      memcpy(s_entries[i].from, from, protocol::NODE_ID_LEN);
      s_entries[i].count = 0;
      s_entries[i].firstAddTime = millis();
      s_entries[i].inUse = true;
      return &s_entries[i];
    }
  }
  return nullptr;
}

void add(const uint8_t* from, uint32_t msgId, uint8_t txSf) {
  if (!s_inited || !from) return;
  AckEntry* e = findOrCreate(from);
  if (!e || e->count >= ACK_MSGIDS_MAX) return;
  e->msgIds[e->count++] = msgId;
  e->txSf = (txSf >= 7 && txSf <= 12) ? txSf : 12;
}

static uint8_t currentTxFree() {
  if (!radioCmdQueue) return 0;
  return (uint8_t)uxQueueSpacesAvailable(radioCmdQueue);
}

static uint8_t toaBoundIds(uint8_t txSf, uint8_t idsCap) {
  uint32_t toaBudgetUs = 90000;  // SF7 baseline
  if (txSf >= 8 && txSf <= 9) toaBudgetUs = 120000;
  else if (txSf == 10) toaBudgetUs = 160000;
  else if (txSf == 11) toaBudgetUs = 220000;
  else if (txSf == 12) toaBudgetUs = 320000;

  for (int n = (int)idsCap; n >= 1; n--) {
    size_t plainLen = 1 + (size_t)n * msg_queue::MSG_ID_LEN;
    size_t encLen = plainLen + crypto::OVERHEAD;
    size_t pktLen = protocol::PAYLOAD_OFFSET + encLen;
    uint32_t toaUs = radio::getTimeOnAir(pktLen);
    if (toaUs == 0 || toaUs <= toaBudgetUs) return (uint8_t)n;
  }
  return 1;
}

static uint8_t adaptiveBatchIds(uint8_t txSf, uint8_t txFree) {
  uint8_t bySf = 6;
  if (txSf >= 12) bySf = 2;
  else if (txSf == 11) bySf = 3;
  else if (txSf == 10) bySf = 4;
  else if (txSf >= 8) bySf = 5;

  if (txFree <= 2) bySf = 1;
  else if (txFree <= 4 && bySf > 2) bySf = 2;
  else if (txFree <= 6 && bySf > 3) bySf = 3;

  if (bySf > ACK_MSGIDS_MAX) bySf = ACK_MSGIDS_MAX;
  return toaBoundIds(txSf, bySf);
}

static uint32_t adaptiveWindowMs(uint8_t txSf, uint8_t txFree) {
  uint32_t w = COALESCE_WINDOW_MS;
  if (txSf >= 10) w += 20;       // higher SF: give more time to aggregate
  if (txFree <= 2) w = 12;       // queue pressure: flush quickly
  else if (txFree <= 4) w = 20;
  return w;
}

static void flushEntry(AckEntry* e) {
  if (e->count == 0) return;
  uint8_t txSf = e->txSf;
  if (txSf == 0) txSf = 12;
  uint8_t txSfOrtho = neighbors::rssiToSfOrthogonal(e->from);
  if (txSfOrtho != 0) txSf = txSfOrtho;
  uint8_t txFree = currentTxFree();
  uint8_t congestion = radio::getCongestionLevel();
  uint8_t sendCount = e->count;
  uint8_t maxIds = adaptiveBatchIds(txSf, txFree);
  // Hard guard for overload bursts: keep ACK minimal and fast.
  if (txFree <= 2 && congestion >= 2) maxIds = 1;
  if (sendCount > maxIds) sendCount = maxIds;
  if (sendCount == 0) sendCount = 1;

  if (sendCount == 1) {
    if (!x25519_keys::hasKeyFor(e->from)) {
      RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=no_pairwise_key from=%02X%02X type=single",
          e->from[0], e->from[1]);
      e->inUse = false;
      e->count = 0;
      return;
    }
    uint8_t ackPlain[msg_queue::MSG_ID_LEN];
    memcpy(ackPlain, &e->msgIds[0], msg_queue::MSG_ID_LEN);
    uint8_t ackPayload[msg_queue::MSG_ID_LEN + crypto::OVERHEAD];
    size_t ackPayloadLen = sizeof(ackPayload);
    if (!crypto::encryptFor(e->from, ackPlain, msg_queue::MSG_ID_LEN, ackPayload, &ackPayloadLen)) {
      RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=encrypt_fail from=%02X%02X type=single",
          e->from[0], e->from[1]);
      e->inUse = false;
      e->count = 0;
      return;
    }
    uint8_t ackPkt[protocol::PAYLOAD_OFFSET + msg_queue::MSG_ID_LEN + crypto::OVERHEAD + 8];
    size_t ackLen = protocol::buildPacket(ackPkt, sizeof(ackPkt),
        node::getId(), e->from, 31, protocol::OP_ACK,
        ackPayload, ackPayloadLen, true, false);
    if (ackLen > 0)
      queueDeferredAck(ackPkt, ackLen, txSf, 50);
  } else {
    if (!x25519_keys::hasKeyFor(e->from)) {
      RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=no_pairwise_key from=%02X%02X type=batch",
          e->from[0], e->from[1]);
      e->inUse = false;
      e->count = 0;
      return;
    }
    uint8_t batchPayload[1 + ACK_MSGIDS_MAX * msg_queue::MSG_ID_LEN];
    batchPayload[0] = sendCount;
    for (uint8_t i = 0; i < sendCount; i++)
      memcpy(batchPayload + 1 + i * msg_queue::MSG_ID_LEN, &e->msgIds[i], msg_queue::MSG_ID_LEN);
    size_t batchLen = 1 + sendCount * msg_queue::MSG_ID_LEN;
    uint8_t batchEnc[1 + ACK_MSGIDS_MAX * msg_queue::MSG_ID_LEN + crypto::OVERHEAD];
    size_t batchEncLen = sizeof(batchEnc);
    if (!crypto::encryptFor(e->from, batchPayload, batchLen, batchEnc, &batchEncLen)) {
      RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=encrypt_fail from=%02X%02X type=batch",
          e->from[0], e->from[1]);
      e->inUse = false;
      e->count = 0;
      return;
    }
    uint8_t ackPkt[protocol::PAYLOAD_OFFSET + 96];
    size_t ackLen = protocol::buildPacket(ackPkt, sizeof(ackPkt),
        node::getId(), e->from, 31, protocol::OP_ACK_BATCH,
        batchEnc, batchEncLen, true, false);
    if (ackLen > 0) {
      RIFTLINK_DIAG("ACK", "event=ACK_BATCH_ADAPT from=%02X%02X sf=%u tx_free=%u congestion=%u sent=%u pending=%u",
          e->from[0], e->from[1], (unsigned)txSf, (unsigned)txFree,
          (unsigned)congestion,
          (unsigned)sendCount, (unsigned)e->count);
      queueDeferredAck(ackPkt, ackLen, txSf, 50);
    }
  }

  if (sendCount < e->count) {
    uint8_t left = e->count - sendCount;
    memmove(e->msgIds, e->msgIds + sendCount, left * sizeof(uint32_t));
    e->count = left;
    e->firstAddTime = millis();
    e->txSf = txSf;
    e->inUse = true;
    return;
  }
  e->inUse = false;
  e->count = 0;
}

void flush() {
  if (!s_inited) return;
  uint32_t now = millis();
  for (int i = 0; i < ACK_COALESCE_ENTRIES; i++) {
    if (!s_entries[i].inUse || s_entries[i].count == 0) continue;
    uint8_t txSf = s_entries[i].txSf;
    if (txSf == 0) txSf = 12;
    uint8_t txSfOrtho = neighbors::rssiToSfOrthogonal(s_entries[i].from);
    if (txSfOrtho != 0) txSf = txSfOrtho;
    uint8_t txFree = currentTxFree();
    uint8_t maxIds = adaptiveBatchIds(txSf, txFree);
    if (txFree <= 2 && radio::getCongestionLevel() >= 2) maxIds = 1;
    uint32_t wndMs = adaptiveWindowMs(txSf, txFree);
    if (s_entries[i].count >= maxIds || (now - s_entries[i].firstAddTime) >= wndMs)
      flushEntry(&s_entries[i]);
  }
}

}  // namespace ack_coalesce
