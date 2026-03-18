/**
 * RiftLink Packet Layer — сборка и разбор пакетов
 */

#include "packet.h"
#include <string.h>

namespace protocol {

const uint8_t BROADCAST_ID[NODE_ID_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Header: version(4) + flags(4): encrypted=0x08, compressed=0x04, ack_req=0x02
constexpr uint8_t VERSION_MASK = 0xF0;
constexpr uint8_t FLAGS_MASK = 0x0F;
constexpr uint8_t FLAG_ENCRYPTED = 0x08;
constexpr uint8_t FLAG_COMPRESSED = 0x04;
constexpr uint8_t FLAG_ACK_REQ = 0x02;

size_t buildPacket(uint8_t* buf, size_t maxLen,
                  const uint8_t* from, const uint8_t* to,
                  uint8_t ttl, uint8_t opcode,
                  const uint8_t* payload, size_t payloadLen,
                  bool encrypted, bool ackReq, bool compressed, uint8_t channel) {
  if (maxLen < SYNC_LEN + HEADER_LEN + payloadLen) return 0;

  uint8_t* p = buf;
  *p++ = SYNC_BYTE;  // маркер начала — поиск при сдвиге (SX1262 corruption)
  *p++ = 0x10 | (encrypted ? FLAG_ENCRYPTED : 0) | (ackReq ? FLAG_ACK_REQ : 0) | (compressed ? FLAG_COMPRESSED : 0);
  memcpy(p, from, NODE_ID_LEN); p += NODE_ID_LEN;  // from first (CUSTOM_PROTOCOL_PLAN §4.2)
  memcpy(p, to, NODE_ID_LEN);   p += NODE_ID_LEN;
  *p++ = ttl;
  *p++ = opcode;
  *p++ = channel;
  if (payloadLen > 0) {
    memcpy(p, payload, payloadLen);
  }
  return SYNC_LEN + HEADER_LEN + payloadLen;
}

constexpr size_t HEADER_LEN_V0 = 19;  // Без channel (обратная совместимость)
constexpr int OPCODE_OFFSET_SYNC = 19;   // sync(1) + version(1) + from(8) + to(8) + ttl(1)
constexpr int OPCODE_OFFSET_LEGACY = 18; // version(1) + from(8) + to(8) + ttl(1)

static bool isValidOpcode(uint8_t op) {
  return (op >= 0x01 && op <= 0x0C) || op == OP_PONG || op == OP_PING;
}

// Найти смещение начала пакета: sync-first, затем opcode-at-offset
static int findPacketStart(const uint8_t* buf, size_t len) {
  if (len < HEADER_LEN_V0) return -1;
  // Стратегия 1: поиск SYNC_BYTE в первых 8 байтах
  for (int off = 0; off <= 8 && off + 1 + HEADER_LEN_V0 <= (int)len; off++) {
    if (buf[off] == SYNC_BYTE) {
      uint8_t v = buf[off + 1];
      if ((v & 0xF0) == 0x10 || (v & 0xF0) == 0x20 || (v & 0xF0) == 0x30) return off;
    }
  }
  // Стратегия 2: поиск opcode на фиксированном offset — синхронизация при сдвиге
  for (int i = OPCODE_OFFSET_SYNC; i < (int)len && i <= 32; i++) {
    if (!isValidOpcode(buf[i])) continue;
    int start = i - OPCODE_OFFSET_SYNC;
    if (start >= 0 && buf[start] == SYNC_BYTE) {
      uint8_t v = buf[start + 1];
      if ((v & 0xF0) == 0x10 || (v & 0xF0) == 0x20 || (v & 0xF0) == 0x30) return start;
    }
  }
  // Legacy: без sync
  uint8_t v0 = buf[0];
  if ((v0 & 0xF0) == 0x10 || (v0 & 0xF0) == 0x20 || (v0 & 0xF0) == 0x30) return 0;
  if (v0 == 0x00 && len > (size_t)OPCODE_OFFSET_LEGACY && buf[OPCODE_OFFSET_LEGACY] == OP_HELLO) return 0;
  return -1;
}

bool parsePacket(const uint8_t* buf, size_t len, PacketHeader* hdr, const uint8_t** payload, size_t* payloadLen) {
  if (len < HEADER_LEN_V0) return false;
  if (len > SYNC_LEN + HEADER_LEN + MAX_PAYLOAD + 64) return false;

  int off = findPacketStart(buf, len);
  if (off < 0) return false;

  const uint8_t* p = buf + off;
  size_t pLen = len - off;

  // С форматом sync: p[0]=0x5A, p[1]=version. Без sync: p[0]=version.
  bool hasSync = (p[0] == SYNC_BYTE);
  const uint8_t* h = hasSync ? (p + 1) : p;
  size_t hLen = pLen - (hasSync ? 1 : 0);

  if (hLen < HEADER_LEN_V0) return false;

  uint8_t v0 = h[0];
  if ((v0 & 0xF0) == 0x10 || (v0 & 0xF0) == 0x20 || (v0 & 0xF0) == 0x30) {
    hdr->version_flags = v0;
  } else if (v0 == 0x00 && h[1 + NODE_ID_LEN * 2 + 1] == OP_HELLO) {
    hdr->version_flags = 0x10;
  } else {
    return false;
  }

  memcpy(hdr->from, h + 1, NODE_ID_LEN);
  memcpy(hdr->to, h + 1 + NODE_ID_LEN, NODE_ID_LEN);
  if (hdr->from[0] == 0xFF && hdr->from[1] == 0xFF && (hdr->to[0] != 0xFF || hdr->to[1] != 0xFF)) {
    uint8_t tmp[NODE_ID_LEN];
    memcpy(tmp, hdr->from, NODE_ID_LEN);
    memcpy(hdr->from, hdr->to, NODE_ID_LEN);
    memcpy(hdr->to, tmp, NODE_ID_LEN);
  }
  hdr->ttl = h[1 + NODE_ID_LEN * 2];
  hdr->opcode = h[1 + NODE_ID_LEN * 2 + 1];
  hdr->channel = (hLen >= HEADER_LEN) ? h[1 + NODE_ID_LEN * 2 + 2] : CHANNEL_DEFAULT;

  if (hdr->opcode == OP_HELLO && hLen != HEADER_LEN_V0 && hLen != HEADER_LEN) return false;
  // HELLO: строгая длина — ровно 21 (sync) или 20 (legacy), иначе сдвиг/коррупция
  if (hdr->opcode == OP_HELLO) {
    size_t expectedLen = (hasSync ? SYNC_LEN : 0) + (hLen >= HEADER_LEN ? HEADER_LEN : HEADER_LEN_V0);
    if (pLen != expectedLen) return false;
  }

  size_t hdrLen = (hLen >= HEADER_LEN) ? HEADER_LEN : HEADER_LEN_V0;
  size_t logicalStart = off + (hasSync ? 1 : 0);
  size_t pl = len > logicalStart + hdrLen ? len - logicalStart - hdrLen : 0;
  if (payload) *payload = buf + logicalStart + hdrLen;
  if (payloadLen) *payloadLen = pl;

  // Валидация длины по opcode — отсечь неполные/превышающие
  size_t minPl, maxPl;
  if (getExpectedPayloadRange(hdr->opcode, &minPl, &maxPl)) {
    if (pl < minPl || pl > maxPl) return false;
  }
  size_t expected = getExpectedPacketLength(hdr->opcode, pl);
  if (expected != 0 && pLen != expected) return false;

  return true;
}

size_t getExpectedPacketLength(uint8_t opcode, size_t payloadLen) {
  switch (opcode) {
    case OP_HELLO:
    case OP_PING:
    case OP_PONG:
      return SYNC_LEN + HEADER_LEN;  // 21
    case OP_ACK:
    case OP_READ:
      return (payloadLen == 4) ? (SYNC_LEN + HEADER_LEN + 4) : 0;
    case OP_KEY_EXCHANGE:
      return (payloadLen == 32) ? (SYNC_LEN + HEADER_LEN + 32) : 0;
    case OP_ROUTE_REQ:
    case OP_ROUTE_REPLY:
      return (payloadLen == 21) ? (SYNC_LEN + HEADER_LEN + 21) : 0;
    default:
      return 0;  // переменная длина
  }
}

bool getExpectedPayloadRange(uint8_t opcode, size_t* minOut, size_t* maxOut) {
  if (!minOut || !maxOut) return false;
  switch (opcode) {
    case OP_HELLO:
    case OP_PING:
    case OP_PONG:
      *minOut = *maxOut = 0;
      return true;
    case OP_ACK:
    case OP_READ:
      *minOut = *maxOut = 4;
      return true;
    case OP_KEY_EXCHANGE:
      *minOut = *maxOut = 32;
      return true;
    case OP_ROUTE_REQ:
    case OP_ROUTE_REPLY:
      *minOut = *maxOut = 21;
      return true;
    case OP_TELEMETRY:
      *minOut = 28;  // 4 plain + crypto::OVERHEAD
      *maxOut = 48;
      return true;
    case OP_LOCATION:
      *minOut = 38;  // 10 plain + crypto::OVERHEAD
      *maxOut = MAX_PAYLOAD;
      return true;
    case OP_MSG:
      *minOut = 29;  // 1 plain + crypto::OVERHEAD
      *maxOut = MAX_PAYLOAD;
      return true;
    case OP_GROUP_MSG:
      *minOut = 32;  // 4 (groupId) + crypto::OVERHEAD
      *maxOut = MAX_PAYLOAD;
      return true;
    case OP_MSG_FRAG:
    case OP_VOICE_MSG:
      *minOut = 6;   // FRAG_HEADER_LEN / voice header
      *maxOut = MAX_PAYLOAD;
      return true;
    default:
      return false;
  }
}

bool isEncrypted(const PacketHeader& hdr) {
  return (hdr.version_flags & FLAG_ENCRYPTED) != 0;
}

bool isAckReq(const PacketHeader& hdr) {
  return (hdr.version_flags & FLAG_ACK_REQ) != 0;
}

bool isCompressed(const PacketHeader& hdr) {
  return (hdr.version_flags & FLAG_COMPRESSED) != 0;
}

}  // namespace protocol
