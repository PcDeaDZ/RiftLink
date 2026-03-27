/**
 * Очередь TX + отложенные отправки (millis), без FreeRTOS.
 * Паритет с async_tasks: queueDeferredSend / queueDeferredRelay / relayHeard.
 */

#include "async_tx.h"
#include "ack_coalesce/ack_coalesce.h"
#include "radio.h"
#include "log.h"
#include "protocol/packet.h"
#include <Arduino.h>
#include <string.h>

#define ASYNC_TX_QUEUE_DEPTH 6
#define DEFERRED_SEND_SLOTS 32
#define HEARD_RELAY_SIZE 8
#define PKT_CAP (protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID + protocol::MAX_PAYLOAD + 64)

struct TxSlot {
  uint8_t buf[PKT_CAP];
  size_t len;
  uint8_t txSf;
  bool priority;
  bool used;
};

struct DeferSlot {
  uint8_t buf[PKT_CAP];
  size_t len;
  uint8_t txSf;
  uint32_t dueMs;
  bool used;
  bool isRelay;
  uint8_t relayFrom[protocol::NODE_ID_LEN];
  uint32_t relayHash;
};

struct HeardRelayEntry {
  uint8_t from[protocol::NODE_ID_LEN];
  uint32_t hash;
};

static TxSlot s_txQ[ASYNC_TX_QUEUE_DEPTH];
static DeferSlot s_def[DEFERRED_SEND_SLOTS];
static HeardRelayEntry s_heardRelay[HEARD_RELAY_SIZE];
static uint8_t s_heardRelayIdx = 0;

static bool trySendNow(const uint8_t* buf, size_t len, uint8_t /* txSf */, char* reasonBuf, size_t reasonLen) {
  if (!buf || len == 0 || len > PKT_CAP) {
    if (reasonBuf && reasonLen) snprintf(reasonBuf, reasonLen, "bad_len");
    return false;
  }
  // Паритет с firmware/src/async_tasks.cpp queueTxRequestInternal: подсказки per-packet SF не используются.
  const uint8_t sf = radio::getMeshTxSfForQueue();
  bool ok = radio::send(buf, len, sf, false);
  if (!ok && reasonBuf && reasonLen) snprintf(reasonBuf, reasonLen, "radio_send_fail");
  return ok;
}

static bool relayHeardCheckAndRemove(const uint8_t* from, uint32_t hash) {
  for (int i = 0; i < HEARD_RELAY_SIZE; i++) {
    if (memcmp(s_heardRelay[i].from, from, protocol::NODE_ID_LEN) == 0 && s_heardRelay[i].hash == hash) {
      s_heardRelay[i].hash = 0xFFFFFFFFU;
      return true;
    }
  }
  return false;
}

void relayHeard(const uint8_t* from, uint32_t payloadHash) {
  if (!from) return;
  memcpy(s_heardRelay[s_heardRelayIdx].from, from, protocol::NODE_ID_LEN);
  s_heardRelay[s_heardRelayIdx].hash = payloadHash;
  s_heardRelayIdx = (uint8_t)((s_heardRelayIdx + 1u) % HEARD_RELAY_SIZE);
}

bool queueTxPacket(const uint8_t* buf, size_t len, uint8_t txSf, bool priority, TxRequestClass, char* reasonBuf,
    size_t reasonLen) {
  (void)priority;
  if (trySendNow(buf, len, txSf, reasonBuf, reasonLen)) return true;
  for (int i = 0; i < ASYNC_TX_QUEUE_DEPTH; i++) {
    if (!s_txQ[i].used) {
      memcpy(s_txQ[i].buf, buf, len);
      s_txQ[i].len = len;
      s_txQ[i].txSf = txSf;
      s_txQ[i].priority = priority;
      s_txQ[i].used = true;
      if (reasonBuf && reasonLen) reasonBuf[0] = '\0';
      return true;
    }
  }
  if (reasonBuf && reasonLen) snprintf(reasonBuf, reasonLen, "tx_q_full");
  return false;
}

void queueDeferredAck(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs, bool) {
  queueDeferredSend(pkt, len, txSf, delayMs, false);
}

void queueDeferredSend(const uint8_t* pkt, size_t len, uint8_t /* txSf */, uint32_t delayMs, bool) {
  if (!pkt || len == 0 || len > PKT_CAP) return;
  const uint8_t meshSf = radio::getMeshTxSfForQueue();
  uint32_t due = millis() + delayMs;
  for (int i = 0; i < DEFERRED_SEND_SLOTS; i++) {
    if (!s_def[i].used) {
      memcpy(s_def[i].buf, pkt, len);
      s_def[i].len = len;
      s_def[i].txSf = meshSf;
      s_def[i].dueMs = due;
      s_def[i].used = true;
      s_def[i].isRelay = false;
      return;
    }
  }
  char rb[24];
  if (!queueTxPacket(pkt, len, meshSf, true, TxRequestClass::data, rb, sizeof(rb))) {
    RIFTLINK_DIAG("ASYNC_TX", "event=DEFERRED_SEND_FALLBACK_FAIL cause=%s", rb[0] ? rb : "?");
  }
}

void queueDeferredRelay(const uint8_t* pkt, size_t len, uint8_t /* txSf */, uint32_t delayMs, const uint8_t* from,
    uint32_t payloadHash, bool) {
  if (!pkt || len == 0 || len > PKT_CAP || !from) return;
  const uint8_t meshSf = radio::getMeshTxSfForQueue();
  uint32_t due = millis() + delayMs;
  for (int i = 0; i < DEFERRED_SEND_SLOTS; i++) {
    if (!s_def[i].used) {
      memcpy(s_def[i].buf, pkt, len);
      s_def[i].len = len;
      s_def[i].txSf = meshSf;
      s_def[i].dueMs = due;
      s_def[i].used = true;
      s_def[i].isRelay = true;
      memcpy(s_def[i].relayFrom, from, protocol::NODE_ID_LEN);
      s_def[i].relayHash = payloadHash;
      return;
    }
  }
  char rb[24];
  if (!queueTxPacket(pkt, len, meshSf, true, TxRequestClass::data, rb, sizeof(rb))) {
    RIFTLINK_DIAG("ASYNC_TX", "event=DEFERRED_RELAY_FALLBACK_FAIL cause=%s", rb[0] ? rb : "?");
  }
}

void asyncTxPoll() {
  ack_coalesce::flush();
  uint32_t now = millis();
  for (int i = 0; i < DEFERRED_SEND_SLOTS; i++) {
    if (!s_def[i].used) continue;
    if ((int32_t)(now - s_def[i].dueMs) < 0) continue;
    if (s_def[i].isRelay && relayHeardCheckAndRemove(s_def[i].relayFrom, s_def[i].relayHash)) {
      s_def[i].used = false;
      continue;
    }
    char rb[24];
    if (trySendNow(s_def[i].buf, s_def[i].len, s_def[i].txSf, rb, sizeof(rb))) {
      s_def[i].used = false;
    } else {
      s_def[i].dueMs = now + 20;
    }
  }
  for (int pass = 0; pass < ASYNC_TX_QUEUE_DEPTH; pass++) {
    int best = -1;
    for (int i = 0; i < ASYNC_TX_QUEUE_DEPTH; i++) {
      if (!s_txQ[i].used) continue;
      if (best < 0 || s_txQ[i].priority) best = i;
    }
    if (best < 0) break;
    TxSlot& s = s_txQ[best];
    char rb[24];
    if (trySendNow(s.buf, s.len, s.txSf, rb, sizeof(rb))) {
      s.used = false;
    } else {
      break;
    }
  }
}
