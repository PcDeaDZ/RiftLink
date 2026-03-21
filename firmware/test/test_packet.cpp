/**
 * Синтетические тесты для protocol::buildPacket и parsePacket
 * Включает: битые пакеты, relay-сценарии, граничные случаи
 * Запуск: pio test -e native
 */

#include <cstring>
#include <unity.h>
#include "protocol/packet.h"

static uint8_t s_from[8] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x08};
static uint8_t s_to[8] = {0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x08, 0x19};
static uint8_t s_relay[8] = {0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x08, 0x19, 0x2A};

void setUp(void) {}
void tearDown(void) {}

// --- Базовые roundtrip ---
void test_build_parse_broadcast_roundtrip() {
  uint8_t buf[256];
  // OP_HELLO: payload must be 0 bytes
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 5, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  TEST_ASSERT_GREATER_THAN(0, len);

  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_HELLO, hdr.opcode);
  TEST_ASSERT_EQUAL(5, hdr.ttl);
  TEST_ASSERT_EQUAL(0, hdr.channel);
  TEST_ASSERT_EQUAL(0, plLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(s_from, hdr.from, 8);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(protocol::BROADCAST_ID, hdr.to, 8);
}

void test_build_parse_unicast_roundtrip() {
  uint8_t buf[256];
  // OP_MSG: min 29 bytes (1 + crypto OVERHEAD), max MAX_PAYLOAD
  uint8_t payload[32];
  for (int i = 0; i < 32; i++) payload[i] = (uint8_t)i;
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 3, protocol::OP_MSG,
      payload, 32, true, true, false, 1, 0);
  TEST_ASSERT_GREATER_THAN(0, len);

  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_MSG, hdr.opcode);
  TEST_ASSERT_EQUAL(3, hdr.ttl);
  TEST_ASSERT_EQUAL(1, hdr.channel);
  TEST_ASSERT_TRUE(protocol::isEncrypted(hdr));
  TEST_ASSERT_TRUE(protocol::isAckReq(hdr));
  TEST_ASSERT_EQUAL(32, plLen);
}

void test_parse_invalid_too_short() {
  uint8_t buf[] = {0x5A, 0x20};
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, 2, &hdr, nullptr, nullptr);
  TEST_ASSERT_FALSE(ok);
}

void test_broadcast_id_constant() {
  for (int i = 0; i < 8; i++) {
    TEST_ASSERT_EQUAL(0xFF, protocol::BROADCAST_ID[i]);
  }
}

void test_sync_byte_in_output() {
  uint8_t buf[64];
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 0, protocol::OP_PING,
      nullptr, 0, false, false, false, 0, 0);
  TEST_ASSERT_GREATER_THAN(0, len);
  TEST_ASSERT_EQUAL(protocol::SYNC_BYTE, buf[0]);
}

// --- Битые пакеты (corruption) ---
void test_corrupt_garbage_before_packet() {
  uint8_t valid[64];
  size_t vlen = protocol::buildPacket(valid, sizeof(valid),
      s_from, protocol::BROADCAST_ID, 3, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  uint8_t buf[80];
  memset(buf, 0xAA, 5);  // 5 байт мусора
  memcpy(buf + 5, valid, vlen);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, 5 + vlen, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_HELLO, hdr.opcode);
}

void test_corrupt_shifted_sync_sx1262() {
  uint8_t valid[64];
  size_t vlen = protocol::buildPacket(valid, sizeof(valid),
      s_from, protocol::BROADCAST_ID, 2, protocol::OP_PONG,
      nullptr, 0, false, false, false, 0, 0);
  uint8_t buf[80];
  for (int shift = 1; shift <= 8; shift++) {
    memset(buf, 0xAA, shift);
    memcpy(buf + shift, valid, vlen);
    protocol::PacketHeader hdr;
    bool ok = protocol::parsePacket(buf, shift + (int)vlen, &hdr, nullptr, nullptr);
    TEST_ASSERT_TRUE_MESSAGE(ok, "findPacketStart should find sync after shift");
  }
}

