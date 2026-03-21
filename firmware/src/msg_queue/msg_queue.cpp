/**
 * RiftLink — очередь сообщений, ACK, retransmit
 */

#include "msg_queue.h"
#include "log.h"
#include "async_tasks.h"
#include "pkt_cache/pkt_cache.h"
#include "node/node.h"
#include "radio/radio.h"
#include "neighbors/neighbors.h"
#include "crypto/crypto.h"
#include "compress/compress.h"
#include "frag/frag.h"
#include "groups/groups.h"
#include "offline_queue/offline_queue.h"
#include "mab/mab.h"
#include "collision_slots/collision_slots.h"
#include "beacon_sync/beacon_sync.h"
#include "clock_drift/clock_drift.h"
#include "packet_fusion/packet_fusion.h"
#include "bls_n/bls_n.h"
#include "esp_now_slots/esp_now_slots.h"
#include "x25519_keys/x25519_keys.h"
#include <Arduino.h>
#include <esp_random.h>
#include <string.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define MAX_PENDING           12
#define MAX_PENDING_BROADCAST 4
#define MAX_RETRIES           4   // макс. повторов при отсутствии ACK (не вечно)
// Paper: 2 deferred copies (radioCmdQueue 16) — экономия слотов при heap ~10KB
#if defined(USE_EINK)
#define BROADCAST_DEFERRED_COPIES 2
#else
#define BROADCAST_DEFERRED_COPIES 3
#endif
#define BROADCAST_ACK_TIMEOUT_MS 12000  // 12 с — ждём ACK от соседей
#define MUTEX_TIMEOUT_MS 100
#define ACK_REPLAY_GUARD_SIZE 16
#define ACK_REPLAY_GUARD_MS 60000
#define MSG_ENCRYPT_RETRY_COUNT 3
#define MSG_ENCRYPT_RETRY_DELAY_MS 2

// ACK timeout с учётом SF: SF7 быстрее, SF12 дольше (ToA, relay в mesh)
static uint32_t getAckTimeoutMs(uint8_t txSf) {
  if (txSf < 7 || txSf > 12) txSf = 12;
  return 2000 + (uint32_t)(txSf - 6) * 500;  // SF7: 2.5s, SF12: 5s
}

struct PendingMsg {
  uint32_t msgId;
  uint8_t to[protocol::NODE_ID_LEN];
  uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t pktLen;
  uint32_t lastSendTime;
  uint8_t retries;
  uint8_t txSf;   // SF при отправке — для timeout и retry
  bool inUse;
  bool triggerSend;  // RIT: отправить при следующем update (получили POLL от получателя)
  int8_t lastAction;  // MAB: последнее действие (0..2) для reward
  bool critical;
  uint8_t triggerType;
  uint32_t triggerValueMs;
  bool triggerReleasedNotified;
};

struct PendingBroadcast {
  uint32_t msgId;
  uint8_t totalNeighbors;
  uint8_t ackedFrom[16][protocol::NODE_ID_LEN];  // кто уже прислал ACK
  uint8_t ackedCount;
  uint32_t sendTime;
  bool inUse;
};

struct PendingBatch {
  uint8_t to[protocol::NODE_ID_LEN];
  uint32_t msgIds[4];
  uint8_t acked[4];
  int count;
  bool inUse;
};

static PendingMsg s_pending[MAX_PENDING];
static PendingBatch s_batchPending[2];
static PendingBroadcast s_bcPending[MAX_PENDING_BROADCAST];
static uint32_t s_msgIdCounter = 0;
static bool s_inited = false;
static SemaphoreHandle_t s_mutex = nullptr;
static void (*s_onUnicastSent)(const uint8_t* to, uint32_t msgId) = nullptr;
static void (*s_onUnicastUndelivered)(const uint8_t* to, uint32_t msgId) = nullptr;
static void (*s_onBroadcastSent)(uint32_t msgId) = nullptr;
static void (*s_onBroadcastDelivery)(uint32_t msgId, int delivered, int total) = nullptr;
static void (*s_onTimeCapsuleReleased)(const uint8_t* to, uint32_t msgId, uint8_t triggerType) = nullptr;
static std::atomic<uint8_t> s_lastSendFailReason{msg_queue::SEND_FAIL_NONE};
struct AckReplayEntry {
  std::atomic<uint32_t> fromLo;
  std::atomic<uint32_t> fromHi;
  std::atomic<uint32_t> msgId;
  std::atomic<uint32_t> seenAtMs;
};
static AckReplayEntry s_ackReplayGuard[ACK_REPLAY_GUARD_SIZE];
static std::atomic<uint32_t> s_ackReplayWriteIdx{0};

