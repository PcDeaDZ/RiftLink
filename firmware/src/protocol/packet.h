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
constexpr uint8_t OP_NACK = 0x0D;   // Запрос повтора (payload: pktId 2B)
constexpr uint8_t OP_ECHO = 0x0E;   // Эхо вместо ACK: broadcast msgId+originalFrom (12B)
constexpr uint8_t OP_POLL = 0x0F;   // RIT: broadcast «присылайте пакеты для меня» (payload пустой)
constexpr uint8_t OP_MSG_BATCH = 0x10;  // Packet Fusion: count(1) + [len(2)+enc]* — несколько MSG в одном пакете
constexpr uint8_t OP_XOR_RELAY = 0x11;  // Network Coding: XOR(A,B) broadcast, meta: pktIdA/B, fromA/B, toA/B
constexpr uint8_t OP_SF_BEACON = 0x12;  // broadcast: payload 1B = mesh SF (7,9,10,12) — новый узел узнаёт, на каком SF искать
constexpr uint8_t OP_ACK_BATCH = 0x13;  // unicast: count(1) + msgId(4)* — батч ACK для MSG_BATCH (pipelining)
constexpr uint8_t OP_PONG = 0xFE;
constexpr uint8_t OP_PING = 0xFF;

// Размеры
constexpr size_t NODE_ID_LEN = 8;
constexpr uint8_t SYNC_BYTE = 0x5A;  // маркер начала пакета — искать при сдвиге (SX1262 corruption)
constexpr size_t HEADER_LEN = 1 + NODE_ID_LEN * 2 + 1 + 1 + 1;  // version + From + To + TTL + Opcode + Channel (unicast)
constexpr size_t HEADER_LEN_BROADCAST = 1 + 1 + 1 + NODE_ID_LEN + 1 + 1;  // v2: sync+ver+opcode+from+ttl+channel (13 B)
// v2.1: version 0x30 = заголовок с pktId для NACK
constexpr uint8_t VERSION_V2_PKTID = 0x30;
constexpr size_t PKTID_LEN = 2;
constexpr size_t HEADER_LEN_BROADCAST_PKTID = 1 + 1 + 1 + PKTID_LEN + NODE_ID_LEN + 1 + 1;  // 15 B
constexpr size_t HEADER_LEN_PKTID = 1 + 1 + 1 + PKTID_LEN + NODE_ID_LEN * 2 + 1 + 1;  // 23 B
constexpr size_t SYNC_LEN = 1;
constexpr size_t PAYLOAD_OFFSET = SYNC_LEN + HEADER_LEN;  // смещение payload в пакете с sync
constexpr uint8_t CHANNEL_DEFAULT = 0;  // Публичный канал
constexpr size_t MAC_LEN = 16;
constexpr size_t MAX_PAYLOAD = 200;

struct PacketHeader {
  uint8_t version_flags;
  uint16_t pktId;   // 0 = без pktId (v2), ненулевой = v2.1 с NACK
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
                  uint8_t channel = CHANNEL_DEFAULT, uint16_t pktId = 0);

bool parsePacket(const uint8_t* buf, size_t len, PacketHeader* hdr,
                const uint8_t** payload, size_t* payloadLen);

/** Ожидаемая длина пакета (pLen) для opcode. 0 = неизвестно/переменная. */
size_t getExpectedPacketLength(uint8_t opcode, size_t payloadLen, bool isBroadcast, bool hasPktId = false);

/** Полный размер KEY_EXCHANGE в эфире (как в buildPacket): v2.1 unicast = HEADER_LEN_PKTID+32 (55 B). */
constexpr inline size_t keyExchangeTotalLen(bool hasPktId, bool isBroadcast) {
  if (hasPktId) {
    return isBroadcast ? (HEADER_LEN_BROADCAST_PKTID + 32) : (HEADER_LEN_PKTID + 32);
  }
  return isBroadcast ? (HEADER_LEN_BROADCAST + 32) : (SYNC_LEN + HEADER_LEN + 32);
}

/** Min/max payload для opcode. true если opcode известен. */
bool getExpectedPayloadRange(uint8_t opcode, size_t* minOut, size_t* maxOut);

bool isEncrypted(const PacketHeader& hdr);
bool isAckReq(const PacketHeader& hdr);
bool isCompressed(const PacketHeader& hdr);

}  // namespace protocol