void test_shifted_parse_all_core_opcodes_with_offset() {
  struct Case {
    uint8_t opcode;
    const uint8_t* to;
    uint8_t payload[40];
    size_t payloadLen;
    bool encrypted;
    uint16_t pktId;
  } cases[] = {
    {protocol::OP_HELLO, protocol::BROADCAST_ID, {0}, 0, false, 0},
    {protocol::OP_KEY_EXCHANGE, s_to, {0}, 32, false, 7},
    {protocol::OP_NACK, s_to, {0x34, 0x12}, 2, false, 0},
    {protocol::OP_ACK, s_to, {0x11, 0x22, 0x33, 0x44}, 4, false, 0},
    {protocol::OP_MSG, s_to, {0}, 32, true, 0},
    {protocol::OP_TELEMETRY, protocol::BROADCAST_ID, {0}, 32, true, 0},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    if (cases[i].payloadLen == 32 && (cases[i].opcode == protocol::OP_KEY_EXCHANGE ||
                                      cases[i].opcode == protocol::OP_MSG ||
                                      cases[i].opcode == protocol::OP_TELEMETRY)) {
      for (size_t k = 0; k < 32; k++) cases[i].payload[k] = (uint8_t)(k + i);
    }
    uint8_t pkt[256];
    size_t pktLen = protocol::buildPacket(pkt, sizeof(pkt),
        s_from, cases[i].to, 2, cases[i].opcode,
        cases[i].payloadLen ? cases[i].payload : nullptr, cases[i].payloadLen,
        cases[i].encrypted, false, false, 0, cases[i].pktId);
    TEST_ASSERT_GREATER_THAN(0, pktLen);

    for (size_t shift = 1; shift <= 4; shift++) {
      uint8_t buf[300];
      memset(buf, 0xAA, shift);
      memcpy(buf + shift, pkt, pktLen);
      protocol::PacketHeader hdr;
      const uint8_t* pl = nullptr;
      size_t plLen = 0;
      protocol::ParseResult res;
      bool ok = protocol::parsePacketEx(buf, shift + pktLen, &hdr, &pl, &plLen, &res);
      TEST_ASSERT_TRUE(ok);
      TEST_ASSERT_EQUAL_UINT32(shift, (uint32_t)res.startOffset);
      TEST_ASSERT_EQUAL(cases[i].opcode, hdr.opcode);
      TEST_ASSERT_EQUAL(cases[i].payloadLen, plLen);
    }
  }
}

void test_shifted_parse_status_payload_range_for_bad_ack_len() {
  uint8_t pkt[64];
  const uint8_t ackPl[4] = {0x01, 0x02, 0x03, 0x04};
  size_t pktLen = protocol::buildPacket(pkt, sizeof(pkt),
      s_from, s_to, 1, protocol::OP_ACK, ackPl, 4, false, false, false, 0, 0);
  TEST_ASSERT_GREATER_THAN(0, pktLen);
  uint8_t buf[80];
  memset(buf, 0xCC, 2);
  memcpy(buf + 2, pkt, pktLen - 1);  // намеренно обрезаем 1 байт

  protocol::PacketHeader hdr;
  protocol::ParseResult res;
  bool ok = protocol::parsePacketEx(buf, 2 + pktLen - 1, &hdr, nullptr, nullptr, &res);
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_EQUAL(protocol::ParseStatus::payload_range, res.status);
  TEST_ASSERT_EQUAL(protocol::OP_ACK, res.opcode);
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)res.startOffset);
}

void test_parse_status_invalid_ids() {
  uint8_t zero[8] = {0};
  uint8_t pkt[64];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      zero, protocol::BROADCAST_ID, 0, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  TEST_ASSERT_GREATER_THAN(0, len);
  protocol::PacketHeader hdr;
  protocol::ParseResult res;
  bool ok = protocol::parsePacketEx(pkt, len, &hdr, nullptr, nullptr, &res);
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_EQUAL(protocol::ParseStatus::invalid_ids, res.status);
}

