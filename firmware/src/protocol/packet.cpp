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
                  bool encrypted, bool ackReq, bool compressed, uint8_t channel, uint16_t pktId) {
  bool broadcast = isBroadcastTo(to);
  bool usePktId = (pktId != 0);
  size_t hdrLen = usePktId
      ? (broadcast ? HEADER_LEN_BROADCAST_PKTID : HEADER_LEN_PKTID)
      : (broadcast ? HEADER_LEN_BROADCAST : (SYNC_LEN + HEADER_LEN));
  if (maxLen < hdrLen + payloadLen) return 0;

  uint8_t* p = buf;
  *p++ = SYNC_BYTE;
  *p++ = (usePktId ? VERSION_V2_PKTID : VERSION_V2) | (broadcast ? FLAG_BROADCAST : 0) |
         (encrypted ? FLAG_ENCRYPTED : 0) | (ackReq ? FLAG_ACK_REQ : 0) | (compressed ? FLAG_COMPRESSED : 0);
  *p++ = opcode;
  if (usePktId) {
    *p++ = (uint8_t)(pktId & 0xFF);
    *p++ = (uint8_t)(pktId >> 8);
  }
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

constexpr int OPCODE_OFFSET_V2 = 2;  // sync(1) + version(1)

static bool isValidOpcode(uint8_t op) {
  return (op >= 0x01 && op <= 0x12) || op == OP_PONG || op == OP_PING;
}

// Только v2 (0x20) и v2.1 (0x30) — все устройства на одной версии
static int findPacketStart(const uint8_t* buf, size_t len) {
  if (len < HEADER_LEN_BROADCAST) return -1;
  for (int off = 0; off <= 8 && off + HEADER_LEN_BROADCAST <= (int)len; off++) {
    if (buf[off] == SYNC_BYTE) {
      uint8_t v = buf[off + 1];
      if ((v & 0xF0) == 0x20 || (v & 0xF0) == 0x30) return off;
    }
  }
  for (int i = 0; i < (int)len && i <= 32; i++) {
    if (!isValidOpcode(buf[i])) continue;
    int start = i - OPCODE_OFFSET_V2;
    if (start >= 0 && start + 2 <= (int)len && buf[start] == SYNC_BYTE) {
      uint8_t v = buf[start + 1];
      if ((v & 0xF0) == 0x20 || (v & 0xF0) == 0x30) return start;
    }
  }
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
  bool hasPktId = false;

  if (isV2) {
    hasPktId = ((v0 & 0xF0) == 0x30);
    if (hasPktId) {
      // v2.1: opcode(1) + pktId(2) + from(8) + [to(8)?] + ttl(1) + channel(1)
      if (hLen < 14) return false;
      hdr->version_flags = v0;
      hdr->opcode = h[1];
      hdr->pktId = (uint16_t)h[2] | ((uint16_t)h[3] << 8);
      memcpy(hdr->from, h + 4, NODE_ID_LEN);
      if (v0 & FLAG_BROADCAST) {
        memcpy(hdr->to, BROADCAST_ID, NODE_ID_LEN);
        hdr->ttl = h[12];
        hdr->channel = h[13];
        hdrLen = 14;
      } else {
        if (hLen < 22) return false;
        memcpy(hdr->to, h + 12, NODE_ID_LEN);
        hdr->ttl = h[20];
        hdr->channel = h[21];
        hdrLen = 22;
      }
    } else {
      // v2: opcode(1) + from(8) + [to(8)?] + ttl(1) + channel(1)
      if (hLen < 12) return false;
      hdr->version_flags = v0;
      hdr->opcode = h[1];
      hdr->pktId = 0;
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
    }
  } else {
    return false;  // только v2/v2.1
  }

  // Ранний выход: opcodes с фиксированной длиной — сразу отсечь коррупцию (коллизия)
  size_t expectedFullLen = 0;
  bool isBc = (v0 & FLAG_BROADCAST) != 0;
  switch (hdr->opcode) {
    case OP_ACK:
    case OP_READ:
      expectedFullLen = isBc ? (SYNC_LEN + 12 + 4) : (SYNC_LEN + 20 + 4);  // 17 или 25
      break;
    case OP_NACK:
      expectedFullLen = SYNC_LEN + HEADER_LEN + 2;  // 23
      break;
    case OP_KEY_EXCHANGE:
      // v2.1 (pktId): заголовок HEADER_LEN_*PKTID уже включает SYNC в buildPacket — не смешивать с 53 B (v2)
      if (hasPktId) {
        expectedFullLen = isBc ? (HEADER_LEN_BROADCAST_PKTID + 32) : (HEADER_LEN_PKTID + 32);
      } else {
        expectedFullLen = isBc ? (HEADER_LEN_BROADCAST + 32) : (SYNC_LEN + HEADER_LEN + 32);
      }
      break;
    default:
      break;
  }
  if (expectedFullLen != 0 && pLen != expectedFullLen) return false;

  // HELLO: строгая длина
  if (hdr->opcode == OP_HELLO) {
    size_t expectedLen = (hasSync ? SYNC_LEN : 0) + hdrLen;
    if (pLen != expectedLen) return false;
  }

  size_t logicalStart = off + (hasSync ? 1 : 0);
  size_t pl = len > logicalStart + hdrLen ? len - logicalStart - hdrLen : 0;
  if (payload) *payload = buf + logicalStart + hdrLen;
  if (payloadLen) *payloadLen = pl;

  // Fallback: POLL (0x0F) с 32B payload — скорее KEY_EXCHANGE (0x06) с коррупцией opcode при коллизии
  if (hdr->opcode == OP_POLL && pl == 32 && (v0 & FLAG_BROADCAST)) {
    hdr->opcode = OP_KEY_EXCHANGE;
  }

  size_t minPl, maxPl;
  if (getExpectedPayloadRange(hdr->opcode, &minPl, &maxPl)) {
    if (pl < minPl || pl > maxPl) return false;
  }
  bool isBroadcast = (hdr->version_flags & FLAG_BROADCAST) != 0;
  size_t expected = getExpectedPacketLength(hdr->opcode, pl, isBroadcast, hasPktId);
  if (expected != 0 && pLen != expected) return false;

  return true;
}

size_t getExpectedPacketLength(uint8_t opcode, size_t payloadLen, bool isBroadcast, bool hasPktId) {
  size_t hdrLen = isBroadcast ? HEADER_LEN_BROADCAST : (SYNC_LEN + HEADER_LEN);
  switch (opcode) {
    case OP_NACK:
      return (payloadLen == 2) ? (SYNC_LEN + HEADER_LEN + 2) : 0;  // unicast
    case OP_HELLO:
    case OP_PING:
    case OP_PONG:
      return 0;  // переменная: v2 broadcast=13, unicast=21
    case OP_ACK:
    case OP_READ:
      return (payloadLen == 4) ? (hdrLen + 4) : 0;
    case OP_ACK_BATCH:
      return (payloadLen >= 5 && payloadLen <= 33 && (payloadLen - 1) % 4 == 0) ? (hdrLen + payloadLen) : 0;
    case OP_KEY_EXCHANGE:
      if (hasPktId) {
        size_t h = isBroadcast ? HEADER_LEN_BROADCAST_PKTID : HEADER_LEN_PKTID;
        return (payloadLen == 32) ? (h + 32) : 0;
      }
      return (payloadLen == 32) ? (hdrLen + 32) : 0;
    case OP_ROUTE_REQ:
      return (payloadLen == 21) ? (HEADER_LEN_BROADCAST + 21) : 0;  // всегда broadcast
    case OP_ROUTE_REPLY:
      return (payloadLen == 21) ? (SYNC_LEN + HEADER_LEN + 21) : 0;  // всегда unicast
    case OP_ECHO:
      return (payloadLen == 12) ? (HEADER_LEN_BROADCAST + 12) : 0;  // broadcast: msgId(4)+originalFrom(8)
    case OP_POLL:
      return (payloadLen == 0) ? HEADER_LEN_BROADCAST : 0;  // RIT: broadcast без payload
    case OP_SF_BEACON:
      return (payloadLen == 1) ? (HEADER_LEN_BROADCAST + 1) : 0;  // broadcast: mesh SF
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
    case OP_ACK_BATCH:
      *minOut = 5;   // count(1) + msgId(4)
      *maxOut = 33;  // count(1) + msgId(4)*8
      return true;
    case OP_KEY_EXCHANGE:
      *minOut = *maxOut = 32;
      return true;
    case OP_ROUTE_REQ:
    case OP_ROUTE_REPLY:
      *minOut = *maxOut = 21;
      return true;
    case OP_ECHO:
      *minOut = *maxOut = 12;  // msgId(4) + originalFrom(8)
      return true;
    case OP_POLL:
      *minOut = *maxOut = 0;
      return true;
    case OP_SF_BEACON:
      *minOut = *maxOut = 1;  // mesh SF (7, 9, 10, 12)
      return true;
    case OP_TELEMETRY:
      *minOut = 28;  // 4 plain + crypto::OVERHEAD
      *maxOut = 64;  // 48 было мало — коллизии/мерж дают до 56B
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
    case OP_NACK:
      *minOut = *maxOut = 2;  // pktId
      return true;
    case OP_XOR_RELAY:
      *minOut = 36;  // pktIdA(2)+pktIdB(2)+fromA(8)+toA(8)+fromB(8)+toB(8)
      *maxOut = 36 + MAX_PAYLOAD;
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
