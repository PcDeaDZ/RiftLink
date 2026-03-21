/**
 * ACK coalescing — буфер ACK по отправителю, flush как OP_ACK_BATCH
 */

#include "ack_coalesce.h"
#include "protocol/packet.h"
#include "node/node.h"
#include "neighbors/neighbors.h"
#include "msg_queue/msg_queue.h"
#include "async_tasks.h"
#include "crypto/crypto.h"
#include "x25519_keys/x25519_keys.h"
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

static void flushEntry(AckEntry* e) {
  if (e->count == 0) return;
  uint8_t txSf = e->txSf;
  if (txSf == 0) txSf = 12;
  uint8_t txSfOrtho = neighbors::rssiToSfOrthogonal(e->from);
  if (txSfOrtho != 0) txSf = txSfOrtho;

  if (e->count == 1) {
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
    batchPayload[0] = e->count;
    for (uint8_t i = 0; i < e->count; i++)
      memcpy(batchPayload + 1 + i * msg_queue::MSG_ID_LEN, &e->msgIds[i], msg_queue::MSG_ID_LEN);
    size_t batchLen = 1 + e->count * msg_queue::MSG_ID_LEN;
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
    if (ackLen > 0)
      queueDeferredAck(ackPkt, ackLen, txSf, 50);
  }
  e->inUse = false;
  e->count = 0;
}

void flush() {
  if (!s_inited) return;
  uint32_t now = millis();
  for (int i = 0; i < ACK_COALESCE_ENTRIES; i++) {
    if (!s_entries[i].inUse || s_entries[i].count == 0) continue;
    if (s_entries[i].count >= 2 || (now - s_entries[i].firstAddTime) >= COALESCE_WINDOW_MS)
      flushEntry(&s_entries[i]);
  }
}

}  // namespace ack_coalesce