static inline uint32_t idPartLo(const uint8_t* id) {
  uint32_t v = 0;
  memcpy(&v, id, sizeof(uint32_t));
  return v;
}

static inline uint32_t idPartHi(const uint8_t* id) {
  uint32_t v = 0;
  memcpy(&v, id + sizeof(uint32_t), sizeof(uint32_t));
  return v;
}

static bool isAckReplayAndMark(const uint8_t* from, uint32_t msgId) {
  uint32_t now = millis();
  const uint32_t fromLo = idPartLo(from);
  const uint32_t fromHi = idPartHi(from);

  for (int i = 0; i < ACK_REPLAY_GUARD_SIZE; i++) {
    uint32_t ts = s_ackReplayGuard[i].seenAtMs.load(std::memory_order_acquire);
    if (ts == 0 || (now - ts) > ACK_REPLAY_GUARD_MS) continue;
    if (s_ackReplayGuard[i].msgId.load(std::memory_order_relaxed) == msgId &&
        s_ackReplayGuard[i].fromLo.load(std::memory_order_relaxed) == fromLo &&
        s_ackReplayGuard[i].fromHi.load(std::memory_order_relaxed) == fromHi) {
      return true;
    }
  }

  uint32_t idx = s_ackReplayWriteIdx.fetch_add(1, std::memory_order_relaxed) % ACK_REPLAY_GUARD_SIZE;
  s_ackReplayGuard[idx].fromLo.store(fromLo, std::memory_order_relaxed);
  s_ackReplayGuard[idx].fromHi.store(fromHi, std::memory_order_relaxed);
  s_ackReplayGuard[idx].msgId.store(msgId, std::memory_order_relaxed);
  s_ackReplayGuard[idx].seenAtMs.store(now, std::memory_order_release);
  return false;
}

namespace msg_queue {

static inline void setLastSendFail(SendFailReason reason) {
  s_lastSendFailReason.store((uint8_t)reason, std::memory_order_relaxed);
}

SendFailReason getLastSendFailReason() {
  return (SendFailReason)s_lastSendFailReason.load(std::memory_order_relaxed);
}

void init() {
  s_mutex = xSemaphoreCreateMutex();
  memset(s_pending, 0, sizeof(s_pending));
  memset(s_bcPending, 0, sizeof(s_bcPending));
  for (int i = 0; i < ACK_REPLAY_GUARD_SIZE; i++) {
    s_ackReplayGuard[i].fromLo.store(0, std::memory_order_relaxed);
    s_ackReplayGuard[i].fromHi.store(0, std::memory_order_relaxed);
    s_ackReplayGuard[i].msgId.store(0, std::memory_order_relaxed);
    s_ackReplayGuard[i].seenAtMs.store(0, std::memory_order_relaxed);
  }
  s_ackReplayWriteIdx.store(0, std::memory_order_relaxed);
  s_msgIdCounter = (uint32_t)esp_random();
  s_inited = true;
}

static void registerBroadcastPending(uint32_t msgId, int totalNeighbors) {
  for (int i = 0; i < MAX_PENDING_BROADCAST; i++) {
    if (!s_bcPending[i].inUse) {
      s_bcPending[i].msgId = msgId;
      s_bcPending[i].totalNeighbors = (uint8_t)(totalNeighbors > 16 ? 16 : totalNeighbors);
      s_bcPending[i].ackedCount = 0;
      s_bcPending[i].sendTime = millis();
      s_bcPending[i].inUse = true;
      memset(s_bcPending[i].ackedFrom, 0, sizeof(s_bcPending[i].ackedFrom));
      return;
    }
  }
}

static PendingBroadcast* findBroadcastByMsgId(uint32_t msgId) {
  for (int i = 0; i < MAX_PENDING_BROADCAST; i++) {
    if (s_bcPending[i].inUse && s_bcPending[i].msgId == msgId)
      return &s_bcPending[i];
  }
  return nullptr;
}

static void addBroadcastAck(PendingBroadcast* pb, const uint8_t* from) {
  for (int i = 0; i < pb->ackedCount; i++) {
    if (memcmp(pb->ackedFrom[i], from, protocol::NODE_ID_LEN) == 0) return;  // уже есть
  }
  if (pb->ackedCount < 16) {
    memcpy(pb->ackedFrom[pb->ackedCount], from, protocol::NODE_ID_LEN);
    pb->ackedCount++;
  }
}

static PendingMsg* findFreeSlot() {
  for (int i = 0; i < MAX_PENDING; i++) {
    if (!s_pending[i].inUse) return &s_pending[i];
  }
  return nullptr;
}

static PendingMsg* findByMsgId(uint32_t msgId) {
  for (int i = 0; i < MAX_PENDING; i++) {
    if (s_pending[i].inUse && s_pending[i].msgId == msgId)
      return &s_pending[i];
  }
  return nullptr;
}

// Макс. plaintext для одного пакета (encrypted output <= MAX_PAYLOAD)
constexpr size_t MAX_SINGLE_PLAIN = protocol::MAX_PAYLOAD - crypto::OVERHEAD;

constexpr size_t MSG_TTL_LEN = 1;

bool enqueue(const uint8_t* to, const char* text, uint8_t ttlMinutes,
    bool critical, TriggerType triggerType, uint32_t triggerValueMs) {
  setLastSendFail(SEND_FAIL_NONE);
  if (!s_inited) {
    setLastSendFail(SEND_FAIL_NOT_READY);
    return false;
  }
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
    setLastSendFail(SEND_FAIL_MUTEX_BUSY);
    return false;
  }

