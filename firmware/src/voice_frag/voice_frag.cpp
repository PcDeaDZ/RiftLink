/**
 * RiftLink — VOICE_MSG фрагментация
 */

#include "voice_frag.h"
#include "../voice_buffers/voice_buffers.h"
#include "neighbors/neighbors.h"
#include "node/node.h"
#include "radio/radio.h"
#include "async_tasks.h"
#include "log.h"
#include "crypto/crypto.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <string.h>

#define FRAG_HEADER_LEN 6
#define FRAG_DATA_MAX (protocol::MAX_PAYLOAD - FRAG_HEADER_LEN)
#define VOICE_REASSEMBLE_MAX 2
#define VOICE_TIMEOUT_MS 15000

struct VoiceMeshTxProfile {
  uint8_t code;
  uint8_t minSf;
  uint8_t channel;
  uint16_t interFragDelayMs;
};

struct VoiceSlot {
  uint32_t msgId;
  uint8_t from[protocol::NODE_ID_LEN];
  uint8_t to[protocol::NODE_ID_LEN];
  uint8_t* storage;
  size_t storageCap;
  uint8_t partsReceivedUnique;
  uint8_t partsTotal;
  uint16_t lastPartLen;
  bool hasLastPart;
  uint64_t partsMask;     // до 64 фрагментов (MAX_FRAGMENTS=43)
  uint32_t lastTime;
  bool inUse;
};

static VoiceSlot s_slots[VOICE_REASSEMBLE_MAX];
static uint32_t s_msgIdCounter = 0;
static bool s_inited = false;

#define VOICE_PENDING_ACK_MAX 4
struct PendingVoiceAck {
  bool inUse;
  uint8_t to[protocol::NODE_ID_LEN];
  uint32_t msgId;
  uint32_t sentAtMs;
};
static PendingVoiceAck s_pendingAcks[VOICE_PENDING_ACK_MAX];
static constexpr uint32_t VOICE_ACK_TIMEOUT_MS = 120000;

static bool ensureSlotStorage(VoiceSlot* slot) {
  const size_t need = voice_frag::MAX_VOICE_PLAIN + 1024;
  if (slot->storage && slot->storageCap >= need) return true;
  if (slot->storage) {
    heap_caps_free(slot->storage);
    slot->storage = nullptr;
    slot->storageCap = 0;
  }
  slot->storage = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!slot->storage) {
    Serial.printf("[RiftLink] VOICE_SLOT OOM need=%u\n", (unsigned)need);
    return false;
  }
  slot->storageCap = need;
  return true;
}

static void freeSlotStorage(VoiceSlot* slot) {
  if (slot->storage) {
    heap_caps_free(slot->storage);
    slot->storage = nullptr;
    slot->storageCap = 0;
  }
}

static void lazyInit() {
  if (s_inited) return;
  memset(s_slots, 0, sizeof(s_slots));
  s_msgIdCounter = (uint32_t)esp_random();
  s_inited = true;
}

static VoiceMeshTxProfile detectTxProfile(const uint8_t* plain, size_t plainLen) {
  VoiceMeshTxProfile p{voice_frag::VOICE_PROFILE_BALANCED, 9, protocol::CHANNEL_DEFAULT, 16};
  if (plain && plainLen >= 2 && plain[0] == 0xFE && plain[1] >= voice_frag::VOICE_PROFILE_FAST &&
      plain[1] <= voice_frag::VOICE_PROFILE_RESILIENT) {
    p.code = plain[1];
  }
  if (p.code == voice_frag::VOICE_PROFILE_FAST) {
    p.minSf = 7;
    p.channel = protocol::CHANNEL_DEFAULT;
    p.interFragDelayMs = 8;
  } else if (p.code == voice_frag::VOICE_PROFILE_RESILIENT) {
    p.minSf = 11;
    p.channel = protocol::CHANNEL_CRITICAL;
    p.interFragDelayMs = 30;
  }
  return p;
}