void test_corrupt_wrong_sync_byte() {
  uint8_t buf[64];
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 0, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  buf[0] = 0x5B;  // не SYNC_BYTE
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_FALSE(ok);
}

void test_corrupt_wrong_version() {
  uint8_t buf[64];
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 0, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  buf[1] = 0x10;  // не v2/v2.1 (0x20/0x30)
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_FALSE(ok);
}

void test_corrupt_truncated_packet() {
  uint8_t buf[64];
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 0, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len - 2, &hdr, nullptr, nullptr);
  TEST_ASSERT_FALSE(ok);
}

void test_corrupt_hello_with_payload_rejected() {
  uint8_t buf[64];
  const uint8_t bad[] = {0x01};
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 0, protocol::OP_HELLO,
      bad, 1, false, false, false, 0, 0);
  TEST_ASSERT_GREATER_THAN(0, len);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_FALSE(ok);
}

void test_corrupt_msg_payload_too_short() {
  uint8_t buf[64];
  const uint8_t shortPl[] = {0x01, 0x02};
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 1, protocol::OP_MSG,
      shortPl, 2, true, false, false, 0, 0);
  TEST_ASSERT_GREATER_THAN(0, len);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_FALSE(ok);
}

// --- Relay-сценарии ---
void test_relay_broadcast_hello() {
  uint8_t buf[64];
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 10, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(10, hdr.ttl);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(protocol::BROADCAST_ID, hdr.to, 8);
}

void test_relay_unicast_from_to() {
  uint8_t buf[256];
  uint8_t payload[32];
  for (int i = 0; i < 32; i++) payload[i] = (uint8_t)i;
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 5, protocol::OP_MSG,
      payload, 32, true, true, false, 0, 0);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(s_from, hdr.from, 8);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(s_to, hdr.to, 8);
}

void test_relay_echo_broadcast() {
  uint8_t buf[64];
  const uint8_t echoPl[12] = {0x11, 0x22, 0x33, 0x44, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x08};
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_relay, protocol::BROADCAST_ID, 4, protocol::OP_ECHO,
      echoPl, 12, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_ECHO, hdr.opcode);
  TEST_ASSERT_EQUAL(12, plLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(s_relay, hdr.from, 8);
}

void test_relay_poll_rit() {
  uint8_t buf[64];
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 1, protocol::OP_POLL,
      nullptr, 0, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_POLL, hdr.opcode);
}

// --- Граничные случаи ---
void test_edge_ack_broadcast() {
  uint8_t buf[64];
  const uint8_t ackPl[4] = {0x01, 0x02, 0x03, 0x04};
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 0, protocol::OP_ACK,
      ackPl, 4, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_ACK, hdr.opcode);
  TEST_ASSERT_EQUAL(4, plLen);
}

void test_edge_nack_payload() {
  uint8_t buf[64];
  const uint8_t nackPl[2] = {0x12, 0x34};  // pktId в payload
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 2, protocol::OP_NACK,
      nackPl, 2, false, false, false, 0, 0);
  TEST_ASSERT_GREATER_THAN(0, len);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_NACK, hdr.opcode);
  TEST_ASSERT_EQUAL(2, plLen);
}

void test_edge_max_payload() {
  uint8_t buf[512];
  uint8_t payload[protocol::MAX_PAYLOAD];
  for (size_t i = 0; i < protocol::MAX_PAYLOAD; i++) payload[i] = (uint8_t)(i & 0xFF);
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 1, protocol::OP_MSG,
      payload, protocol::MAX_PAYLOAD, true, false, false, 0, 0);
  TEST_ASSERT_GREATER_THAN(0, len);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::MAX_PAYLOAD, plLen);
}