  const bool isUnicast = !node::isBroadcast(to);
  size_t textLen = strlen(text);
  if (textLen == 0) { xSemaphoreGive(s_mutex); setLastSendFail(SEND_FAIL_EMPTY); return false; }  // пустые — не отправлять

  // Длинные сообщения — фрагментация (без ACK для MVP)
  size_t ttlOverhead = (ttlMinutes > 0) ? MSG_TTL_LEN : 0;
  size_t maxSingle = MAX_SINGLE_PLAIN - ttlOverhead
      - (isUnicast ? MSG_ID_LEN : 0)
      - (isUnicast ? 0 : GROUP_ID_LEN + MSG_ID_LEN);  // broadcast: groupId + msgId
  if (textLen > maxSingle) {
    xSemaphoreGive(s_mutex);
    if (textLen > frag::MAX_MSG_PLAIN) textLen = frag::MAX_MSG_PLAIN;
    return frag::send(to, (const uint8_t*)text, textLen, textLen >= compress::MIN_LEN_TO_COMPRESS);
  }

  uint8_t plainBuf[256];
  size_t plainLen;

  if (isUnicast) {
    uint32_t msgId = ++s_msgIdCounter;
    size_t off = 0;
    if (ttlMinutes > 0) {
      plainBuf[0] = ttlMinutes;
      off = MSG_TTL_LEN;
    }
    memcpy(plainBuf + off, &msgId, MSG_ID_LEN);
    memcpy(plainBuf + off + MSG_ID_LEN, text, textLen);
    plainLen = off + MSG_ID_LEN + textLen;

    uint8_t compBuf[protocol::MAX_PAYLOAD];
    size_t compLen = compress::compress(plainBuf, plainLen, compBuf, sizeof(compBuf));
    bool useCompressed = (compLen > 0);
    const uint8_t* toOffer = useCompressed ? compBuf : plainBuf;
    size_t toOfferLen = useCompressed ? compLen : plainLen;
    if (packet_fusion::offer(to, toOffer, toOfferLen, msgId, useCompressed)) {
      if (s_onUnicastSent) s_onUnicastSent(to, msgId);
      xSemaphoreGive(s_mutex);
      return true;
    }

    PendingMsg* slot = findFreeSlot();
    if (!slot) {
      RIFTLINK_LOG_ERR("[RiftLink] MSG pending slots full (max %d)\n", MAX_PENDING);
      xSemaphoreGive(s_mutex);
      setLastSendFail(SEND_FAIL_PENDING_FULL);
      return false;
    }

    uint8_t toEncrypt[protocol::MAX_PAYLOAD + crypto::OVERHEAD];
    size_t toEncryptLen = useCompressed ? compLen : plainLen;
    if (useCompressed) {
      memcpy(toEncrypt, compBuf, compLen);
    } else {
      memcpy(toEncrypt, plainBuf, plainLen);
    }

    uint8_t encBuf[protocol::MAX_PAYLOAD + crypto::OVERHEAD];
    size_t encLen = sizeof(encBuf);
    bool encOk = false;
    for (int encTry = 0; encTry < MSG_ENCRYPT_RETRY_COUNT; encTry++) {
      encLen = sizeof(encBuf);
      if (crypto::encryptFor(to, toEncrypt, toEncryptLen, encBuf, &encLen)) {
        encOk = true;
        break;
      }
      if (encTry + 1 < MSG_ENCRYPT_RETRY_COUNT) {
        vTaskDelay(pdMS_TO_TICKS(MSG_ENCRYPT_RETRY_DELAY_MS));
      }
    }
    if (!encOk) {
      bool hasKeyNow = x25519_keys::hasKeyFor(to);
      if (!hasKeyNow) {
        RIFTLINK_LOG_ERR("[RiftLink] MSG encrypt FAIL (no key for %02X%02X)\n", to[0], to[1]);
        setLastSendFail(SEND_FAIL_NO_KEY);
      } else {
        RIFTLINK_DIAG("KEY", "event=KEY_BUSY_ENCRYPT peer=%02X%02X op=msg_encrypt",
            to[0], to[1]);
        RIFTLINK_LOG_ERR("[RiftLink] MSG encrypt FAIL (key busy for %02X%02X)\n", to[0], to[1]);
        setLastSendFail(SEND_FAIL_KEY_BUSY);
      }
      xSemaphoreGive(s_mutex);
      return false;
    }

    uint16_t pktId = (uint16_t)(msgId & 0xFFFF);
    uint8_t channel = critical ? protocol::CHANNEL_CRITICAL : protocol::CHANNEL_DEFAULT;
    size_t pktLen = protocol::buildPacket(slot->pkt, sizeof(slot->pkt),
        node::getId(), to, 31, protocol::OP_MSG,
        encBuf, encLen, true, true, useCompressed, channel, pktId);
    if (pktLen == 0) {
      xSemaphoreGive(s_mutex);
      setLastSendFail(SEND_FAIL_BUILD_PACKET);
      return false;
    }

    slot->msgId = msgId;
    pkt_cache::add(to, pktId, slot->pkt, pktLen);
    memcpy(slot->to, to, protocol::NODE_ID_LEN);
    slot->pktLen = pktLen;
    slot->lastSendTime = millis();
    slot->retries = 0;
    slot->inUse = true;
    slot->triggerSend = false;
    slot->lastAction = -1;
    slot->critical = critical;
    slot->triggerType = (uint8_t)triggerType;
    slot->triggerValueMs = triggerValueMs;
    slot->triggerReleasedNotified = false;

    uint8_t txSf = neighbors::rssiToSfOrthogonal(to);
    if (txSf == 0) txSf = 12;  // неизвестный сосед — SF12 для дальности
    slot->txSf = txSf;

    // BLS-N + ESP-NOW: RTS перед LoRa, defer при конфликте
    if (bls_n::shouldDeferTx(to) || esp_now_slots::shouldDeferTx(to)) {
      xSemaphoreGive(s_mutex);
      return true;  // отложить, update повторит
    }
    if (bls_n::sendRtsBeforeLora(to, slot->pktLen)) delay(50);
    if (esp_now_slots::sendRtsBeforeLora(to, slot->pktLen)) delay(50);

    // Unicast: только одна отправка. Повтор — только при отсутствии ACK (update), не слепые копии
    bool ok = queueTxPacket(slot->pkt, slot->pktLen, txSf, critical,
        critical ? TxRequestClass::critical : TxRequestClass::data);
    if (!ok) {
      RIFTLINK_LOG_ERR("[RiftLink] MSG radioCmdQueue full, to %02X%02X — retry via update\n", to[0], to[1]);
    }
    neighbors::recordAckSent(to);
    if (s_onUnicastSent) s_onUnicastSent(to, msgId);
    xSemaphoreGive(s_mutex);
    return true;
  } else {
    // Broadcast — OP_GROUP_MSG с groupId=GROUP_ALL, msgId для дедупликации повторов
    const uint32_t groupId = groups::GROUP_ALL;
    uint32_t bcMsgId = ++s_msgIdCounter;
    memcpy(plainBuf, &groupId, GROUP_ID_LEN);
    memcpy(plainBuf + GROUP_ID_LEN, &bcMsgId, MSG_ID_LEN);
    memcpy(plainBuf + GROUP_ID_LEN + MSG_ID_LEN, text, textLen);
    plainLen = GROUP_ID_LEN + MSG_ID_LEN + textLen;

    uint8_t toEncrypt[protocol::MAX_PAYLOAD + crypto::OVERHEAD];
    size_t toEncryptLen = plainLen;
    bool useCompressed = false;

    uint8_t compBuf[protocol::MAX_PAYLOAD];
    size_t compLen = compress::compress(plainBuf, plainLen, compBuf, sizeof(compBuf));
    if (compLen > 0) {
      memcpy(toEncrypt, compBuf, compLen);
      toEncryptLen = compLen;
      useCompressed = true;
    } else {
      memcpy(toEncrypt, plainBuf, plainLen);
    }

    uint8_t encBuf[protocol::MAX_PAYLOAD + crypto::OVERHEAD];
    size_t encLen = sizeof(encBuf);
    if (!crypto::encrypt(toEncrypt, toEncryptLen, encBuf, &encLen)) {
      RIFTLINK_LOG_ERR("[RiftLink] MSG broadcast encrypt FAILED\n");
      setLastSendFail(SEND_FAIL_BUILD_PACKET);
      return false;
    }

    uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
    size_t len = protocol::buildPacket(pkt, sizeof(pkt),
        node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_GROUP_MSG,
        encBuf, encLen, true, false, useCompressed, protocol::CHANNEL_DEFAULT);
    if (len > 0) {
      registerBroadcastPending(bcMsgId, neighbors::getCount());
    }
    xSemaphoreGive(s_mutex);
    if (len > 0) {
      uint8_t sf = neighbors::rssiToSf(neighbors::getMinRssi());
      if (!queueTxPacket(pkt, len, sf, false, TxRequestClass::data)) {
        RIFTLINK_LOG_ERR("[RiftLink] MSG broadcast radioCmdQueue full, drop\n");
        setLastSendFail(SEND_FAIL_RADIO_QUEUE);
        return false;
      }
      if (s_onBroadcastSent) s_onBroadcastSent(bcMsgId);
      queueDeferredSend(pkt, len, sf, 220 + (esp_random() % 130));
      if (BROADCAST_DEFERRED_COPIES >= 2)
        queueDeferredSend(pkt, len, sf, 440 + (esp_random() % 120));
      if (BROADCAST_DEFERRED_COPIES >= 3)
        queueDeferredSend(pkt, len, sf, 800 + (esp_random() % 150));
      return true;
    }
    return false;
  }
}