namespace voice_frag {

void init() {
  lazyInit();
}

void deinit() {
  for (int i = 0; i < VOICE_REASSEMBLE_MAX; i++) {
    freeSlotStorage(&s_slots[i]);
    s_slots[i].inUse = false;
  }
  s_inited = false;
}

static VoiceSlot* findSlot(uint32_t msgId, const uint8_t* from) {
  for (int i = 0; i < VOICE_REASSEMBLE_MAX; i++) {
    if (s_slots[i].inUse && s_slots[i].msgId == msgId &&
        memcmp(s_slots[i].from, from, protocol::NODE_ID_LEN) == 0)
      return &s_slots[i];
  }
  return nullptr;
}

static VoiceSlot* findFreeSlot() {
  uint32_t now = millis();
  for (int i = 0; i < VOICE_REASSEMBLE_MAX; i++) {
    if (!s_slots[i].inUse) return &s_slots[i];
    if (now - s_slots[i].lastTime > VOICE_TIMEOUT_MS) {
      s_slots[i].inUse = false;
      return &s_slots[i];
    }
  }
  // All slots busy — evict the oldest one to avoid permanent stall
  int oldestIdx = 0;
  uint32_t oldestTime = s_slots[0].lastTime;
  for (int i = 1; i < VOICE_REASSEMBLE_MAX; i++) {
    if ((int32_t)(s_slots[i].lastTime - oldestTime) < 0) {
      oldestTime = s_slots[i].lastTime;
      oldestIdx = i;
    }
  }
  Serial.printf("[RiftLink] VOICE_SLOT_EVICT idx=%d msgId=%u age_ms=%lu parts=%u/%u\n",
      oldestIdx, (unsigned)s_slots[oldestIdx].msgId,
      (unsigned long)(now - s_slots[oldestIdx].lastTime),
      (unsigned)s_slots[oldestIdx].partsReceivedUnique,
      (unsigned)s_slots[oldestIdx].partsTotal);
  s_slots[oldestIdx].inUse = false;
  return &s_slots[oldestIdx];
}

static void registerPendingAck(const uint8_t* to, uint32_t msgId) {
  uint32_t now = millis();
  int freeIdx = -1;
  int oldestIdx = 0;
  uint32_t oldestTs = UINT32_MAX;
  for (int i = 0; i < VOICE_PENDING_ACK_MAX; i++) {
    auto& e = s_pendingAcks[i];
    if (e.inUse && (now - e.sentAtMs) > VOICE_ACK_TIMEOUT_MS) e.inUse = false;
    if (!e.inUse && freeIdx < 0) freeIdx = i;
    if (e.sentAtMs < oldestTs) {
      oldestTs = e.sentAtMs;
      oldestIdx = i;
    }
  }
  int idx = (freeIdx >= 0) ? freeIdx : oldestIdx;
  auto& dst = s_pendingAcks[idx];
  dst.inUse = true;
  memcpy(dst.to, to, protocol::NODE_ID_LEN);
  dst.msgId = msgId;
  dst.sentAtMs = now;
}

bool send(const uint8_t* to, const uint8_t* data, size_t dataLen, uint32_t* outMsgId) {
  lazyInit();
  if (outMsgId) *outMsgId = 0;
  if (dataLen == 0 || dataLen > MAX_VOICE_PLAIN || node::isBroadcast(to)) {
    Serial.printf("[RiftLink] VOICE_SEND guard: len=%u max=%u broadcast=%d\n",
        (unsigned)dataLen, (unsigned)MAX_VOICE_PLAIN, node::isBroadcast(to) ? 1 : 0);
    return false;
  }
  VoiceMeshTxProfile txProfile = detectTxProfile(data, dataLen);

  uint8_t* encPtr = voice_buffers_cipher();
  size_t encLen = voice_buffers_cipher_cap();
  if (!encPtr) {
    Serial.printf("[RiftLink] VOICE_SEND no cipher buf\n");
    return false;
  }
  if (!crypto::encryptFor(to, data, dataLen, encPtr, &encLen)) {
    Serial.printf("[RiftLink] VOICE_SEND encryptFor failed len=%u\n", (unsigned)dataLen);
    return false;
  }

  uint32_t msgId = ++s_msgIdCounter;
  if (outMsgId) *outMsgId = msgId;
  uint8_t nFrags = (encLen + FRAG_DATA_MAX - 1) / FRAG_DATA_MAX;
  if (nFrags > 255) {
    Serial.printf("[RiftLink] VOICE_SEND too many frags: %u\n", (unsigned)nFrags);
    return false;
  }

  Serial.printf("[RiftLink] VOICE_SEND encLen=%u nFrags=%u profile=%u msgId=%u\n",
      (unsigned)encLen, (unsigned)nFrags, (unsigned)txProfile.code, (unsigned)msgId);

  for (uint8_t part = 1; part <= nFrags; part++) {
    size_t offset = (part - 1) * FRAG_DATA_MAX;
    size_t chunkLen = encLen - offset;
    if (chunkLen > FRAG_DATA_MAX) chunkLen = FRAG_DATA_MAX;

    uint8_t fragPayload[protocol::MAX_PAYLOAD];
    memcpy(fragPayload, &msgId, 4);
    fragPayload[4] = part;
    fragPayload[5] = nFrags;
    memcpy(fragPayload + FRAG_HEADER_LEN, encPtr + offset, chunkLen);

    uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD];
    uint8_t txSf = neighbors::rssiToSfOrthogonal(to);
    if (txSf == 0) txSf = neighbors::rssiToSf(neighbors::getRssiFor(to));
    if (txSf == 0) txSf = 12;
    if (txSf < txProfile.minSf) txSf = txProfile.minSf;
    size_t pktLen = protocol::buildPacket(pkt, sizeof(pkt),
        node::getId(), to, 31, protocol::OP_VOICE_MSG,
        fragPayload, FRAG_HEADER_LEN + chunkLen, true, false, false, txProfile.channel);
    if (pktLen > 0) {
      bool priority = (txProfile.channel == protocol::CHANNEL_CRITICAL);
      char reasonBuf[40];
      if (!queueTxPacket(pkt, pktLen, txSf, priority, TxRequestClass::voice, reasonBuf, sizeof(reasonBuf))) {
        uint32_t delayMs = 50u + (uint32_t)((part - 1) * 16u);
        queueDeferredSend(pkt, pktLen, txSf, delayMs, true);
        RIFTLINK_DIAG("VOICE", "event=VOICE_TX_DEFER part=%u/%u to=%02X%02X sf=%u cause=%s",
            (unsigned)part, (unsigned)nFrags, to[0], to[1], (unsigned)txSf, reasonBuf[0] ? reasonBuf : "?");
      }
      if (part < nFrags && txProfile.interFragDelayMs > 0) {
        delay(txProfile.interFragDelayMs);
      }
    }
  }
  registerPendingAck(to, msgId);
  Serial.printf("[RiftLink] VOICE_MSG sent %u bytes, %u frags, profile=%u sf>=%u msgId=%u\n",
      (unsigned)dataLen, (unsigned)nFrags, (unsigned)txProfile.code, (unsigned)txProfile.minSf, (unsigned)msgId);
  return true;
}