void test_edge_get_expected_payload_range() {
  size_t minP, maxP;
  TEST_ASSERT_TRUE(protocol::getExpectedPayloadRange(protocol::OP_HELLO, &minP, &maxP));
  TEST_ASSERT_EQUAL(0, minP);
  TEST_ASSERT_EQUAL(0, maxP);
  TEST_ASSERT_TRUE(protocol::getExpectedPayloadRange(protocol::OP_MSG, &minP, &maxP));
  TEST_ASSERT_EQUAL(29, minP);
  TEST_ASSERT_EQUAL(protocol::MAX_PAYLOAD, maxP);
  TEST_ASSERT_FALSE(protocol::getExpectedPayloadRange(0x99, &minP, &maxP));
}

void test_edge_multiple_packets_parse_first_only() {
  uint8_t pkt1[64], pkt2[64];
  size_t len1 = protocol::buildPacket(pkt1, sizeof(pkt1),
      s_from, protocol::BROADCAST_ID, 1, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  size_t len2 = protocol::buildPacket(pkt2, sizeof(pkt2),
      s_to, protocol::BROADCAST_ID, 2, protocol::OP_PONG,
      nullptr, 0, false, false, false, 0, 0);
  uint8_t buf[128];
  memcpy(buf, pkt1, len1);
  memcpy(buf + len1, pkt2, len2);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len1, &hdr, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_HELLO, hdr.opcode);
}

// --- Дополнительные opcodes ---
void test_opcode_key_exchange_roundtrip() {
  uint8_t buf[128];
  uint8_t key[32];
  for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 7);
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 1, protocol::OP_KEY_EXCHANGE,
      key, 32, false, false, false, 0, 0);
  TEST_ASSERT_GREATER_THAN(0, len);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_KEY_EXCHANGE, hdr.opcode);
  TEST_ASSERT_EQUAL(32, plLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(key, pl, 32);
}

void test_opcode_key_exchange_pktid_roundtrip() {
  uint8_t buf[128];
  uint8_t key[32];
  for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 7);
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 1, protocol::OP_KEY_EXCHANGE,
      key, 32, false, false, false, 0, 42);
  TEST_ASSERT_EQUAL(protocol::HEADER_LEN_PKTID + 32, len);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_KEY_EXCHANGE, hdr.opcode);
  TEST_ASSERT_EQUAL(42, hdr.pktId);
  TEST_ASSERT_EQUAL(32, plLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(key, pl, 32);
}

void test_opcode_route_req_broadcast() {
  uint8_t buf[64];
  uint8_t routePl[21];
  memset(routePl, 0xAB, 21);
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 5, protocol::OP_ROUTE_REQ,
      routePl, 21, false, false, false, 0, 0);
  TEST_ASSERT_GREATER_THAN(0, len);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_ROUTE_REQ, hdr.opcode);
  TEST_ASSERT_EQUAL(21, plLen);
}

void test_opcode_route_reply_unicast() {
  uint8_t buf[64];
  uint8_t routePl[21];
  memset(routePl, 0xCD, 21);
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 3, protocol::OP_ROUTE_REPLY,
      routePl, 21, false, false, false, 0, 0);
  TEST_ASSERT_GREATER_THAN(0, len);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_ROUTE_REPLY, hdr.opcode);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(s_to, hdr.to, 8);
}

void test_opcode_ping_pong_roundtrip() {
  uint8_t buf[64];
  size_t lenPing = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 1, protocol::OP_PING,
      nullptr, 0, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, lenPing, &hdr, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_PING, hdr.opcode);
  size_t lenPong = protocol::buildPacket(buf, sizeof(buf),
      s_to, s_from, 0, protocol::OP_PONG,
      nullptr, 0, false, false, false, 0, 0);
  ok = protocol::parsePacket(buf, lenPong, &hdr, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_PONG, hdr.opcode);
}

void test_opcode_read_unicast() {
  uint8_t buf[64];
  const uint8_t readPl[4] = {0x11, 0x22, 0x33, 0x44};
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 0, protocol::OP_READ,
      readPl, 4, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_READ, hdr.opcode);
  TEST_ASSERT_EQUAL(4, plLen);
}