bool enqueueGroup(uint32_t groupId, const char* text) {
  if (!s_inited) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;

  size_t textLen = strlen(text);
  if (textLen == 0) { xSemaphoreGive(s_mutex); return false; }  // пустые — не отправлять
  constexpr size_t maxPlain = MAX_SINGLE_PLAIN - GROUP_ID_LEN - MSG_ID_LEN;
  if (textLen > maxPlain) { xSemaphoreGive(s_mutex); return false; }

  uint32_t bcMsgId = ++s_msgIdCounter;
  uint8_t plainBuf[256];
  memcpy(plainBuf, &groupId, GROUP_ID_LEN);
  memcpy(plainBuf + GROUP_ID_LEN, &bcMsgId, MSG_ID_LEN);
  memcpy(plainBuf + GROUP_ID_LEN + MSG_ID_LEN, text, textLen + 1);
  size_t plainLen = GROUP_ID_LEN + MSG_ID_LEN + textLen;

  uint8_t toEncrypt[protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t toEncryptLen = plainLen;
  bool useCompressed = false;

  uint8_t compBuf[protocol::MAX_PAYLOAD];
  size_t compLen = compress::compress(plainBuf, plainLen, compBuf, sizeof(compBuf));
  if (compLen > 0) {
    memcpy(toEncrypt, compBuf, compLen);
    toEncryptLen = compLen;
    useCompressed = true;
  } else {
    memcpy(toEncrypt, plainBuf, plainLen);
  }

  uint8_t encBuf[protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t encLen = sizeof(encBuf);
  bool encOk = false;
  uint8_t groupKey[32];
  if (groups::getGroupKey(groupId, groupKey)) {
    encOk = crypto::encryptWithGroupKey(groupKey, toEncrypt, toEncryptLen, encBuf, &encLen);
  } else {
    encOk = crypto::encrypt(toEncrypt, toEncryptLen, encBuf, &encLen);
  }
  if (!encOk) {
    xSemaphoreGive(s_mutex);
    return false;
  }

  uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_GROUP_MSG,
      encBuf, encLen, true, false, useCompressed);
  if (len > 0) {
    registerBroadcastPending(bcMsgId, neighbors::getCount());
  }
  xSemaphoreGive(s_mutex);
  if (len > 0) {
    uint8_t sf = neighbors::rssiToSf(neighbors::getMinRssi());
    if (!queueTxPacket(pkt, len, sf, false, TxRequestClass::data)) {
      RIFTLINK_LOG_ERR("[RiftLink] MSG group radioCmdQueue full, drop\n");
      return false;
    }
    if (s_onBroadcastSent) s_onBroadcastSent(bcMsgId);
    queueDeferredSend(pkt, len, sf, 220 + (esp_random() % 130));
    if (BROADCAST_DEFERRED_COPIES >= 2)
      queueDeferredSend(pkt, len, sf, 440 + (esp_random() % 120));
    if (BROADCAST_DEFERRED_COPIES >= 3)
      queueDeferredSend(pkt, len, sf, 800 + (esp_random() % 150));
    return true;
  }
  return false;
}

bool enqueueSos(const char* text) {
  if (!s_inited || !text) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;
  size_t textLen = strlen(text);
  if (textLen == 0 || textLen > (MAX_SINGLE_PLAIN - MSG_ID_LEN)) {
    xSemaphoreGive(s_mutex);
    return false;
  }
  uint8_t plainBuf[256];
  uint32_t msgId = ++s_msgIdCounter;
  memcpy(plainBuf, &msgId, MSG_ID_LEN);
  memcpy(plainBuf + MSG_ID_LEN, text, textLen);
  size_t plainLen = MSG_ID_LEN + textLen;
  uint8_t encBuf[protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t encLen = sizeof(encBuf);
  if (!crypto::encrypt(plainBuf, plainLen, encBuf, &encLen)) {
    xSemaphoreGive(s_mutex);
    return false;
  }
  uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  int n = neighbors::getCount();
  uint8_t ttl = 31;
  if (n <= 1) ttl = 31;
  else if (n <= 3) ttl = 24;
  else if (n <= 6) ttl = 18;
  else ttl = 12;
  size_t len = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), protocol::BROADCAST_ID, ttl,
      protocol::OP_SOS, encBuf, encLen, true, false, false, protocol::CHANNEL_CRITICAL, (uint16_t)(msgId & 0xFFFF));
  xSemaphoreGive(s_mutex);
  if (len == 0) return false;
  uint8_t sf = neighbors::rssiToSf(neighbors::getMinRssi());
  if (sf == 0) sf = 12;
  if (!queueTxPacket(pkt, len, sf, true, TxRequestClass::critical)) return false;
  if (s_onBroadcastSent) s_onBroadcastSent(msgId);
  return true;
}

