/**
 * RiftLink — фрагментация и сборка MSG_FRAG
 */

#include "frag/frag.h"
#include "neighbors.h"
#include "node.h"
#include "radio.h"
#include "async_tx.h"
#include "log.h"
#include "crypto.h"
#include "compress/compress.h"
#include <Arduino.h>
#include <string.h>

#define FRAG_REASSEMBLE_MAX  2       // макс. собираемых сообщений одновременно
#define FRAG_TIMEOUT_MS      60000  // таймаут неполного сообщения
/** Раньше malloc(4096) на слот — теперь фиксированный пул в BSS. */
#define FRAG_SLOT_BUF 4096

struct ReassembleSlot {
  uint32_t msgId;
  uint8_t from[protocol::NODE_ID_LEN];
  uint8_t to[protocol::NODE_ID_LEN];
  uint8_t storage[FRAG_SLOT_BUF];
  uint8_t partsReceivedUnique;
  uint8_t partsTotal;
  uint16_t lastPartLen;
  bool hasLastPart;
  uint32_t partsMask;
  uint32_t lastTime;
  bool compressed;
  bool inUse;
};

static ReassembleSlot s_slots[FRAG_REASSEMBLE_MAX];
static uint32_t s_msgIdCounter = 0;
static bool s_inited = false;
struct TxFragSlot {
  uint32_t msgId;
  uint8_t to[protocol::NODE_ID_LEN];
  uint8_t total;
  uint8_t fragDataLen;
  uint8_t parts[frag::MAX_FRAGMENTS][frag::FRAG_DATA_MAX];
  uint16_t partLens[frag::MAX_FRAGMENTS];
  bool inUse;
};
static TxFragSlot s_txSlots[2];

static TxFragSlot* findTxSlot(uint32_t msgId, const uint8_t* to) {
  for (int i = 0; i < 2; i++) {
    if (!s_txSlots[i].inUse) continue;
    if (s_txSlots[i].msgId == msgId && memcmp(s_txSlots[i].to, to, protocol::NODE_ID_LEN) == 0) return &s_txSlots[i];
  }
  return nullptr;
}

static TxFragSlot* allocTxSlot(uint32_t msgId, const uint8_t* to) {
  TxFragSlot* slot = nullptr;
  for (int i = 0; i < 2; i++) {
    if (!s_txSlots[i].inUse) { slot = &s_txSlots[i]; break; }
  }
  if (!slot) slot = &s_txSlots[0];
  memset(slot, 0, sizeof(*slot));
  slot->inUse = true;
  slot->msgId = msgId;
  memcpy(slot->to, to, protocol::NODE_ID_LEN);
  return slot;
}

