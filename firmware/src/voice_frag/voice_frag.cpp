/**
 * RiftLink — VOICE_MSG фрагментация
 */

#include "voice_frag.h"
#include "neighbors/neighbors.h"
#include "node/node.h"
#include "radio/radio.h"
#include "crypto/crypto.h"
#include <Arduino.h>
#include <esp_random.h>
#include <string.h>

#define FRAG_HEADER_LEN 6
#define FRAG_DATA_MAX (protocol::MAX_PAYLOAD - FRAG_HEADER_LEN)
#if defined(USE_EINK)
#define VOICE_REASSEMBLE_MAX 1   // Paper: 1 слот ~11 KB (экономия heap)
#else
#define VOICE_REASSEMBLE_MAX 2
#endif
#define VOICE_TIMEOUT_MS 60000

struct VoiceMeshTxProfile {
  uint8_t code;
  uint8_t minSf;
  uint8_t channel;
  uint16_t interFragDelayMs;
};

/** Пул слотов в BSS — без malloc на каждую сборку (цена: постоянный RAM на слот). */
struct VoiceSlot {
  uint32_t msgId;
  uint8_t from[protocol::NODE_ID_LEN];
  uint8_t to[protocol::NODE_ID_LEN];
  uint8_t storage[voice_frag::MAX_VOICE_PLAIN + 1024];
  uint8_t partsReceived;
  uint8_t partsTotal;
  uint32_t lastTime;
  bool inUse;
};

static VoiceSlot s_slots[VOICE_REASSEMBLE_MAX];
static uint32_t s_msgIdCounter = 0;
static bool s_inited = false;

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
  return nullptr;
}

bool send(const uint8_t* to, const uint8_t* data, size_t dataLen) {
  lazyInit();
  if (dataLen == 0 || dataLen > MAX_VOICE_PLAIN || node::isBroadcast(to)) return false;
  VoiceMeshTxProfile txProfile = detectTxProfile(data, dataLen);

  uint8_t encBuf[MAX_VOICE_PLAIN + 1024 + crypto::OVERHEAD];
  size_t encLen = sizeof(encBuf);
  if (!crypto::encryptFor(to, data, dataLen, encBuf, &encLen)) return false;

  uint32_t msgId = ++s_msgIdCounter;
  uint8_t nFrags = (encLen + FRAG_DATA_MAX - 1) / FRAG_DATA_MAX;
  if (nFrags > 255) return false;

  for (uint8_t part = 1; part <= nFrags; part++) {
    size_t offset = (part - 1) * FRAG_DATA_MAX;
    size_t chunkLen = encLen - offset;
    if (chunkLen > FRAG_DATA_MAX) chunkLen = FRAG_DATA_MAX;

    uint8_t fragPayload[protocol::MAX_PAYLOAD];
    memcpy(fragPayload, &msgId, 4);
    fragPayload[4] = part;
    fragPayload[5] = nFrags;
    memcpy(fragPayload + FRAG_HEADER_LEN, encBuf + offset, chunkLen);

    uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD];
    uint8_t txSf = neighbors::rssiToSfOrthogonal(to);
    if (txSf == 0) txSf = neighbors::rssiToSf(neighbors::getRssiFor(to));
    if (txSf == 0) txSf = 12;
    if (txSf < txProfile.minSf) txSf = txProfile.minSf;
    size_t pktLen = protocol::buildPacket(pkt, sizeof(pkt),
        node::getId(), to, 31, protocol::OP_VOICE_MSG,
        fragPayload, FRAG_HEADER_LEN + chunkLen, true, false, false, txProfile.channel);
    if (pktLen > 0) {
      radio::send(pkt, pktLen, txSf, txProfile.channel == protocol::CHANNEL_CRITICAL);
      if (part < nFrags && txProfile.interFragDelayMs > 0) {
        delay(txProfile.interFragDelayMs);
      }
    }
  }
  Serial.printf("[RiftLink] VOICE_MSG sent %u bytes, %u frags, profile=%u sf>=%u\n",
      (unsigned)dataLen, (unsigned)nFrags, (unsigned)txProfile.code, (unsigned)txProfile.minSf);
  return true;
}

bool onFragment(const uint8_t* from, const uint8_t* to, const uint8_t* payload, size_t payloadLen,
                uint8_t* out, size_t outMaxLen, size_t* outLen) {
  lazyInit();
  if (payloadLen < FRAG_HEADER_LEN || !out || !outLen) return false;
  *outLen = 0;

  uint32_t msgId;
  memcpy(&msgId, payload, 4);
  uint8_t part = payload[4];
  uint8_t total = payload[5];
  if (part == 0 || total == 0 || part > total) return false;

  VoiceSlot* slot = findSlot(msgId, from);
  if (!slot) {
    slot = findFreeSlot();
    if (!slot) return false;
    slot->msgId = msgId;
    memcpy(slot->from, from, protocol::NODE_ID_LEN);
    memcpy(slot->to, to, protocol::NODE_ID_LEN);
    slot->partsTotal = total;
    slot->partsReceived = 0;
    slot->inUse = true;
    memset(slot->storage, 0, (size_t)total * FRAG_DATA_MAX);
  }
  if (slot->partsTotal != total) return false;

  size_t offset = (part - 1) * FRAG_DATA_MAX;
  size_t chunkLen = payloadLen - FRAG_HEADER_LEN;
  if (offset + chunkLen > sizeof(slot->storage)) return false;

  memcpy(slot->storage + offset, payload + FRAG_HEADER_LEN, chunkLen);
  slot->partsReceived++;
  slot->lastTime = millis();

  if (slot->partsReceived < slot->partsTotal) return false;

  size_t encLen = (slot->partsTotal - 1) * FRAG_DATA_MAX + chunkLen;
  uint8_t decBuf[MAX_VOICE_PLAIN + 1024];
  size_t decLen = 0;
  if (!crypto::decryptFrom(slot->from, slot->storage, encLen, decBuf, &decLen)) {
    slot->inUse = false;
    return false;
  }

  if (decLen > outMaxLen) {
    slot->inUse = false;
    return false;
  }
  memcpy(out, decBuf, decLen);
  *outLen = decLen;
  slot->inUse = false;
  Serial.printf("[RiftLink] VOICE_MSG received %u bytes from %02X%02X\n",
      (unsigned)decLen, from[0], from[1]);
  return true;
}

}  // namespace voice_frag
