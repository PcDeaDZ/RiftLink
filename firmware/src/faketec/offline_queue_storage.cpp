/**
 * Offline queue на storage:: (InternalFS), паритет с firmware/src/offline_queue/offline_queue.cpp.
 */

#include "offline_queue/offline_queue.h"
#include "neighbors/neighbors.h"
#include "node/node.h"
#include "protocol/packet.h"
#include "async_tx.h"
#include "storage.h"
#include "log.h"
#include <Arduino.h>
#include <string.h>

#define NVS_KEY_OFFLINE "offline_q"
#define OFFLINE_FLAG_COMPRESSED 0x01
#define OFFLINE_FLAG_CRITICAL 0x02
#define OFFLINE_FLAG_COURIER 0x04

#pragma pack(push, 1)
struct StoredMsgNvs {
  uint8_t inUse;
  uint8_t to[protocol::NODE_ID_LEN];
  uint16_t payloadLen;
  uint8_t opcode;
  uint8_t flags;
  uint8_t payload[OFFLINE_MAX_LEN];
};
#pragma pack(pop)

struct StoredMsg {
  uint8_t to[protocol::NODE_ID_LEN];
  uint8_t payload[OFFLINE_MAX_LEN];
  size_t payloadLen;
  uint8_t opcode;
  uint8_t flags;
  uint32_t queuedAtMs;
  uint32_t seq;
  bool inUse;
};

static StoredMsg s_msgs[OFFLINE_MAX_MSGS];
static StoredMsgNvs s_nvsBuf[OFFLINE_MAX_MSGS];
static bool s_inited = false;
static bool s_dirty = false;
static uint32_t s_seqCounter = 0;
static uint32_t s_lastSaveMs = 0;

#define NVS_SAVE_INTERVAL_MS 2000
#define OFFLINE_EXPIRY_NORMAL_MS (6UL * 60UL * 60UL * 1000UL)
#define OFFLINE_EXPIRY_COURIER_MS (24UL * 60UL * 60UL * 1000UL)

static void loadFromStorage() {
  size_t len = sizeof(s_nvsBuf);
  if (!storage::getLargeBlob(NVS_KEY_OFFLINE, (uint8_t*)s_nvsBuf, &len) || len != sizeof(s_nvsBuf)) {
    return;
  }
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    s_msgs[i].inUse = (s_nvsBuf[i].inUse != 0);
    memcpy(s_msgs[i].to, s_nvsBuf[i].to, protocol::NODE_ID_LEN);
    s_msgs[i].payloadLen = s_nvsBuf[i].payloadLen;
    s_msgs[i].opcode = s_nvsBuf[i].opcode;
    s_msgs[i].flags = s_nvsBuf[i].flags;
    memcpy(s_msgs[i].payload, s_nvsBuf[i].payload, OFFLINE_MAX_LEN);
    s_msgs[i].queuedAtMs = millis();
    s_msgs[i].seq = ++s_seqCounter;
  }
}

static void persistToStorage() {
  RIFTLINK_DIAG("TRACE", "offline_persist_begin bytes=%u", (unsigned)sizeof(s_nvsBuf));
  const uint32_t t0 = millis();
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    s_nvsBuf[i].inUse = s_msgs[i].inUse ? 1 : 0;
    memcpy(s_nvsBuf[i].to, s_msgs[i].to, protocol::NODE_ID_LEN);
    s_nvsBuf[i].payloadLen = (uint16_t)s_msgs[i].payloadLen;
    s_nvsBuf[i].opcode = s_msgs[i].opcode;
    s_nvsBuf[i].flags = s_msgs[i].flags;
    memcpy(s_nvsBuf[i].payload, s_msgs[i].payload, OFFLINE_MAX_LEN);
  }
  (void)storage::setLargeBlob(NVS_KEY_OFFLINE, (const uint8_t*)s_nvsBuf, sizeof(s_nvsBuf));
  s_dirty = false;
  s_lastSaveMs = millis();
  RIFTLINK_DIAG("TRACE", "offline_persist_end ms=%lu", (unsigned long)(millis() - t0));
}

namespace offline_queue {

void init() {
  if (s_inited) return;
  memset(s_msgs, 0, sizeof(s_msgs));
  loadFromStorage();
  s_lastSaveMs = millis();
  s_inited = true;
}

static StoredMsg* findFree() {
  int oldestIdx = -1;
  uint32_t oldestSeq = 0xFFFFFFFFUL;
  int oldestCourierIdx = -1;
  uint32_t oldestCourierSeq = 0xFFFFFFFFUL;
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    if (!s_msgs[i].inUse) return &s_msgs[i];
    if ((s_msgs[i].flags & OFFLINE_FLAG_COURIER) != 0) {
      if (s_msgs[i].seq < oldestCourierSeq) {
        oldestCourierSeq = s_msgs[i].seq;
        oldestCourierIdx = i;
      }
    } else if (s_msgs[i].seq < oldestSeq) {
      oldestSeq = s_msgs[i].seq;
      oldestIdx = i;
    }
  }
  const int evictIdx = (oldestIdx >= 0) ? oldestIdx : oldestCourierIdx;
  if (evictIdx >= 0) return &s_msgs[evictIdx];
  return nullptr;
}

static bool isExpired(const StoredMsg* m, uint32_t nowMs) {
  if (!m || !m->inUse) return false;
  const uint32_t ttlMs = (m->flags & OFFLINE_FLAG_COURIER) ? OFFLINE_EXPIRY_COURIER_MS : OFFLINE_EXPIRY_NORMAL_MS;
  return (nowMs - m->queuedAtMs) > ttlMs;
}

