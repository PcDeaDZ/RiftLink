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

static bool isValidOpcode(uint8_t op) {
  return (op >= 0x01 && op <= 0x14) || op == OP_PONG || op == OP_PING;
}

static bool isAll(const uint8_t* id, uint8_t v) {
  for (size_t i = 0; i < NODE_ID_LEN; i++) {
    if (id[i] != v) return false;
  }
  return true;
}

static ParseStatus decodeHeaderCandidate(const uint8_t* p, size_t pLen, PacketHeader* hdr, size_t* hdrLenOut) {
  if (!p || !hdr || !hdrLenOut || pLen < 3 || p[0] != SYNC_BYTE) return ParseStatus::bad_header;

  uint8_t v0 = p[1];
  uint8_t version = v0 & VERSION_MASK;
  if (version != VERSION_V2 && version != VERSION_V2_PKTID) return ParseStatus::bad_version;

  uint8_t opcode = p[2];
  if (!isValidOpcode(opcode)) return ParseStatus::invalid_opcode;

  bool hasPktId = (version == VERSION_V2_PKTID);
  bool isBroadcast = (v0 & FLAG_BROADCAST) != 0;
  size_t hdrLen = hasPktId
      ? (isBroadcast ? HEADER_LEN_BROADCAST_PKTID : HEADER_LEN_PKTID)
      : (isBroadcast ? HEADER_LEN_BROADCAST : (SYNC_LEN + HEADER_LEN));

  if (pLen < hdrLen) return ParseStatus::bad_header;

  hdr->version_flags = v0;
  hdr->opcode = opcode;
  if (hasPktId) {
    hdr->pktId = (uint16_t)p[3] | ((uint16_t)p[4] << 8);
    memcpy(hdr->from, p + 5, NODE_ID_LEN);
    if (isBroadcast) {
      memcpy(hdr->to, BROADCAST_ID, NODE_ID_LEN);
      hdr->ttl = p[13];
      hdr->channel = p[14];
    } else {
      memcpy(hdr->to, p + 13, NODE_ID_LEN);
      hdr->ttl = p[21];
      hdr->channel = p[22];
    }
  } else {
    hdr->pktId = 0;
    memcpy(hdr->from, p + 3, NODE_ID_LEN);
    if (isBroadcast) {
      memcpy(hdr->to, BROADCAST_ID, NODE_ID_LEN);
      hdr->ttl = p[11];
      hdr->channel = p[12];
    } else {
      memcpy(hdr->to, p + 11, NODE_ID_LEN);
      hdr->ttl = p[19];
      hdr->channel = p[20];
    }
  }

  if (isAll(hdr->from, 0x00) || isAll(hdr->from, 0xFF)) return ParseStatus::invalid_ids;
  if (!isBroadcast && (isAll(hdr->to, 0x00) || isAll(hdr->to, 0xFF))) return ParseStatus::invalid_ids;

  *hdrLenOut = hdrLen;
  return ParseStatus::ok;
}

const char* parseStatusToString(ParseStatus status) {
  switch (status) {
    case ParseStatus::ok: return "ok";
    case ParseStatus::no_sync: return "no_sync";
    case ParseStatus::bad_version: return "bad_version";
    case ParseStatus::bad_header: return "bad_header";
    case ParseStatus::invalid_opcode: return "invalid_opcode";
    case ParseStatus::len_mismatch: return "len_mismatch";
    case ParseStatus::payload_range: return "payload_range";
    case ParseStatus::invalid_ids: return "invalid_ids";
    default: return "unknown";
  }
}

