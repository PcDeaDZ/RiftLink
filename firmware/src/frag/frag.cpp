/**
 * RiftLink — фрагментация и сборка MSG_FRAG
 */

#include "frag.h"
#include "neighbors/neighbors.h"
#include "node/node.h"
#include "radio/radio.h"
#include "crypto/crypto.h"
#include "compress/compress.h"
#include <Arduino.h>
#include <esp_random.h>
#include <string.h>

#define FRAG_REASSEMBLE_MAX  4       // макс. собираемых сообщений одновременно
#define FRAG_TIMEOUT_MS      60000  // таймаут неполного сообщения

struct ReassembleSlot {
  uint32_t msgId;
  uint8_t from[protocol::NODE_ID_LEN];
  uint8_t to[protocol::NODE_ID_LEN];
  uint8_t* buf;
  size_t bufSize;
  uint8_t partsReceived;
  uint8_t partsTotal;
  uint32_t lastTime;
  bool compressed;
  bool inUse;
};

static ReassembleSlot s_slots[FRAG_REASSEMBLE_MAX];
static uint32_t s_msgIdCounter = 0;
static bool s_inited = false;

namespace frag {

void init() {
  memset(s_slots, 0, sizeof(s_slots));
  for (int i = 0; i < FRAG_REASSEMBLE_MAX; i++) {
    s_slots[i].buf = (uint8_t*)malloc(4096);  // encrypted blob
    s_slots[i].bufSize = 4096;
  }
  s_msgIdCounter = (uint32_t)esp_random();
  s_inited = true;
}

static ReassembleSlot* findSlot(uint32_t msgId, const uint8_t* from) {
  for (int i = 0; i < FRAG_REASSEMBLE_MAX; i++) {
    if (s_slots[i].inUse && s_slots[i].msgId == msgId &&
        memcmp(s_slots[i].from, from, protocol::NODE_ID_LEN) == 0)
      return &s_slots[i];
  }
  return nullptr;
}

static ReassembleSlot* findFreeSlot() {
  uint32_t now = millis();
  for (int i = 0; i < FRAG_REASSEMBLE_MAX; i++) {
    if (!s_slots[i].inUse) return &s_slots[i];
    if (now - s_slots[i].lastTime > FRAG_TIMEOUT_MS) {
      s_slots[i].inUse = false;
      return &s_slots[i];
    }
  }
  return nullptr;
}

bool send(const uint8_t* to, const uint8_t* plain, size_t plainLen, bool compressed) {
  if (!s_inited || plainLen == 0 || plainLen > MAX_MSG_PLAIN) return false;

  uint8_t toEncrypt[MAX_MSG_PLAIN + 512];
  size_t toEncryptLen = plainLen;
  bool useCompressed = compressed;

  if (compressed) {
    uint8_t compBuf[MAX_MSG_PLAIN];
    size_t compLen = compress::compress(plain, plainLen, compBuf, sizeof(compBuf));
    if (compLen > 0) {
      memcpy(toEncrypt, compBuf, compLen);
      toEncryptLen = compLen;
    } else {
      useCompressed = false;
    }
  }
  if (!useCompressed) {
    memcpy(toEncrypt, plain, plainLen);
  }

  uint8_t encBuf[MAX_MSG_PLAIN + 512 + crypto::OVERHEAD];
  size_t encLen = sizeof(encBuf);
  if (!crypto::encryptFor(to, toEncrypt, toEncryptLen, encBuf, &encLen)) return false;

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
    size_t pktLen = protocol::buildPacket(pkt, sizeof(pkt),
        node::getId(), to, 31, protocol::OP_MSG_FRAG,
        fragPayload, FRAG_HEADER_LEN + chunkLen, false, false, useCompressed);
    if (pktLen > 0) radio::send(pkt, pktLen, neighbors::rssiToSf(neighbors::getRssiFor(to)));
  }
  return true;
}

bool onFragment(const uint8_t* from, const uint8_t* to, const uint8_t* payload, size_t payloadLen,
                bool compressed, uint8_t* out, size_t outMaxLen, size_t* outLen) {
  if (!s_inited || payloadLen < FRAG_HEADER_LEN) return false;
  *outLen = 0;

  uint32_t msgId;
  memcpy(&msgId, payload, 4);
  uint8_t part = payload[4];
  uint8_t total = payload[5];
  if (part == 0 || total == 0 || part > total) return false;

  ReassembleSlot* slot = findSlot(msgId, from);
  if (!slot) {
    slot = findFreeSlot();
    if (!slot) return false;
    slot->msgId = msgId;
    memcpy(slot->from, from, protocol::NODE_ID_LEN);
    memcpy(slot->to, to, protocol::NODE_ID_LEN);
    slot->partsTotal = total;
    slot->partsReceived = 0;
    slot->compressed = compressed;
    slot->inUse = true;
    memset(slot->buf, 0, (size_t)total * FRAG_DATA_MAX);
  }
  if (slot->partsTotal != total) return false;

  size_t offset = (part - 1) * FRAG_DATA_MAX;
  size_t chunkLen = payloadLen - FRAG_HEADER_LEN;
  if (offset + chunkLen > slot->bufSize) return false;

  memcpy(slot->buf + offset, payload + FRAG_HEADER_LEN, chunkLen);
  slot->partsReceived++;
  slot->lastTime = millis();

  if (slot->partsReceived < slot->partsTotal) return false;

  size_t encLen = (slot->partsTotal - 1) * FRAG_DATA_MAX + chunkLen;
  uint8_t decBuf[MAX_MSG_PLAIN + 512];
  size_t decLen = 0;
  if (!crypto::decryptFrom(slot->from, slot->buf, encLen, decBuf, &decLen)) {
    slot->inUse = false;
    return false;
  }

  uint8_t* plain = decBuf;
  size_t plainLen = decLen;
  uint8_t decompBuf[MAX_MSG_PLAIN];
  if (slot->compressed) {
    size_t d = compress::decompress(decBuf, decLen, decompBuf, sizeof(decompBuf));
    if (d > 0) {
      plain = decompBuf;
      plainLen = d;
    }
  }

  if (plainLen > outMaxLen) {
    slot->inUse = false;
    return false;
  }
  memcpy(out, plain, plainLen);
  *outLen = plainLen;
  slot->inUse = false;
  return true;
}

}  // namespace frag