namespace frag {

void init() {
  memset(s_slots, 0, sizeof(s_slots));
  memset(s_txSlots, 0, sizeof(s_txSlots));
  s_msgIdCounter = (uint32_t)random(0x7fffffff);
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
  int ackRate = neighbors::getAckRatePermille(to);
  int rssi = neighbors::getRssiFor(to);
  size_t fragDataLen = FRAG_DATA_MAX;
  if ((ackRate >= 0 && ackRate < 600) || (rssi != -128 && rssi < -103)) {
    fragDataLen = FRAG_DATA_MAX - 48;
  } else if ((ackRate >= 0 && ackRate < 750) || (rssi != -128 && rssi < -95)) {
    fragDataLen = FRAG_DATA_MAX - 24;
  }
  if (fragDataLen < 64) fragDataLen = 64;
  uint8_t nFrags = (uint8_t)((encLen + fragDataLen - 1) / fragDataLen);
  if (nFrags == 0 || nFrags > MAX_FRAGMENTS) return false;
  TxFragSlot* txSlot = allocTxSlot(msgId, to);
  txSlot->total = nFrags;
  txSlot->fragDataLen = (uint8_t)fragDataLen;

  for (uint8_t part = 1; part <= nFrags; part++) {
    size_t offset = (part - 1) * fragDataLen;
    size_t chunkLen = encLen - offset;
    if (chunkLen > fragDataLen) chunkLen = fragDataLen;

    uint8_t fragPayload[protocol::MAX_PAYLOAD];
    memcpy(fragPayload, &msgId, 4);
    fragPayload[4] = part;
    fragPayload[5] = nFrags;
    fragPayload[6] = (uint8_t)fragDataLen;
    fragPayload[7] = useCompressed ? 0x01 : 0x00;
    memcpy(fragPayload + FRAG_HEADER_LEN, encBuf + offset, chunkLen);
    if (txSlot && part <= MAX_FRAGMENTS) {
      memcpy(txSlot->parts[part - 1], encBuf + offset, chunkLen);
      txSlot->partLens[part - 1] = (uint16_t)chunkLen;
    }

    uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD];
    size_t pktLen = protocol::buildPacket(pkt, sizeof(pkt),
        node::getId(), to, 31, protocol::OP_MSG_FRAG,
        fragPayload, FRAG_HEADER_LEN + chunkLen, false, false, useCompressed);
    if (pktLen > 0) {
      uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(to));
      char reasonBuf[40];
      if (!queueTxPacket(pkt, pktLen, txSf, false, TxRequestClass::data, reasonBuf, sizeof(reasonBuf))) {
        // Keep frag order predictable: part index contributes to deferred delay.
        uint32_t delayMs = 60u + (uint32_t)((part - 1) * 18u);
        queueDeferredSend(pkt, pktLen, txSf, delayMs, true);
        RIFTLINK_DIAG("FRAG", "event=FRAG_TX_DEFER part=%u/%u to=%02X%02X cause=%s",
            (unsigned)part, (unsigned)nFrags, to[0], to[1], reasonBuf[0] ? reasonBuf : "?");
      }
    }
  }
  return true;
}

bool onFragment(const uint8_t* from, const uint8_t* to, const uint8_t* payload, size_t payloadLen,
                bool compressed, uint8_t* out, size_t outMaxLen, size_t* outLen, uint32_t* outMsgId) {
  if (!s_inited || payloadLen < FRAG_HEADER_LEN || !outLen || !outMsgId) return false;
  *outLen = 0;
  *outMsgId = 0;

  uint32_t msgId;
  memcpy(&msgId, payload, 4);
  if (msgId == 0) return false;  // strict: MSG_FRAG без msgId не поддерживаем
  uint8_t part = payload[4];
  uint8_t total = payload[5];
  uint8_t fragDataLen = payload[6];
  if (part == 0 || total == 0 || part > total || total > MAX_FRAGMENTS) return false;
  if (fragDataLen == 0 || fragDataLen > FRAG_DATA_MAX) return false;

  ReassembleSlot* slot = findSlot(msgId, from);
  if (!slot) {
    slot = findFreeSlot();
    if (!slot) return false;
    slot->msgId = msgId;
    memcpy(slot->from, from, protocol::NODE_ID_LEN);
    memcpy(slot->to, to, protocol::NODE_ID_LEN);
    slot->partsTotal = total;
    slot->partsReceivedUnique = 0;
    slot->lastPartLen = 0;
    slot->hasLastPart = false;
    slot->partsMask = 0;
    slot->compressed = compressed;
    slot->inUse = true;
    const size_t reserve = (size_t)total * fragDataLen;
    if (reserve > sizeof(slot->storage)) {
      slot->inUse = false;
      return false;
    }
    memset(slot->storage, 0, reserve);
  }
  if (slot->partsTotal != total) return false;

  size_t offset = (part - 1) * fragDataLen;
  size_t chunkLen = payloadLen - FRAG_HEADER_LEN;
  if (offset + chunkLen > sizeof(slot->storage)) return false;
  if (part < total && chunkLen != fragDataLen) return false;
  if (part == total && (chunkLen == 0 || chunkLen > fragDataLen)) return false;

  const uint32_t partBit = (uint32_t)1u << (part - 1);
  if ((slot->partsMask & partBit) != 0) {
    slot->lastTime = millis();
    return false;  // duplicate part: ignore without affecting completion counters
  }

  memcpy(slot->storage + offset, payload + FRAG_HEADER_LEN, chunkLen);
  slot->partsMask |= partBit;
  slot->partsReceivedUnique++;
  if (part == total) {
    slot->lastPartLen = (uint16_t)chunkLen;
    slot->hasLastPart = true;
  }
  slot->lastTime = millis();

  if (slot->partsReceivedUnique < slot->partsTotal || !slot->hasLastPart) {
    uint8_t ctrlPlain[10];
    ctrlPlain[0] = 0x01;  // frag selective status
    memcpy(ctrlPlain + 1, &msgId, 4);
    ctrlPlain[5] = total;
    ctrlPlain[6] = (uint8_t)(slot->partsMask & 0xFF);
    ctrlPlain[7] = (uint8_t)((slot->partsMask >> 8) & 0xFF);
    ctrlPlain[8] = (uint8_t)((slot->partsMask >> 16) & 0xFF);
    ctrlPlain[9] = (uint8_t)((slot->partsMask >> 24) & 0xFF);
    uint8_t ctrlEnc[32];
    size_t ctrlEncLen = sizeof(ctrlEnc);
    if (crypto::encryptFor(from, ctrlPlain, sizeof(ctrlPlain), ctrlEnc, &ctrlEncLen)) {
      uint8_t ctrlPkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD];
      size_t ctrlLen = protocol::buildPacket(ctrlPkt, sizeof(ctrlPkt),
          node::getId(), from, 31, protocol::OP_FRAG_CTRL,
          ctrlEnc, ctrlEncLen, true, false, false, protocol::CHANNEL_CRITICAL, (uint16_t)(msgId & 0xFFFF));
      if (ctrlLen > 0) {
        uint8_t txSf = neighbors::rssiToSfOrthogonal(from);
        if (txSf == 0) txSf = 12;
        queueDeferredAck(ctrlPkt, ctrlLen, txSf, 30, false);
      }
    }
    return false;
  }

  size_t encLen = (slot->partsTotal - 1) * fragDataLen + slot->lastPartLen;
  static uint8_t decBuf[MAX_MSG_PLAIN + 512];
  size_t decLen = 0;
  if (!crypto::decryptFrom(slot->from, slot->storage, encLen, decBuf, &decLen)) {
    slot->inUse = false;
    return false;
  }

  uint8_t* plain = decBuf;
  size_t plainLen = decLen;
  static uint8_t decompBuf[MAX_MSG_PLAIN];
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
  *outMsgId = msgId;
  slot->inUse = false;
  return true;
}