void test_opcode_msg_frag_min_payload() {
  uint8_t buf[128];
  const uint8_t fragPl[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 2, protocol::OP_MSG_FRAG,
      fragPl, 6, true, false, false, 0, 0);
  TEST_ASSERT_GREATER_THAN(0, len);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_MSG_FRAG, hdr.opcode);
  TEST_ASSERT_EQUAL(6, plLen);
}

void test_opcode_group_msg_min_payload() {
  uint8_t buf[128];
  uint8_t grpPl[32];
  for (int i = 0; i < 32; i++) grpPl[i] = (uint8_t)i;
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 3, protocol::OP_GROUP_MSG,
      grpPl, 32, true, false, false, 0, 0);
  TEST_ASSERT_GREATER_THAN(0, len);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_GROUP_MSG, hdr.opcode);
}

// --- buildPacket граничные ---
void test_build_buffer_too_small() {
  uint8_t buf[5];
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 0, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  TEST_ASSERT_EQUAL(0, len);
}

void test_build_null_payload_zero_len() {
  uint8_t buf[64];
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 0, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  TEST_ASSERT_GREATER_THAN(0, len);
}

void test_build_all_flags() {
  uint8_t buf[64];
  uint8_t pl[32];
  memset(pl, 0xAA, 32);
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 1, protocol::OP_MSG,
      pl, 32, true, true, true, 2, 0);
  TEST_ASSERT_GREATER_THAN(0, len);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_TRUE(protocol::isEncrypted(hdr));
  TEST_ASSERT_TRUE(protocol::isAckReq(hdr));
  TEST_ASSERT_TRUE(protocol::isCompressed(hdr));
  TEST_ASSERT_EQUAL(2, hdr.channel);
}

void test_build_channel_values() {
  uint8_t buf[64];
  for (uint8_t ch = 0; ch <= 2; ch++) {
    size_t len = protocol::buildPacket(buf, sizeof(buf),
        s_from, protocol::BROADCAST_ID, 0, protocol::OP_HELLO,
        nullptr, 0, false, false, false, ch, 0);
    TEST_ASSERT_GREATER_THAN(0, len);
    protocol::PacketHeader hdr;
    bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(ch, hdr.channel);
  }
}

void test_build_ttl_values() {
  uint8_t buf[64];
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 255, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(255, hdr.ttl);
}

// --- parsePacket граничные ---
void test_parse_zero_length() {
  uint8_t buf[] = {0x5A};
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, 0, &hdr, nullptr, nullptr);
  TEST_ASSERT_FALSE(ok);
}

void test_parse_buffer_too_large_rejected() {
  uint8_t buf[512];
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 0, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  uint8_t big[512];
  memset(big, 0x5A, sizeof(big));
  memcpy(big, buf, len);
  bool ok = protocol::parsePacket(big, 400, &hdr, nullptr, nullptr);
  TEST_ASSERT_FALSE(ok);
}

// --- getExpectedPayloadRange все opcodes ---
void test_get_expected_payload_range_all() {
  size_t minP, maxP;
  const struct { uint8_t op; size_t minV; size_t maxV; } cases[] = {
    {protocol::OP_HELLO, 0, 0},
    {protocol::OP_ACK, 4, 4},
    {protocol::OP_KEY_EXCHANGE, 32, 32},
    {protocol::OP_ROUTE_REQ, 21, 21},
    {protocol::OP_ECHO, 12, 12},
    {protocol::OP_POLL, 0, 0},
    {protocol::OP_NACK, 2, 2},
    {protocol::OP_TELEMETRY, 28, 64},
    {protocol::OP_MSG, 29, protocol::MAX_PAYLOAD},
    {protocol::OP_GROUP_MSG, 32, protocol::MAX_PAYLOAD},
    {protocol::OP_MSG_FRAG, 6, protocol::MAX_PAYLOAD},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    bool ok = protocol::getExpectedPayloadRange(cases[i].op, &minP, &maxP);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(cases[i].minV, minP);
    TEST_ASSERT_EQUAL(cases[i].maxV, maxP);
  }
}