void setOnBroadcastSent(void (*cb)(uint32_t msgId)) {
  s_onBroadcastSent = cb;
}

void setOnBroadcastDelivery(void (*cb)(uint32_t msgId, int delivered, int total)) {
  s_onBroadcastDelivery = cb;
}

void setOnTimeCapsuleReleased(void (*cb)(const uint8_t* to, uint32_t msgId, uint8_t triggerType)) {
  s_onTimeCapsuleReleased = cb;
}

void setOnUnicastSent(void (*cb)(const uint8_t* to, uint32_t msgId)) {
  s_onUnicastSent = cb;
}

void setOnUnicastUndelivered(void (*cb)(const uint8_t* to, uint32_t msgId)) {
  s_onUnicastUndelivered = cb;
}

bool onAckReceived(const uint8_t* from, const uint8_t* payload, size_t payloadLen,
    bool requireOnline, bool allowUnicast, bool allowBroadcast) {
  if (payloadLen < MSG_ID_LEN || !from || !payload) return false;
  if (node::isInvalidNodeId(from) || node::isBroadcast(from) || node::isForMe(from)) return false;
  if (requireOnline && !neighbors::isOnline(from)) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;
  uint32_t msgId;
  memcpy(&msgId, payload, MSG_ID_LEN);
  if (isAckReplayAndMark(from, msgId)) {
    xSemaphoreGive(s_mutex);
    return false;
  }
  bool unicastCleared = false;
  if (allowUnicast) {
    PendingMsg* p = findByMsgId(msgId);
    if (p && memcmp(p->to, from, protocol::NODE_ID_LEN) == 0) {
      if (p->lastAction >= 0) mab::reward(p->lastAction, 1);
      p->inUse = false;
      neighbors::recordAckReceived(from);
      RIFTLINK_DIAG("ACK", "event=ACK_COUNTED mode=unicast from=%02X%02X msgId=%lu",
          from[0], from[1], (unsigned long)msgId);
      unicastCleared = true;
    } else {
      for (int i = 0; i < 2; i++) {
        if (!s_batchPending[i].inUse) continue;
        if (memcmp(s_batchPending[i].to, from, protocol::NODE_ID_LEN) != 0) continue;
        for (int j = 0; j < s_batchPending[i].count; j++) {
          if (s_batchPending[i].msgIds[j] == msgId && !s_batchPending[i].acked[j]) {
            s_batchPending[i].acked[j] = 1;
            unicastCleared = true;
            bool allDone = true;
            for (int k = 0; k < s_batchPending[i].count; k++) {
              if (!s_batchPending[i].acked[k]) { allDone = false; break; }
            }
            if (allDone) s_batchPending[i].inUse = false;
            RIFTLINK_DIAG("ACK", "event=ACK_COUNTED mode=batch from=%02X%02X msgId=%lu",
                from[0], from[1], (unsigned long)msgId);
            break;
          }
        }
        if (unicastCleared) break;
      }
    }
  }
  if (allowBroadcast && !unicastCleared) {
    PendingBroadcast* pb = findBroadcastByMsgId(msgId);
    if (pb && (!requireOnline || neighbors::isOnline(from))) {
      addBroadcastAck(pb, from);
      RIFTLINK_DIAG("ACK", "event=ACK_COUNTED mode=broadcast from=%02X%02X msgId=%lu delivered=%u total=%u",
          from[0], from[1], (unsigned long)msgId, (unsigned)pb->ackedCount, (unsigned)pb->totalNeighbors);
    }
  }
  xSemaphoreGive(s_mutex);
  return unicastCleared;
}

