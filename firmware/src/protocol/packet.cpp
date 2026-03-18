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
  if (maxLen < HEADER_LEN + payloadLen) return 0;

  uint8_t* p = buf;
  *p++ = 0x10 | (encrypted ? FLAG_ENCRYPTED : 0) | (ackReq ? FLAG_ACK_REQ : 0) | (compressed ? FLAG_COMPRESSED : 0);
  memcpy(p, from, NODE_ID_LEN); p += NODE_ID_LEN;  // from first (CUSTOM_PROTOCOL_PLAN §4.2)
  memcpy(p, to, NODE_ID_LEN);   p += NODE_ID_LEN;
  *p++ = ttl;
  *p++ = opcode;
  *p++ = channel;
  if (payloadLen > 0) {
    memcpy(p, payload, payloadLen);
  }
  return HEADER_LEN + payloadLen;
}

constexpr size_t HEADER_LEN_V0 = 19;  // Без channel (обратная совместимость)

bool parsePacket(const uint8_t* buf, size_t len, PacketHeader* hdr, const uint8_t** payload, size_t* payloadLen) {
  if (len < HEADER_LEN_V0) return false;
  // Версия: старшие 4 бита = 0x1, 0x2 или 0x3. Отсекаем чужие (0xFF, Meshtastic).
  // buf[0]=0x39 — Paper/старая прошивка; buf[0]=0x00 — Paper: порча при SPI-конфликте с e-ink.
  uint8_t v = buf[0] & 0xF0;
  if (v != 0x10 && v != 0x20 && v != 0x30) {
    if (buf[0] == 0x00 && len >= HEADER_LEN_V0 && buf[1 + NODE_ID_LEN * 2 + 1] == OP_HELLO) {
      hdr->version_flags = 0x10;  // предполагаем version 1
    } else {
      return false;
    }
  } else {
    hdr->version_flags = buf[0];
  }
  memcpy(hdr->from, buf + 1, NODE_ID_LEN);          // bytes 1-8
  memcpy(hdr->to, buf + 1 + NODE_ID_LEN, NODE_ID_LEN);    // bytes 9-16
  // Совместимость: если from=broadcast и to≠broadcast — пакет в формате "to first"
  if (hdr->from[0] == 0xFF && hdr->from[1] == 0xFF && (hdr->to[0] != 0xFF || hdr->to[1] != 0xFF)) {
    uint8_t tmp[NODE_ID_LEN];
    memcpy(tmp, hdr->from, NODE_ID_LEN);
    memcpy(hdr->from, hdr->to, NODE_ID_LEN);
    memcpy(hdr->to, tmp, NODE_ID_LEN);
  }
  hdr->ttl = buf[1 + NODE_ID_LEN * 2];
  hdr->opcode = buf[1 + NODE_ID_LEN * 2 + 1];
  hdr->channel = (len >= HEADER_LEN) ? buf[1 + NODE_ID_LEN * 2 + 2] : CHANNEL_DEFAULT;

  size_t hdrLen = (len >= HEADER_LEN) ? HEADER_LEN : HEADER_LEN_V0;
  if (payload) *payload = buf + hdrLen;
  if (payloadLen) *payloadLen = len > hdrLen ? len - hdrLen : 0;
  return true;
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