void test_get_expected_packet_length() {
  size_t v = protocol::getExpectedPacketLength(protocol::OP_HELLO, 0, true);
  TEST_ASSERT_EQUAL(0, v);
  v = protocol::getExpectedPacketLength(protocol::OP_ACK, 4, true);
  TEST_ASSERT_EQUAL(protocol::SYNC_LEN + 12 + 4, v);
  v = protocol::getExpectedPacketLength(protocol::OP_NACK, 2, false);
  TEST_ASSERT_EQUAL(protocol::SYNC_LEN + protocol::HEADER_LEN + 2, v);
}

// --- Доп. corruption ---
void test_corrupt_all_garbage_no_sync() {
  uint8_t buf[64];
  memset(buf, 0x42, sizeof(buf));
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, sizeof(buf), &hdr, nullptr, nullptr);
  TEST_ASSERT_FALSE(ok);
}

void test_corrupt_ack_wrong_length() {
  uint8_t buf[32];
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 0, protocol::OP_ACK,
      (const uint8_t*)"\x01\x02\x03\x04", 4, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len - 1, &hdr, nullptr, nullptr);
  TEST_ASSERT_FALSE(ok);
}

// --- Сценарии: mesh, E2E, telemetry, location ---
void test_scenario_mesh_chain_abc() {
  uint8_t nodeA[8] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8};
  uint8_t nodeB[8] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8};
  uint8_t nodeC[8] = {0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8};
  uint8_t buf[256];
  uint8_t pl[32];
  memset(pl, 0xDD, 32);
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      nodeA, nodeC, 2, protocol::OP_MSG,
      pl, 32, true, true, false, 0, 0);
  protocol::PacketHeader hdr;
  const uint8_t* outPl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &outPl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(nodeA, hdr.from, 8);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(nodeC, hdr.to, 8);
  TEST_ASSERT_EQUAL(2, hdr.ttl);
}

void test_scenario_key_exchange_then_msg() {
  uint8_t buf[128];
  uint8_t pubKey[32];
  for (int i = 0; i < 32; i++) pubKey[i] = (uint8_t)(i ^ 0x55);
  size_t lenKx = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 1, protocol::OP_KEY_EXCHANGE,
      pubKey, 32, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, lenKx, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_KEY_EXCHANGE, hdr.opcode);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(pubKey, pl, 32);
}

void test_scenario_telemetry_roundtrip() {
  uint8_t buf[128];
  uint8_t telem[32];
  for (int i = 0; i < 32; i++) telem[i] = (uint8_t)(i + 0x80);
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 1, protocol::OP_TELEMETRY,
      telem, 32, true, false, false, 0, 0);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_TELEMETRY, hdr.opcode);
  TEST_ASSERT_EQUAL(32, plLen);
}

void test_scenario_location_roundtrip() {
  uint8_t buf[256];
  uint8_t loc[48];
  memset(loc, 0x77, 48);
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 3, protocol::OP_LOCATION,
      loc, 48, true, false, false, 0, 0);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_LOCATION, hdr.opcode);
  TEST_ASSERT_EQUAL(48, plLen);
}

void test_scenario_voice_msg_roundtrip() {
  uint8_t buf[256];
  uint8_t voice[64];
  for (int i = 0; i < 64; i++) voice[i] = (uint8_t)(i | 0x40);
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 1, protocol::OP_VOICE_MSG,
      voice, 64, true, true, false, 0, 0);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_VOICE_MSG, hdr.opcode);
  TEST_ASSERT_EQUAL(64, plLen);
}

void test_scenario_xor_relay_meta() {
  uint8_t buf[256];
  uint8_t meta[40];
  memset(meta, 0x11, 40);
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_relay, protocol::BROADCAST_ID, 2, protocol::OP_XOR_RELAY,
      meta, 40, true, false, false, 0, 0);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_XOR_RELAY, hdr.opcode);
  TEST_ASSERT_EQUAL(40, plLen);
}