bool parsePacketEx(const uint8_t* buf, size_t len, PacketHeader* hdr,
                   const uint8_t** payload, size_t* payloadLen, ParseResult* result) {
  if (result) *result = ParseResult{};
  if (!buf || !hdr || len < HEADER_LEN_BROADCAST) return false;
  if (len > SYNC_LEN + HEADER_LEN + MAX_PAYLOAD + 64) return false;

  constexpr size_t MAX_START_SCAN = 32;
  bool sawSync = false;
  ParseResult best;
  best.status = ParseStatus::no_sync;

  size_t maxOff = (len > 0) ? (len - 1) : 0;
  if (maxOff > MAX_START_SCAN) maxOff = MAX_START_SCAN;
  for (size_t off = 0; off <= maxOff; off++) {
    if (buf[off] != SYNC_BYTE) continue;
    sawSync = true;

    PacketHeader candidateHdr{};
    size_t hdrLen = 0;
    size_t packetLen = len - off;
    ParseStatus st = decodeHeaderCandidate(buf + off, packetLen, &candidateHdr, &hdrLen);

    ParseResult candidateResult{};
    candidateResult.status = st;
    candidateResult.startOffset = off;
    candidateResult.packetLen = packetLen;
    candidateResult.opcode = candidateHdr.opcode;
    candidateResult.pktId = candidateHdr.pktId;
    candidateResult.hasPktId = (candidateHdr.version_flags & VERSION_MASK) == VERSION_V2_PKTID;
    candidateResult.isBroadcast = (candidateHdr.version_flags & FLAG_BROADCAST) != 0;

    if (st == ParseStatus::ok) {
      size_t pl = packetLen - hdrLen;
      if (pl > MAX_PAYLOAD) {
        st = ParseStatus::payload_range;
      } else {
        bool directionOk = true;
        switch (candidateHdr.opcode) {
          case OP_ROUTE_REQ:
          case OP_ECHO:
          case OP_POLL:
          case OP_SF_BEACON:
            directionOk = candidateResult.isBroadcast;
            break;
          case OP_ROUTE_REPLY:
          case OP_NACK:
            directionOk = !candidateResult.isBroadcast;
            break;
          default:
            break;
        }
        if (!directionOk) {
          st = ParseStatus::len_mismatch;
          candidateResult.expectedLen = hdrLen + pl;
        }
      }

      if (st == ParseStatus::ok) {
        size_t minPl = 0;
        size_t maxPl = 0;
        if (getExpectedPayloadRange(candidateHdr.opcode, &minPl, &maxPl) &&
            (pl < minPl || pl > maxPl)) {
          st = ParseStatus::payload_range;
        } else {
          size_t expected = getExpectedPacketLength(candidateHdr.opcode, pl,
              candidateResult.isBroadcast, candidateResult.hasPktId);
          candidateResult.expectedLen = expected;
          if (expected != 0 && packetLen != expected) {
            st = ParseStatus::len_mismatch;
          }
        }
      }
      candidateResult.status = st;
    }

    if (st == ParseStatus::ok) {
      *hdr = candidateHdr;
      if (payload) *payload = buf + off + hdrLen;
      if (payloadLen) *payloadLen = packetLen - hdrLen;
      if (result) *result = candidateResult;
      return true;
    }

    if (best.status == ParseStatus::no_sync) {
      best = candidateResult;
    }
  }

  if (!sawSync) {
    best.status = ParseStatus::no_sync;
  }
  if (result) *result = best;
  return false;
}

bool parsePacket(const uint8_t* buf, size_t len, PacketHeader* hdr,
                 const uint8_t** payload, size_t* payloadLen) {
  return parsePacketEx(buf, len, hdr, payload, payloadLen, nullptr);
}

size_t getExpectedPacketLength(uint8_t opcode, size_t payloadLen, bool isBroadcast, bool hasPktId) {
  size_t hdrLen = hasPktId
      ? (isBroadcast ? HEADER_LEN_BROADCAST_PKTID : HEADER_LEN_PKTID)
      : (isBroadcast ? HEADER_LEN_BROADCAST : (SYNC_LEN + HEADER_LEN));
  switch (opcode) {
    case OP_NACK:
      return (payloadLen == 2) ? (hdrLen + 2) : 0;
    case OP_HELLO:
    case OP_PING:
    case OP_PONG:
      return (payloadLen == 0) ? hdrLen : 0;
    case OP_ACK:
    case OP_ACK_BATCH:
      return 0;  // ACK v2 payload может быть зашифрован и переменной длины
    case OP_READ:
      return (payloadLen == 4) ? (hdrLen + 4) : 0;
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
      // v1 ACK(4B) допускаем только для явного reject в main.cpp;
      // v2 ACK — зашифрованный payload (nonce+tag+plain).
      *minOut = 4;
      *maxOut = 96;
      return true;
    case OP_READ:
      *minOut = *maxOut = 4;
      return true;
    case OP_ACK_BATCH:
      // v1 ACK_BATCH оставляем в parser для диагностики/explicit reject.
      *minOut = 5;
      *maxOut = 96;
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
    case OP_SOS:
      *minOut = 28;  // msgId(4) + crypto::OVERHEAD
      *maxOut = MAX_PAYLOAD;
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