bool enqueue(const uint8_t* to, const uint8_t* encPayload, size_t encLen, uint8_t opcode, uint8_t flags) {
  if (!s_inited || !to || encLen > OFFLINE_MAX_LEN) return false;
  StoredMsg* slot = findFree();
  if (!slot) return false;
  memcpy(slot->to, to, protocol::NODE_ID_LEN);
  memcpy(slot->payload, encPayload, encLen);
  slot->payloadLen = encLen;
  slot->opcode = opcode;
  slot->flags = flags;
  slot->queuedAtMs = millis();
  slot->seq = ++s_seqCounter;
  slot->inUse = true;
  s_dirty = true;
  return true;
}

bool enqueueCourier(const uint8_t* pkt, size_t len) {
  if (!s_inited || !pkt || len == 0 || len > OFFLINE_MAX_LEN) return false;
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  if (!protocol::parsePacket(pkt, len, &hdr, &pl, &plLen)) return false;
  if (node::isBroadcast(hdr.to)) return false;
  if (hdr.opcode != protocol::OP_MSG && hdr.opcode != protocol::OP_SOS) return false;
  StoredMsg* slot = findFree();
  if (!slot) return false;
  memcpy(slot->to, hdr.to, protocol::NODE_ID_LEN);
  memcpy(slot->payload, pkt, len);
  slot->payloadLen = len;
  slot->opcode = hdr.opcode;
  slot->flags = OFFLINE_FLAG_COURIER | ((hdr.channel == protocol::CHANNEL_CRITICAL) ? OFFLINE_FLAG_CRITICAL : 0);
  slot->queuedAtMs = millis();
  slot->seq = ++s_seqCounter;
  slot->inUse = true;
  s_dirty = true;
  return true;
}

static uint8_t s_onlinePktBuf[protocol::PAYLOAD_OFFSET + OFFLINE_MAX_LEN];

void onNodeOnline(const uint8_t* nodeId) {
  if (!s_inited || !nodeId) return;
  bool modified = false;
  const uint32_t now = millis();
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    StoredMsg* m = &s_msgs[i];
    if (!m->inUse) continue;
    if (isExpired(m, now)) {
      m->inUse = false;
      modified = true;
      continue;
    }
    if (memcmp(m->to, nodeId, protocol::NODE_ID_LEN) != 0) continue;

    uint8_t* pkt = s_onlinePktBuf;
    size_t len = 0;
    const bool isCourier = (m->flags & OFFLINE_FLAG_COURIER) != 0;
    const bool isCritical = (m->flags & OFFLINE_FLAG_CRITICAL) != 0;
    if (isCourier) {
      if (m->payloadLen <= sizeof(s_onlinePktBuf)) {
        memcpy(pkt, m->payload, m->payloadLen);
        len = m->payloadLen;
      }
    } else {
      const bool compressed = (m->flags & OFFLINE_FLAG_COMPRESSED) != 0;
      const uint8_t channel = isCritical ? protocol::CHANNEL_CRITICAL : protocol::CHANNEL_DEFAULT;
      len = protocol::buildPacket(pkt, sizeof(s_onlinePktBuf), node::getId(), m->to, 31, m->opcode, m->payload,
          m->payloadLen, true, true, compressed, channel);
    }
    if (len > 0) {
      const uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(nodeId));
      char reasonBuf[40];
      const bool queued = queueTxPacket(pkt, len, txSf, true,
          isCritical ? TxRequestClass::critical : TxRequestClass::data, reasonBuf, sizeof(reasonBuf));
      if (queued) {
        RIFTLINK_LOG_EVENT("[RiftLink] Offline delivery to %02X%02X\n", nodeId[0], nodeId[1]);
        m->inUse = false;
        modified = true;
      } else {
        queueDeferredSend(pkt, len, txSf, 90, true);
        RIFTLINK_DIAG("OFFLINE", "event=OFFLINE_TX_DEFER to=%02X%02X cause=%s", nodeId[0], nodeId[1],
            reasonBuf[0] ? reasonBuf : "?");
      }
    } else {
      if (!isCourier) {
        RIFTLINK_DIAG("OFFLINE", "event=OFFLINE_BUILD_FAIL to=%02X%02X op=0x%02X pl=%u max_payload=%u", m->to[0],
            m->to[1], (unsigned)m->opcode, (unsigned)m->payloadLen, (unsigned)protocol::MAX_PAYLOAD);
      }
      m->inUse = false;
      modified = true;
    }
  }
  if (modified) s_dirty = true;
}

int getPendingCount() {
  int n = 0;
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    if (s_msgs[i].inUse) n++;
  }
  return n;
}

int getCourierPendingCount() {
  int n = 0;
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    if (s_msgs[i].inUse && (s_msgs[i].flags & OFFLINE_FLAG_COURIER) != 0) n++;
  }
  return n;
}

int getDirectPendingCount() {
  int n = 0;
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    if (s_msgs[i].inUse && (s_msgs[i].flags & OFFLINE_FLAG_COURIER) == 0) n++;
  }
  return n;
}

void update() {
  if (!s_inited || !s_dirty) return;
  const uint32_t now = millis();
  if ((uint32_t)(now - s_lastSaveMs) < NVS_SAVE_INTERVAL_MS) return;
  persistToStorage();
}

}  // namespace offline_queue