bool onBroadcastAckWitness(const uint8_t* from, uint32_t msgId, bool requireOnline) {
  uint8_t payload[MSG_ID_LEN];
  memcpy(payload, &msgId, MSG_ID_LEN);
  return onAckReceived(from, payload, MSG_ID_LEN, requireOnline, false, true);
}

void onAckBatchReceived(const uint8_t* from, const uint8_t* payload, size_t payloadLen, int rssi,
    void (*onDelivered)(const uint8_t* from, uint32_t msgId, int rssi)) {
  if (payloadLen < 5 || !from) return;  // count(1) + msgId(4)
  uint8_t count = payload[0];
  if (count == 0 || count > 8 || payloadLen < 1 + count * MSG_ID_LEN) return;
  for (uint8_t i = 0; i < count; i++) {
    uint32_t msgId;
    memcpy(&msgId, payload + 1 + i * MSG_ID_LEN, MSG_ID_LEN);
    uint8_t singlePayload[MSG_ID_LEN];
    memcpy(singlePayload, &msgId, MSG_ID_LEN);
    if (onAckReceived(from, singlePayload, MSG_ID_LEN, false, true, true) && onDelivered) {
      onDelivered(from, msgId, rssi);
    }
  }
}

bool registerPendingFromFusion(const uint8_t* to, uint32_t msgId, const uint8_t* pkt, size_t pktLen, uint8_t txSf) {
  if (!s_inited || !to || !pkt || pktLen > sizeof(s_pending[0].pkt)) return false;
  PendingMsg* slot = findFreeSlot();
  if (!slot) return false;
  slot->msgId = msgId;
  memcpy(slot->to, to, protocol::NODE_ID_LEN);
  memcpy(slot->pkt, pkt, pktLen);
  slot->pktLen = pktLen;
  slot->lastSendTime = millis();
  slot->retries = 0;
  slot->txSf = txSf;
  slot->inUse = true;
  slot->triggerSend = false;
  slot->lastAction = -1;
  slot->critical = false;
  slot->triggerType = TRIGGER_NONE;
  slot->triggerValueMs = 0;
  slot->triggerReleasedNotified = false;
  return true;
}

