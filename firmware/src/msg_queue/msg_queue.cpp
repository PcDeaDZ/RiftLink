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
#include <Arduino.h>
#include <esp_random.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define MAX_PENDING      8
#define ACK_TIMEOUT_MS   6000
#define MAX_RETRIES      3
#define MUTEX_TIMEOUT_MS 100

struct PendingMsg {
  uint32_t msgId;
  uint8_t to[protocol::NODE_ID_LEN];
  uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t pktLen;
  uint32_t lastSendTime;
  uint8_t retries;
  bool inUse;
};

static PendingMsg s_pending[MAX_PENDING];
static uint32_t s_msgIdCounter = 0;
static bool s_inited = false;
static SemaphoreHandle_t s_mutex = nullptr;
static void (*s_onUnicastSent)(const uint8_t* to, uint32_t msgId) = nullptr;

namespace msg_queue {

void init() {
  s_mutex = xSemaphoreCreateMutex();
  memset(s_pending, 0, sizeof(s_pending));
  s_msgIdCounter = (uint32_t)esp_random();
  s_inited = true;
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

bool enqueue(const uint8_t* to, const char* text, uint8_t ttlMinutes) {
  if (!s_inited) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;

  const bool isUnicast = !node::isBroadcast(to);
  size_t textLen = strlen(text);
  if (textLen == 0) { xSemaphoreGive(s_mutex); return false; }  // пустые — не отправлять

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
    PendingMsg* slot = findFreeSlot();
    if (!slot) {
      xSemaphoreGive(s_mutex);
      return false;
    }
    uint32_t msgId = ++s_msgIdCounter;
    size_t off = 0;
    if (ttlMinutes > 0) {
      plainBuf[0] = ttlMinutes;
      off = MSG_TTL_LEN;
    }
    memcpy(plainBuf + off, &msgId, MSG_ID_LEN);
    memcpy(plainBuf + off + MSG_ID_LEN, text, textLen);
    plainLen = off + MSG_ID_LEN + textLen;

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
    if (!crypto::encryptFor(to, toEncrypt, toEncryptLen, encBuf, &encLen)) {
      RIFTLINK_LOG_ERR("[RiftLink] MSG encrypt FAIL (no key for %02X%02X)\n", to[0], to[1]);
      xSemaphoreGive(s_mutex);
      return false;
    }

    uint16_t pktId = (uint16_t)(msgId & 0xFFFF);
    size_t pktLen = protocol::buildPacket(slot->pkt, sizeof(slot->pkt),
        node::getId(), to, 31, protocol::OP_MSG,
        encBuf, encLen, true, true, useCompressed, protocol::CHANNEL_DEFAULT, pktId);
    if (pktLen == 0) {
      xSemaphoreGive(s_mutex);
      return false;
    }

    slot->msgId = msgId;
    pkt_cache::add(to, pktId, slot->pkt, pktLen);
    memcpy(slot->to, to, protocol::NODE_ID_LEN);
    slot->pktLen = pktLen;
    slot->lastSendTime = millis();
    slot->retries = 0;
    slot->inUse = true;

    uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(to));
    if (txSf == 0) txSf = 12;  // неизвестный сосед — SF12 для дальности
    bool ok = radio::send(slot->pkt, slot->pktLen, txSf);
    if (ok) {
      queueDeferredSend(slot->pkt, slot->pktLen, txSf, 250 + (esp_random() % 100));   // copy2: 250–350 ms
      queueDeferredSend(slot->pkt, slot->pktLen, txSf, 500 + (esp_random() % 150));   // copy3: 500–650 ms
    } else {
      RIFTLINK_LOG_ERR("[RiftLink] MSG sendQueue full, to %02X%02X — retry via update\n", to[0], to[1]);
    }
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
    if (!crypto::encrypt(toEncrypt, toEncryptLen, encBuf, &encLen)) return false;

    uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
    size_t len = protocol::buildPacket(pkt, sizeof(pkt),
        node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_GROUP_MSG,
        encBuf, encLen, true, false, useCompressed);
    xSemaphoreGive(s_mutex);
    if (len > 0) {
      uint8_t sf = neighbors::rssiToSf(neighbors::getMinRssi());
      if (!radio::send(pkt, len, sf)) {
        RIFTLINK_LOG_ERR("[RiftLink] MSG broadcast sendQueue full, drop\n");
        return false;
      }
      queueDeferredSend(pkt, len, sf, 220 + (esp_random() % 130));   // 2-я копия
      queueDeferredSend(pkt, len, sf, 440 + (esp_random() % 120));   // 3-я копия
      return true;
    }
    return false;
  }
}

bool enqueueGroup(uint32_t groupId, const char* text) {
  if (!s_inited) return false;

  size_t textLen = strlen(text);
  if (textLen == 0) return false;  // пустые — не отправлять
  constexpr size_t maxPlain = MAX_SINGLE_PLAIN - GROUP_ID_LEN - MSG_ID_LEN;
  if (textLen > maxPlain) return false;

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
  if (!crypto::encrypt(toEncrypt, toEncryptLen, encBuf, &encLen)) return false;

  uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_GROUP_MSG,
      encBuf, encLen, true, false, useCompressed);
  if (len > 0) {
    uint8_t sf = neighbors::rssiToSf(neighbors::getMinRssi());
    if (!radio::send(pkt, len, sf)) {
      RIFTLINK_LOG_ERR("[RiftLink] MSG group sendQueue full, drop\n");
      return false;
    }
    queueDeferredSend(pkt, len, sf, 220 + (esp_random() % 130));
    queueDeferredSend(pkt, len, sf, 440 + (esp_random() % 120));
    return true;
  }
  return false;
}

void setOnUnicastSent(void (*cb)(const uint8_t* to, uint32_t msgId)) {
  s_onUnicastSent = cb;
}

void onAckReceived(const uint8_t* payload, size_t payloadLen) {
  if (payloadLen < MSG_ID_LEN) return;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return;
  uint32_t msgId;
  memcpy(&msgId, payload, MSG_ID_LEN);
  PendingMsg* p = findByMsgId(msgId);
  if (p) p->inUse = false;
  xSemaphoreGive(s_mutex);
}

void update() {
  if (!s_inited) return;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  uint32_t now = millis();

  for (int i = 0; i < MAX_PENDING; i++) {
    PendingMsg* p = &s_pending[i];
    if (!p->inUse) continue;
    if (now - p->lastSendTime < ACK_TIMEOUT_MS) continue;

    if (p->retries >= MAX_RETRIES) {
      uint8_t flags = (p->pkt[protocol::SYNC_LEN] & 0x04) ? 1 : 0;  // compressed (version_flags)
      offline_queue::enqueue(p->to, p->pkt + protocol::PAYLOAD_OFFSET,
          p->pktLen - protocol::PAYLOAD_OFFSET, protocol::OP_MSG, flags);
      p->inUse = false;
      continue;
    }

    p->retries++;
    p->lastSendTime = now;
    radio::send(p->pkt, p->pktLen, neighbors::rssiToSf(neighbors::getRssiFor(p->to)), true);
  }
  xSemaphoreGive(s_mutex);
}

}  // namespace msg_queue