void onFragCtrl(const uint8_t* from, uint32_t msgId, uint8_t total, uint32_t receivedMask) {
  if (!from || total == 0 || total > MAX_FRAGMENTS) return;
  TxFragSlot* slot = findTxSlot(msgId, from);
  if (!slot || !slot->inUse) return;
  uint32_t fullMask = (total >= 32) ? 0xFFFFFFFFu : ((1u << total) - 1u);
  uint32_t missing = fullMask & ~receivedMask;
  uint8_t txSf = neighbors::rssiToSfOrthogonal(from);
  if (txSf == 0) txSf = 12;
  for (uint8_t i = 0; i < total; i++) {
    if (((missing >> i) & 0x01u) == 0) continue;
    uint16_t chunkLen = slot->partLens[i];
    if (chunkLen == 0 || chunkLen > FRAG_DATA_MAX) continue;
    uint8_t fragPayload[protocol::MAX_PAYLOAD];
    memcpy(fragPayload, &msgId, 4);
    fragPayload[4] = (uint8_t)(i + 1);
    fragPayload[5] = total;
    fragPayload[6] = slot->fragDataLen;
    fragPayload[7] = 0;
    memcpy(fragPayload + FRAG_HEADER_LEN, slot->parts[i], chunkLen);
    uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD];
    size_t pktLen = protocol::buildPacket(pkt, sizeof(pkt),
        node::getId(), from, 31, protocol::OP_MSG_FRAG,
        fragPayload, FRAG_HEADER_LEN + chunkLen, false, false, false, protocol::CHANNEL_DEFAULT, (uint16_t)(msgId & 0xFFFF));
    if (pktLen > 0) {
      (void)queueTxPacket(pkt, pktLen, txSf, true, TxRequestClass::voice);
    }
  }
}

}  // namespace frag
