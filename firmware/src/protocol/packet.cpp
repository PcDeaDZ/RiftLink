/**
 * RiftLink Packet Layer — сборка и разбор пакетов
 */

#include "packet.h"
#include <string.h>

namespace protocol {

const uint8_t BROADCAST_ID[NODE_ID_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Header: version(4) + flags(4): encrypted=0x08, compressed=0x04, ack_req=0x02, broadcast=0x01
constexpr uint8_t VERSION_MASK = 0xF0;
constexpr uint8_t FLAGS_MASK = 0x0F;
constexpr uint8_t FLAG_ENCRYPTED = 0x08;
constexpr uint8_t FLAG_COMPRESSED = 0x04;
constexpr uint8_t FLAG_ACK_REQ = 0x02;
constexpr uint8_t FLAG_BROADCAST = 0x01;
constexpr uint8_t VERSION_V2 = 0x20;  // opcode first, compact broadcast

static bool isBroadcastTo(const uint8_t* to) {
  for (size_t i = 0; i < NODE_ID_LEN; i++) {
    if (to[i] != 0xFF) return false;
  }
  return true;
}

size_t buildPacket(uint8_t* buf, size_t maxLen,
                  const uint8_t* from, const uint8_t* to,
                  uint8_t ttl, uint8_t opcode,
                  const uint8_t* payload, size_t payloadLen,
                  bool encrypted, bool ackReq, bool compressed, uint8_t channel) {
  bool broadcast = isBroadcastTo(to);
  size_t hdrLen = broadcast ? HEADER_LEN_BROADCAST : (SYNC_LEN + HEADER_LEN);
  if (maxLen < hdrLen + payloadLen) return 0;

  uint8_t* p = buf;
  *p++ = SYNC_BYTE;
  *p++ = VERSION_V2 | (broadcast ? FLAG_BROADCAST : 0) |
         (encrypted ? FLAG_ENCRYPTED : 0) | (ackReq ? FLAG_ACK_REQ : 0) | (compressed ? FLAG_COMPRESSED : 0);
  *p++ = opcode;  // opcode первым — быстрая идентификация
  memcpy(p, from, NODE_ID_LEN);
  p += NODE_ID_LEN;
  if (!broadcast) {
    memcpy(p, to, NODE_ID_LEN);
    p += NODE_ID_LEN;
  }
  *p++ = ttl;
  *p++ = channel;
  if (payloadLen > 0) {
    memcpy(p, payload, payloadLen);
  }
  return hdrLen + payloadLen;
}

constexpr size_t HEADER_LEN_V0 = 19;  // Без channel (обратная совместимость)
constexpr int OPCODE_OFFSET_V1 = 19;   // sync(1) + version(1) + from(8) + to(8) + ttl(1)
constexpr int OPCODE_OFFSET_V2 = 2;    // sync(1) + version(1)
constexpr int OPCODE_OFFSET_LEGACY = 18; // version(1) + from(8) + to(8) + ttl(1)

static bool isValidOpcode(uint8_t op) {
  return (op >= 0x01 && op <= 0x0C) || op == OP_PONG || op == OP_PING;
}

// Найти смещение начала пакета: sync-first, затем opcode-at-offset (v1 или v2)
static int findPacketStart(const uint8_t* buf, size_t len) {
  if (len < HEADER_LEN_BROADCAST) return -1;  // v2 broadcast HELLO = 13 байт
  // Стратегия 1: поиск SYNC_BYTE в первых 8 байтах
  for (int off = 0; off <= 8 && off + HEADER_LEN_BROADCAST <= (int)len; off++) {
    if (buf[off] == SYNC_BYTE) {
      uint8_t v = buf[off + 1];
      if ((v & 0xF0) == 0x10 || (v & 0xF0) == 0x20 || (v & 0xF0) == 0x30) return off;
    }
  }
  // Стратегия 2: поиск opcode на offset v2 (2) или v1 (19)
  for (int i = 0; i < (int)len && i <= 32; i++) {
    if (!isValidOpcode(buf[i])) continue;
    int start = i - OPCODE_OFFSET_V2;
    if (start >= 0 && start + 2 <= (int)len && buf[start] == SYNC_BYTE) {
      uint8_t v = buf[start + 1];
      if ((v & 0xF0) == 0x10 || (v & 0xF0) == 0x20 || (v & 0xF0) == 0x30) return start;
    }
    start = i - OPCODE_OFFSET_V1;
    if (start >= 0 && start + 2 <= (int)len && buf[start] == SYNC_BYTE) {
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
  if (len < HEADER_LEN_BROADCAST) return false;
  if (len > SYNC_LEN + HEADER_LEN + MAX_PAYLOAD + 64) return false;

  int off = findPacketStart(buf, len);
  if (off < 0) return false;

  const uint8_t* p = buf + off;
  size_t pLen = len - off;

  bool hasSync = (p[0] == SYNC_BYTE);
  const uint8_t* h = hasSync ? (p + 1) : p;
  size_t hLen = pLen - (hasSync ? 1 : 0);

  uint8_t v0 = h[0];
  size_t hdrLen;
  bool isV2 = ((v0 & 0xF0) == 0x20 || (v0 & 0xF0) == 0x30);

  if (isV2) {
    // v2: opcode(1) + from(8) + [to(8)?] + ttl(1) + channel(1)
    if (hLen < 12) return false;  // min: ver+opcode+from+ttl+channel
    hdr->version_flags = v0;
    hdr->opcode = h[1];
    memcpy(hdr->from, h + 2, NODE_ID_LEN);
    if (v0 & FLAG_BROADCAST) {
      memcpy(hdr->to, BROADCAST_ID, NODE_ID_LEN);
      hdr->ttl = h[10];
      hdr->channel = h[11];
      hdrLen = 12;
    } else {
      if (hLen < 20) return false;
      memcpy(hdr->to, h + 10, NODE_ID_LEN);
      hdr->ttl = h[18];
      hdr->channel = h[19];
      hdrLen = 20;
    }
  } else if ((v0 & 0xF0) == 0x10) {
    // v1: from(8) + to(8) + ttl(1) + opcode(1) + channel(1)
    if (hLen < HEADER_LEN_V0) return false;
    hdr->version_flags = v0;
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
    hdrLen = (hLen >= HEADER_LEN) ? HEADER_LEN : HEADER_LEN_V0;
  } else if (v0 == 0x00 && hLen > (size_t)(1 + NODE_ID_LEN * 2 + 1) && h[1 + NODE_ID_LEN * 2 + 1] == OP_HELLO) {
    hdr->version_flags = 0x10;
    memcpy(hdr->from, h + 1, NODE_ID_LEN);
    memcpy(hdr->to, h + 1 + NODE_ID_LEN, NODE_ID_LEN);
    hdr->ttl = h[1 + NODE_ID_LEN * 2];
    hdr->opcode = OP_HELLO;
    hdr->channel = CHANNEL_DEFAULT;
    hdrLen = HEADER_LEN_V0;
  } else {
    return false;
  }

  // HELLO: строгая длина
  if (hdr->opcode == OP_HELLO) {
    size_t expectedLen = (hasSync ? SYNC_LEN : 0) + hdrLen;
    if (pLen != expectedLen) return false;
  }

  size_t logicalStart = off + (hasSync ? 1 : 0);
  size_t pl = len > logicalStart + hdrLen ? len - logicalStart - hdrLen : 0;
  if (payload) *payload = buf + logicalStart + hdrLen;
  if (payloadLen) *payloadLen = pl;

  size_t minPl, maxPl;
  if (getExpectedPayloadRange(hdr->opcode, &minPl, &maxPl)) {
    if (pl < minPl || pl > maxPl) return false;
  }
  bool isBroadcast = (hdr->version_flags & FLAG_BROADCAST) != 0;
  size_t expected = getExpectedPacketLength(hdr->opcode, pl, isBroadcast);
  if (expected != 0 && pLen != expected) return false;

  return true;
}

size_t getExpectedPacketLength(uint8_t opcode, size_t payloadLen, bool isBroadcast) {
  size_t hdrLen = isBroadcast ? HEADER_LEN_BROADCAST : (SYNC_LEN + HEADER_LEN);
  switch (opcode) {
    case OP_HELLO:
    case OP_PING:
    case OP_PONG:
      return 0;  // переменная: v2 broadcast=13, v1/v2 unicast=21
    case OP_ACK:
    case OP_READ:
      return (payloadLen == 4) ? (hdrLen + 4) : 0;
    case OP_KEY_EXCHANGE:
      return (payloadLen == 32) ? (hdrLen + 32) : 0;
    case OP_ROUTE_REQ:
      return (payloadLen == 21) ? (HEADER_LEN_BROADCAST + 21) : 0;  // всегда broadcast
    case OP_ROUTE_REPLY:
      return (payloadLen == 21) ? (SYNC_LEN + HEADER_LEN + 21) : 0;  // всегда unicast
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
