/**
 * Packet Layer — формат пакета, opcodes
 * См. docs/CUSTOM_PROTOCOL_PLAN.md §4.2
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace protocol {

// Opcodes
constexpr uint8_t OP_MSG = 0x01;
constexpr uint8_t OP_ACK = 0x02;
constexpr uint8_t OP_HELLO = 0x03;
constexpr uint8_t OP_ROUTE_REQ = 0x04;
constexpr uint8_t OP_ROUTE_REPLY = 0x05;
constexpr uint8_t OP_KEY_EXCHANGE = 0x06;
constexpr uint8_t OP_TELEMETRY = 0x07;
constexpr uint8_t OP_LOCATION = 0x08;
constexpr uint8_t OP_GROUP_MSG = 0x09;
constexpr uint8_t OP_MSG_FRAG = 0x0A;
constexpr uint8_t OP_VOICE_MSG = 0x0B;
constexpr uint8_t OP_READ = 0x0C;   // Подтверждение прочтения (msg_id 4B)
constexpr uint8_t OP_PONG = 0xFE;
constexpr uint8_t OP_PING = 0xFF;

// Размеры
constexpr size_t NODE_ID_LEN = 8;
constexpr size_t HEADER_LEN = 1 + NODE_ID_LEN * 2 + 1 + 1 + 1;  // + From + To + TTL + Opcode + Channel
constexpr uint8_t CHANNEL_DEFAULT = 0;  // Публичный канал
constexpr size_t MAC_LEN = 16;
constexpr size_t MAX_PAYLOAD = 200;

struct PacketHeader {
  uint8_t version_flags;
  uint8_t from[NODE_ID_LEN];
  uint8_t to[NODE_ID_LEN];
  uint8_t ttl;
  uint8_t opcode;
  uint8_t channel;  // Логический канал (0 = публичный)
};

// Broadcast: все 0xFF
extern const uint8_t BROADCAST_ID[NODE_ID_LEN];

size_t buildPacket(uint8_t* buf, size_t maxLen,
                  const uint8_t* from, const uint8_t* to,
                  uint8_t ttl, uint8_t opcode,
                  const uint8_t* payload, size_t payloadLen,
                  bool encrypted = false, bool ackReq = false, bool compressed = false,
                  uint8_t channel = CHANNEL_DEFAULT);

bool parsePacket(const uint8_t* buf, size_t len, PacketHeader* hdr,
                const uint8_t** payload, size_t* payloadLen);

bool isEncrypted(const PacketHeader& hdr);
bool isAckReq(const PacketHeader& hdr);
bool isCompressed(const PacketHeader& hdr);

}  // namespace protocol