void test_scenario_payload_integrity() {
  uint8_t buf[256];
  uint8_t payload[50];
  for (int i = 0; i < 50; i++) payload[i] = (uint8_t)((i * 31 + 17) & 0xFF);
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 1, protocol::OP_MSG,
      payload, 50, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  bool ok = protocol::parsePacket(buf, len, &hdr, &pl, &plLen);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(50, plLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, pl, 50);
}

void test_scenario_node_id_all_zeros_rejected() {
  uint8_t zero[8] = {0};
  uint8_t buf[64];
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      zero, protocol::BROADCAST_ID, 0, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_FALSE(ok);
}

void test_scenario_unicast_ack() {
  uint8_t buf[64];
  const uint8_t ackPl[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 0, protocol::OP_ACK,
      ackPl, 4, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_ACK, hdr.opcode);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(s_to, hdr.to, 8);
}

void test_scenario_exact_buffer_size() {
  size_t need = protocol::SYNC_LEN + 12 + 0;
  uint8_t buf[32];
  size_t len = protocol::buildPacket(buf, need,
      s_from, protocol::BROADCAST_ID, 0, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  TEST_ASSERT_EQUAL(need, len);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
}

void test_scenario_flags_independent() {
  uint8_t buf[128];
  uint8_t pl[32];
  memset(pl, 0x22, 32);
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, s_to, 1, protocol::OP_MSG,
      pl, 32, true, false, false, 0, 0);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_TRUE(protocol::isEncrypted(hdr));
  TEST_ASSERT_FALSE(protocol::isAckReq(hdr));
  TEST_ASSERT_FALSE(protocol::isCompressed(hdr));
}

void test_scenario_garbage_between_packets() {
  uint8_t pkt1[64], pkt2[64];
  size_t len1 = protocol::buildPacket(pkt1, sizeof(pkt1),
      s_from, protocol::BROADCAST_ID, 1, protocol::OP_HELLO,
      nullptr, 0, false, false, false, 0, 0);
  size_t len2 = protocol::buildPacket(pkt2, sizeof(pkt2),
      s_to, protocol::BROADCAST_ID, 2, protocol::OP_PONG,
      nullptr, 0, false, false, false, 0, 0);
  uint8_t buf[160];
  memcpy(buf, pkt1, len1);
  memset(buf + len1, 0xCC, 10);
  memcpy(buf + len1 + 10, pkt2, len2);
  // parsePacket ожидает точную длину пакета — передаём только len1, чтобы распарсить первый
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len1, &hdr, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL(protocol::OP_HELLO, hdr.opcode);
}

void test_get_expected_packet_length_extended() {
  size_t v = protocol::getExpectedPacketLength(protocol::OP_ROUTE_REQ, 21, true);
  TEST_ASSERT_EQUAL(protocol::HEADER_LEN_BROADCAST + 21, v);
  v = protocol::getExpectedPacketLength(protocol::OP_ECHO, 12, true);
  TEST_ASSERT_EQUAL(protocol::HEADER_LEN_BROADCAST + 12, v);
  v = protocol::getExpectedPacketLength(protocol::OP_KEY_EXCHANGE, 32, false);
  TEST_ASSERT_EQUAL(protocol::SYNC_LEN + protocol::HEADER_LEN + 32, v);
  v = protocol::getExpectedPacketLength(protocol::OP_KEY_EXCHANGE, 32, false, true);
  TEST_ASSERT_EQUAL(protocol::HEADER_LEN_PKTID + 32, v);
}

void test_scenario_route_req_always_broadcast() {
  uint8_t buf[64];
  uint8_t routePl[21];
  memset(routePl, 0, 21);
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_from, protocol::BROADCAST_ID, 5, protocol::OP_ROUTE_REQ,
      routePl, 21, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(protocol::BROADCAST_ID, hdr.to, 8);
}