void registerBatchSent(const uint8_t* to, const uint32_t* msgIds, int count) {
  if (!to || !msgIds || count < 1 || count > 4) return;
  for (int i = 0; i < 2; i++) {
    if (!s_batchPending[i].inUse) {
      memcpy(s_batchPending[i].to, to, protocol::NODE_ID_LEN);
      for (int j = 0; j < count; j++) s_batchPending[i].msgIds[j] = msgIds[j];
      memset(s_batchPending[i].acked, 0, sizeof(s_batchPending[i].acked));
      s_batchPending[i].count = count;
      s_batchPending[i].inUse = true;
      return;
    }
  }
}

void onPollReceived(const uint8_t* from) {
  if (!from || !s_inited || !s_mutex) return;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return;
  for (int i = 0; i < MAX_PENDING; i++) {
    if (s_pending[i].inUse && memcmp(s_pending[i].to, from, protocol::NODE_ID_LEN) == 0) {
      s_pending[i].triggerSend = true;
      break;
    }
  }
  xSemaphoreGive(s_mutex);
}

void update() {
  if (!s_inited) return;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  uint32_t now = millis();

  packet_fusion::flush();

  for (int i = 0; i < MAX_PENDING_BROADCAST; i++) {
    PendingBroadcast* pb = &s_bcPending[i];
    if (!pb->inUse) continue;
    if (now - pb->sendTime < BROADCAST_ACK_TIMEOUT_MS) continue;
    if (s_onBroadcastDelivery) {
      s_onBroadcastDelivery(pb->msgId, pb->ackedCount, pb->totalNeighbors);
    }
    pb->inUse = false;
  }

  for (int i = 0; i < MAX_PENDING; i++) {
    PendingMsg* p = &s_pending[i];
    if (!p->inUse) continue;

    // RIT: POLL от получателя — отправить с малым jitter (50–150 ms)
    if (p->triggerSend) {
      p->triggerSend = false;
      uint32_t jitter = 50 + (uint32_t)(esp_random() % 100);
      if (now - p->lastSendTime >= jitter) {
        if (bls_n::shouldDeferTx(p->to) || esp_now_slots::shouldDeferTx(p->to)) continue;
        if (bls_n::sendRtsBeforeLora(p->to, p->pktLen)) delay(50);
        if (esp_now_slots::sendRtsBeforeLora(p->to, p->pktLen)) delay(50);
        p->retries++;
        p->lastSendTime = now;
        (void)queueTxPacket(p->pkt, p->pktLen, p->txSf, true,
            p->critical ? TxRequestClass::critical : TxRequestClass::data);
      }
      continue;
    }

    uint32_t ackTimeout = getAckTimeoutMs(p->txSf);
    if (p->triggerType == TRIGGER_TARGET_ONLINE && !neighbors::isOnline(p->to)) continue;
    if (p->triggerType == TRIGGER_DELIVER_AFTER && (int32_t)(now - p->triggerValueMs) < 0) continue;
    if (p->triggerType != TRIGGER_NONE && !p->triggerReleasedNotified) {
      p->triggerReleasedNotified = true;
      if (s_onTimeCapsuleReleased) s_onTimeCapsuleReleased(p->to, p->msgId, p->triggerType);
    }
    int action = mab::selectAction();
    uint32_t jitter = mab::getDelayMs(action) + collision_slots::getAvoidanceDelayMs()
        + beacon_sync::getAvoidanceDelayMs();
    uint32_t quietMs = clock_drift::getQuietWindowMs(p->to);
    if (quietMs > 0 && quietMs > jitter) jitter = quietMs;  // предпочесть «тихое» окно соседа
    if (now - p->lastSendTime < ackTimeout + jitter) continue;

    uint8_t maxRetries = p->critical ? (MAX_RETRIES + 2) : MAX_RETRIES;
    if (p->retries >= maxRetries) {
      if (p->lastAction >= 0) mab::reward(p->lastAction, -1);
      if (s_onUnicastUndelivered) s_onUnicastUndelivered(p->to, p->msgId);
      uint8_t flags = (p->pkt[protocol::SYNC_LEN] & 0x04) ? 1 : 0;  // compressed (version_flags)
      if (p->critical) flags |= 2;
      offline_queue::enqueue(p->to, p->pkt + protocol::PAYLOAD_OFFSET,
          p->pktLen - protocol::PAYLOAD_OFFSET, protocol::OP_MSG, flags);
      p->inUse = false;
      RIFTLINK_LOG_ERR("[RiftLink] MSG no ACK after %u retries, to %02X%02X → offline\n",
          (unsigned)MAX_RETRIES, p->to[0], p->to[1]);
      continue;
    }

    if (bls_n::shouldDeferTx(p->to) || esp_now_slots::shouldDeferTx(p->to)) continue;
    if (bls_n::sendRtsBeforeLora(p->to, p->pktLen)) delay(50);
    if (esp_now_slots::sendRtsBeforeLora(p->to, p->pktLen)) delay(50);
    p->lastAction = action;
    p->retries++;
    p->lastSendTime = now;
    neighbors::recordAckSent(p->to);
    (void)queueTxPacket(p->pkt, p->pktLen, p->txSf, p->critical,
        p->critical ? TxRequestClass::critical : TxRequestClass::data);
  }
  xSemaphoreGive(s_mutex);
}

}  // namespace msg_queue