bool onFragment(const uint8_t* from, const uint8_t* to, const uint8_t* payload, size_t payloadLen,
                uint8_t* out, size_t outMaxLen, size_t* outLen, uint32_t* outMsgId) {
  (void)out;
  lazyInit();
  if (payloadLen < FRAG_HEADER_LEN || !outLen) {
    RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_RX_REJECT reason=header_short payloadLen=%u", (unsigned)payloadLen);
    return false;
  }
  *outLen = 0;
  if (outMsgId) *outMsgId = 0;

  uint32_t msgId;
  memcpy(&msgId, payload, 4);
  uint8_t part = payload[4];
  uint8_t total = payload[5];
  RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_RX from=%02X%02X msgId=%u part=%u/%u payloadLen=%u",
      from[0], from[1], (unsigned)msgId, (unsigned)part, (unsigned)total, (unsigned)payloadLen);
  if (part == 0 || total == 0 || part > total) {
    RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_RX_REJECT reason=bad_index msgId=%u part=%u total=%u", (unsigned)msgId, (unsigned)part, (unsigned)total);
    return false;
  }

  VoiceSlot* slot = findSlot(msgId, from);
  if (!slot) {
    slot = findFreeSlot();
    if (!slot) {
      RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_RX_DROP reason=no_slot msgId=%u from=%02X%02X", (unsigned)msgId, from[0], from[1]);
      return false;
    }
    if (!ensureSlotStorage(slot)) {
      RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_RX_DROP reason=oom msgId=%u from=%02X%02X", (unsigned)msgId, from[0], from[1]);
      return false;
    }
    slot->msgId = msgId;
    memcpy(slot->from, from, protocol::NODE_ID_LEN);
    memcpy(slot->to, to, protocol::NODE_ID_LEN);
    slot->partsTotal = total;
    slot->partsReceivedUnique = 0;
    slot->lastPartLen = 0;
    slot->hasLastPart = false;
    slot->partsMask = 0;
    slot->inUse = true;
    memset(slot->storage, 0, slot->storageCap);
  }
  if (slot->partsTotal != total) {
    RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_RX_REJECT reason=total_mismatch msgId=%u expected=%u got=%u",
        (unsigned)msgId, (unsigned)slot->partsTotal, (unsigned)total);
    return false;
  }

  size_t offset = (part - 1) * FRAG_DATA_MAX;
  size_t chunkLen = payloadLen - FRAG_HEADER_LEN;
  if (!slot->storage || offset + chunkLen > slot->storageCap) {
    RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_RX_REJECT reason=overflow msgId=%u part=%u offset=%u chunkLen=%u cap=%u",
        (unsigned)msgId, (unsigned)part, (unsigned)offset, (unsigned)chunkLen, (unsigned)(slot->storageCap));
    return false;
  }

  if (part < total && chunkLen != FRAG_DATA_MAX) {
    RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_RX_REJECT reason=bad_chunk_len msgId=%u part=%u/%u chunkLen=%u expected=%u",
        (unsigned)msgId, (unsigned)part, (unsigned)total, (unsigned)chunkLen, (unsigned)FRAG_DATA_MAX);
    return false;
  }
  if (part == total && (chunkLen == 0 || chunkLen > FRAG_DATA_MAX)) {
    RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_RX_REJECT reason=bad_last_chunk msgId=%u part=%u chunkLen=%u",
        (unsigned)msgId, (unsigned)part, (unsigned)chunkLen);
    return false;
  }

  const uint64_t partBit = (uint64_t)1u << (part - 1);
  if ((slot->partsMask & partBit) != 0) {
    slot->lastTime = millis();
    RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_RX_DUP msgId=%u part=%u/%u", (unsigned)msgId, (unsigned)part, (unsigned)total);
    return false;
  }

  memcpy(slot->storage + offset, payload + FRAG_HEADER_LEN, chunkLen);
  slot->partsMask |= partBit;
  slot->partsReceivedUnique++;
  if (part == total) {
    slot->lastPartLen = (uint16_t)chunkLen;
    slot->hasLastPart = true;
  }
  slot->lastTime = millis();

  RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_RX_OK msgId=%u part=%u/%u received=%u/%u mask=0x%llX",
      (unsigned)msgId, (unsigned)part, (unsigned)slot->partsTotal,
      (unsigned)slot->partsReceivedUnique, (unsigned)slot->partsTotal,
      (unsigned long long)slot->partsMask);

  if (slot->partsReceivedUnique < slot->partsTotal || !slot->hasLastPart) return false;

  RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_COMPLETE msgId=%u parts=%u from=%02X%02X",
      (unsigned)msgId, (unsigned)slot->partsTotal, from[0], from[1]);

  if (!voice_buffers_acquire()) {
    RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_DECRYPT_FAIL reason=buffers_acquire msgId=%u", (unsigned)msgId);
    slot->inUse = false;
    return false;
  }

  size_t encLen = (slot->partsTotal - 1) * FRAG_DATA_MAX + slot->lastPartLen;
  size_t decLen = 0;
  uint8_t* dest = voice_buffers_plain();
  if (!dest) {
    RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_DECRYPT_FAIL reason=no_plain_buf msgId=%u", (unsigned)msgId);
    voice_buffers_release();
    slot->inUse = false;
    return false;
  }
  if (!crypto::decryptFrom(slot->from, slot->storage, encLen, dest, &decLen)) {
    RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_DECRYPT_FAIL reason=decrypt msgId=%u encLen=%u from=%02X%02X",
        (unsigned)msgId, (unsigned)encLen, slot->from[0], slot->from[1]);
    voice_buffers_release();
    slot->inUse = false;
    return false;
  }

  if (decLen > outMaxLen || decLen > voice_buffers_plain_cap()) {
    RIFTLINK_DIAG("VOICE_FRAG", "event=FRAG_DECRYPT_FAIL reason=output_overflow msgId=%u decLen=%u outMax=%u bufCap=%u",
        (unsigned)msgId, (unsigned)decLen, (unsigned)outMaxLen, (unsigned)voice_buffers_plain_cap());
    voice_buffers_release();
    slot->inUse = false;
    return false;
  }
  *outLen = decLen;
  if (outMsgId) *outMsgId = msgId;
  slot->inUse = false;
  Serial.printf("[RiftLink] VOICE_MSG received %u bytes from %02X%02X msgId=%u\n",
      (unsigned)decLen, from[0], from[1], (unsigned)msgId);
  return true;
}

bool matchAck(const uint8_t* from, uint32_t msgId) {
  for (int i = 0; i < VOICE_PENDING_ACK_MAX; i++) {
    auto& e = s_pendingAcks[i];
    if (e.inUse && e.msgId == msgId && memcmp(e.to, from, protocol::NODE_ID_LEN) == 0) {
      e.inUse = false;
      return true;
    }
  }
  return false;
}

}  // namespace voice_frag