void test_scenario_read_unicast_to_sender() {
  uint8_t buf[64];
  const uint8_t readPl[4] = {0x01, 0x00, 0x00, 0x00};
  size_t len = protocol::buildPacket(buf, sizeof(buf),
      s_to, s_from, 0, protocol::OP_READ,
      readPl, 4, false, false, false, 0, 0);
  protocol::PacketHeader hdr;
  bool ok = protocol::parsePacket(buf, len, &hdr, nullptr, nullptr);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(s_from, hdr.to, 8);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_build_parse_broadcast_roundtrip);
  RUN_TEST(test_build_parse_unicast_roundtrip);
  RUN_TEST(test_parse_invalid_too_short);
  RUN_TEST(test_broadcast_id_constant);
  RUN_TEST(test_sync_byte_in_output);
  RUN_TEST(test_corrupt_garbage_before_packet);
  RUN_TEST(test_corrupt_shifted_sync_sx1262);
  RUN_TEST(test_shifted_parse_all_core_opcodes_with_offset);
  RUN_TEST(test_shifted_parse_status_payload_range_for_bad_ack_len);
  RUN_TEST(test_parse_status_invalid_ids);
  RUN_TEST(test_corrupt_wrong_sync_byte);
  RUN_TEST(test_corrupt_wrong_version);
  RUN_TEST(test_corrupt_truncated_packet);
  RUN_TEST(test_corrupt_hello_with_payload_rejected);
  RUN_TEST(test_corrupt_msg_payload_too_short);
  RUN_TEST(test_relay_broadcast_hello);
  RUN_TEST(test_relay_unicast_from_to);
  RUN_TEST(test_relay_echo_broadcast);
  RUN_TEST(test_relay_poll_rit);
  RUN_TEST(test_edge_ack_broadcast);
  RUN_TEST(test_edge_nack_payload);
  RUN_TEST(test_edge_max_payload);
  RUN_TEST(test_edge_get_expected_payload_range);
  RUN_TEST(test_edge_multiple_packets_parse_first_only);
  RUN_TEST(test_opcode_key_exchange_roundtrip);
  RUN_TEST(test_opcode_key_exchange_pktid_roundtrip);
  RUN_TEST(test_opcode_route_req_broadcast);
  RUN_TEST(test_opcode_route_reply_unicast);
  RUN_TEST(test_opcode_ping_pong_roundtrip);
  RUN_TEST(test_opcode_read_unicast);
  RUN_TEST(test_opcode_msg_frag_min_payload);
  RUN_TEST(test_opcode_group_msg_min_payload);
  RUN_TEST(test_build_buffer_too_small);
  RUN_TEST(test_build_null_payload_zero_len);
  RUN_TEST(test_build_all_flags);
  RUN_TEST(test_build_channel_values);
  RUN_TEST(test_build_ttl_values);
  RUN_TEST(test_parse_zero_length);
  RUN_TEST(test_parse_buffer_too_large_rejected);
  RUN_TEST(test_get_expected_payload_range_all);
  RUN_TEST(test_get_expected_packet_length);
  RUN_TEST(test_corrupt_all_garbage_no_sync);
  RUN_TEST(test_corrupt_ack_wrong_length);
  RUN_TEST(test_scenario_mesh_chain_abc);
  RUN_TEST(test_scenario_key_exchange_then_msg);
  RUN_TEST(test_scenario_telemetry_roundtrip);
  RUN_TEST(test_scenario_location_roundtrip);
  RUN_TEST(test_scenario_voice_msg_roundtrip);
  RUN_TEST(test_scenario_xor_relay_meta);
  RUN_TEST(test_scenario_payload_integrity);
  RUN_TEST(test_scenario_node_id_all_zeros_rejected);
  RUN_TEST(test_scenario_unicast_ack);
  RUN_TEST(test_scenario_exact_buffer_size);
  RUN_TEST(test_scenario_flags_independent);
  RUN_TEST(test_scenario_garbage_between_packets);
  RUN_TEST(test_get_expected_packet_length_extended);
  RUN_TEST(test_scenario_route_req_always_broadcast);
  RUN_TEST(test_scenario_read_unicast_to_sender);
  return UNITY_END();
}
