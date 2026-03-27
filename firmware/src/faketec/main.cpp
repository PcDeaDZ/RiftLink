/**
 * RiftLink FakeTech V5 — nRF52840 (NiceNano) + HT-RA62/RA-01SH
 * Паритет с Heltec V3 по крипте и формату MSG/GROUP_MSG/KEY_EXCHANGE (см. firmware/src/main.cpp).
 */

#include <Arduino.h>
#include "board.h"
#include "storage.h"
#include "node.h"
#include "region.h"
#include "crypto.h"
#include "radio.h"
#include "display.h"
#include "ble.h"
#include "heap_metrics.h"
#include "neighbors.h"
#include "protocol/packet.h"
#include "compress/compress.h"
#include "log.h"
#include "x25519_keys.h"
#include "routing/routing.h"
#include "msg_queue/msg_queue.h"
#include "async_tx.h"
#include "frag/frag.h"
#include "pkt_cache/pkt_cache.h"
#include "ack_coalesce/ack_coalesce.h"
#include "packet_fusion/packet_fusion.h"
#include "network_coding/network_coding.h"
#include "clock_drift/clock_drift.h"
#include "beacon_sync/beacon_sync.h"
#include "offline_queue/offline_queue.h"
#include "groups/groups.h"
#include "mab/mab.h"
#include "collision_slots/collision_slots.h"
#include "voice_frag/voice_frag.h"
#include "telemetry_nrf.h"
#include "nrf_wdt.h"
#include <string.h>

#define HELLO_INTERVAL_MS 36000
/** Как на Heltec V3: чаще HELLO при discovery (0 соседей) и при одном соседе. */
#define HELLO_INTERVAL_AGGRESSIVE_MS 12000
#define HELLO_INTERVAL_ONE_NEIGH_MS 24000
#define HELLO_JITTER_MS 3000
#define HELLO_FIRST_MS 5000
/** Как main.cpp Heltec: не чаще HELLO в эфир раз в 10 с; тихое окно после KEY / handshake. */
#define HELLO_HARD_MIN_INTERVAL_MS 10000
#define HELLO_QUIET_AFTER_KEY_MS 2500
#define HANDSHAKE_TRAFFIC_QUIET_MS 3000

/** Как main.cpp (Heltec): проверка radius в OP_LOCATION. */
static constexpr uint32_t GEOFENCE_RADIUS_MAX_M = 50000;

static uint32_t s_lastHelloAirMs = 0;
static uint32_t s_handshakeQuietUntilMs = 0;
static uint32_t s_lastObservedKeyTxMs = 0;
static uint32_t s_lastKeyPairingHintMs = 0;
/** Корреляция с `radio::RX_RECOVERY` — при смене seq ожидаем снова HELLO/KEY в логе. */
static uint32_t s_mainLastRxRecoverySeq = 0;
/** В этой итерации loop уже вывели HEARTBEAT — KEY_HINT переносим в конец loop после offline/BLE (не создавать ложную связь с «зависанием после строки»). */
static bool s_heartbeatEmittedThisLoop = false;

static inline bool isHandshakeQuietActive() { return (int32_t)(s_handshakeQuietUntilMs - millis()) > 0; }

static uint32_t helloIntervalMsForNetwork() {
  const int n = neighbors::getCount();
  if (n <= 0) return HELLO_INTERVAL_AGGRESSIVE_MS;
  if (n == 1) return HELLO_INTERVAL_ONE_NEIGH_MS;
  return HELLO_INTERVAL_MS;
}
#ifndef RIFTLINK_SKIP_BLE
static void pollBleForRadio(void) { ble::update(); }
/** evt:neighbors из handlePacket после долгого radio::receive(): иначе NUS notify без свежего ble::update() может подвиснуть SoftDevice. */
static bool s_bleNeighborsNotifyPending = false;
#endif

/** Формат plaintext как msg_queue (ESP): unicast V3 и broadcast GROUP_ALL. */
static constexpr size_t MSG_ID_LEN = 4;
static constexpr uint32_t GROUP_ALL = 1;

static uint32_t nextHelloDue = 0;
static uint8_t s_xorDecoded[512];
static uint8_t s_fragOutBuf[2048];
static char s_msgBatchText[256];

/** Дедупликация повторов broadcast GROUP_ALL (как main.cpp ESP, BC_DEDUP_SIZE). */
#define BC_DEDUP_SIZE 32
struct BcDedupEntry {
  uint8_t from[protocol::NODE_ID_LEN];
  uint32_t msgId;
};
static BcDedupEntry s_bcDedup[BC_DEDUP_SIZE];
static uint8_t s_bcDedupIdx = 0;

static bool bcDedupSeen(const uint8_t* from, uint32_t msgId) {
  for (int i = 0; i < BC_DEDUP_SIZE; i++) {
    if (memcmp(s_bcDedup[i].from, from, protocol::NODE_ID_LEN) == 0 && s_bcDedup[i].msgId == msgId) return true;
  }
  return false;
}

static void bcDedupAdd(const uint8_t* from, uint32_t msgId) {
  memcpy(s_bcDedup[s_bcDedupIdx].from, from, protocol::NODE_ID_LEN);
  s_bcDedup[s_bcDedupIdx].msgId = msgId;
  s_bcDedupIdx = (uint8_t)((s_bcDedupIdx + 1u) % BC_DEDUP_SIZE);
}

/** Relay dedup + rate (firmware/src/main.cpp ~349–397). */
#define RELAY_DEDUP_SIZE 24
#define RELAY_RATE_MAX 3
#define RELAY_RATE_WINDOW_MS 1000
struct RelayDedupEntry {
  uint8_t from[protocol::NODE_ID_LEN];
  uint32_t payloadHash;
};
static RelayDedupEntry s_relayDedup[RELAY_DEDUP_SIZE];
static uint8_t s_relayDedupIdx = 0;
static uint32_t s_relayRateWindowStart = 0;
static uint8_t s_relayRateCount = 0;

static uint32_t relayPayloadHash(const uint8_t* p, size_t n) {
  uint32_t h = 5381;
  for (size_t i = 0; i < n && i < 128; i++) h = ((h << 5) + h) + p[i];
  return h;
}
static bool relayDedupSeen(const uint8_t* from, uint32_t hash) {
  for (int i = 0; i < RELAY_DEDUP_SIZE; i++) {
    if (memcmp(s_relayDedup[i].from, from, protocol::NODE_ID_LEN) == 0 && s_relayDedup[i].payloadHash == hash) return true;
  }
  return false;
}
static void relayDedupAdd(const uint8_t* from, uint32_t hash) {
  memcpy(s_relayDedup[s_relayDedupIdx].from, from, protocol::NODE_ID_LEN);
  s_relayDedup[s_relayDedupIdx].payloadHash = hash;
  s_relayDedupIdx = (uint8_t)((s_relayDedupIdx + 1u) % RELAY_DEDUP_SIZE);
}
static bool relayRateLimitExceeded() {
  uint32_t now = millis();
  if (now - s_relayRateWindowStart >= RELAY_RATE_WINDOW_MS) {
    s_relayRateWindowStart = now;
    s_relayRateCount = 0;
  }
  return s_relayRateCount >= RELAY_RATE_MAX;
}
static void relayRateRecord() { s_relayRateCount++; }

#define SOS_RELAY_RATE_MAX 5
#define SOS_RELAY_RATE_WINDOW_MS 2000
#define SOS_RELAY_PER_SRC_MAX 2
#define SOS_RELAY_PER_SRC_WINDOW_MS 2500
static uint32_t s_sosRelayRateWindowStart = 0;
static uint8_t s_sosRelayRateCount = 0;
struct SosRelaySrcQuota {
  uint8_t from[protocol::NODE_ID_LEN];
  uint32_t windowStartMs;
  uint8_t count;
  bool used;
};
static SosRelaySrcQuota s_sosSrcQuota[8];

static bool relaySosRateLimitExceeded() {
  uint32_t now = millis();
  if (now - s_sosRelayRateWindowStart >= SOS_RELAY_RATE_WINDOW_MS) {
    s_sosRelayRateWindowStart = now;
    s_sosRelayRateCount = 0;
  }
  return s_sosRelayRateCount >= SOS_RELAY_RATE_MAX;
}
static void relaySosRateRecord() { s_sosRelayRateCount++; }
static int findSosSrcQuota(const uint8_t* from) {
  for (int i = 0; i < 8; i++) {
    if (s_sosSrcQuota[i].used && memcmp(s_sosSrcQuota[i].from, from, protocol::NODE_ID_LEN) == 0) return i;
  }
  return -1;
}
static int findFreeSosSrcQuota() {
  uint32_t now = millis();
  for (int i = 0; i < 8; i++) {
    if (!s_sosSrcQuota[i].used) return i;
    if (now - s_sosSrcQuota[i].windowStartMs >= SOS_RELAY_PER_SRC_WINDOW_MS) return i;
  }
  int oldest = 0;
  uint32_t oldestTs = s_sosSrcQuota[0].windowStartMs;
  for (int i = 1; i < 8; i++) {
    if (s_sosSrcQuota[i].windowStartMs < oldestTs) {
      oldestTs = s_sosSrcQuota[i].windowStartMs;
      oldest = i;
    }
  }
  return oldest;
}
static bool relaySosPerSourceLimitExceeded(const uint8_t* from) {
  uint32_t now = millis();
  int idx = findSosSrcQuota(from);
  if (idx < 0) {
    idx = findFreeSosSrcQuota();
    memcpy(s_sosSrcQuota[idx].from, from, protocol::NODE_ID_LEN);
    s_sosSrcQuota[idx].windowStartMs = now;
    s_sosSrcQuota[idx].count = 0;
    s_sosSrcQuota[idx].used = true;
  }
  if (now - s_sosSrcQuota[idx].windowStartMs >= SOS_RELAY_PER_SRC_WINDOW_MS) {
    s_sosSrcQuota[idx].windowStartMs = now;
    s_sosSrcQuota[idx].count = 0;
  }
  return s_sosSrcQuota[idx].count >= SOS_RELAY_PER_SRC_MAX;
}
static void relaySosPerSourceRecord(const uint8_t* from) {
  int idx = findSosSrcQuota(from);
  if (idx < 0) {
    idx = findFreeSosSrcQuota();
    memcpy(s_sosSrcQuota[idx].from, from, protocol::NODE_ID_LEN);
    s_sosSrcQuota[idx].windowStartMs = millis();
    s_sosSrcQuota[idx].count = 0;
    s_sosSrcQuota[idx].used = true;
  }
  s_sosSrcQuota[idx].count++;
}

static uint8_t rxBuf[protocol::SYNC_LEN + protocol::HEADER_LEN + protocol::MAX_PAYLOAD + 64];
static uint8_t s_relayBuf[protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID + protocol::MAX_PAYLOAD + 64];
static uint8_t s_decodedPending[512];

static void handlePacket(const uint8_t* buf, int len);

/** Как main.cpp ~1084–1187: XOR/defer/courier/overhear, без немедленного radio::send. */
static void processRelayPipeline(const protocol::PacketHeader& hdr, const uint8_t* buf, int len, const uint8_t* payload,
    size_t payloadLen, int rssi) {
  if (hdr.ttl <= 0) return;
  if (hdr.opcode == protocol::OP_ROUTE_REQ || hdr.opcode == protocol::OP_ROUTE_REPLY ||
      hdr.opcode == protocol::OP_HELLO || hdr.opcode == protocol::OP_SF_BEACON || hdr.opcode == protocol::OP_NACK ||
      hdr.opcode == protocol::OP_KEY_EXCHANGE) {
    /* KEY — только прямой unicast; mesh не ретранслирует (см. KEY_HINT / docs). */
    return;
  }
  const bool needRelay =
      (!node::isForMe(hdr.to) && !node::isBroadcast(hdr.to)) ||
      ((hdr.opcode == protocol::OP_GROUP_MSG || hdr.opcode == protocol::OP_VOICE_MSG ||
        hdr.opcode == protocol::OP_SOS) &&
       neighbors::getCount() >= 2);
  if (!needRelay) return;

  const uint32_t relayPayloadHashVal = relayPayloadHash(payload ? payload : (const uint8_t*)"", payloadLen);
  if (relayDedupSeen(hdr.from, relayPayloadHashVal)) {
    relayHeard(hdr.from, relayPayloadHashVal);
    return;
  }
  if (hdr.opcode == protocol::OP_SOS) {
    if (relaySosRateLimitExceeded() || relaySosPerSourceLimitExceeded(hdr.from)) return;
  } else if (relayRateLimitExceeded()) {
    return;
  }

  relayDedupAdd(hdr.from, relayPayloadHashVal);
  if (hdr.opcode == protocol::OP_SOS) {
    relaySosRateRecord();
    relaySosPerSourceRecord(hdr.from);
  } else {
    relayRateRecord();
  }

  if (hdr.opcode == protocol::OP_MSG && hdr.pktId != 0 && !node::isBroadcast(hdr.to)) {
    pkt_cache::addOverheard(hdr.from, hdr.to, hdr.pktId, buf, (size_t)len);
    (void)offline_queue::enqueueCourier(buf, (size_t)len);
  }

  uint8_t txSf = 0;
  if (node::isBroadcast(hdr.to)) {
    const int minRssi = neighbors::getMinRssi();
    txSf = neighbors::rssiToSf(minRssi);
  } else {
    uint8_t nextHop[protocol::NODE_ID_LEN];
    if (routing::getNextHop(hdr.to, nextHop)) {
      const int r = neighbors::getRssiFor(nextHop);
      txSf = neighbors::rssiToSf(r);
    }
    if (txSf == 0) txSf = 12;
  }
  const int rssiClamp = (rssi < -120) ? -120 : (rssi > -40 ? -40 : rssi);
  const int32_t d = 10 * (rssiClamp + 100);
  uint32_t delayMs = (d <= 0) ? 0 : ((d > 500) ? 500 : (uint32_t)d);
  if (hdr.opcode == protocol::OP_SOS) {
    delayMs += 60 + (uint32_t)random(90);
  }

  bool xorSent = false;
  if (network_coding::addForXor(buf, (size_t)len, hdr.from, hdr.to)) {
    uint8_t otherFrom[protocol::NODE_ID_LEN];
    uint32_t otherHash = 0;
    network_coding::getLastPairOther(otherFrom, &otherHash);
    relayHeard(otherFrom, otherHash);
    uint8_t xorBuf[sizeof(s_relayBuf)];
    size_t xorLen = 0;
    if (network_coding::getXorPacket(xorBuf, sizeof(xorBuf), &xorLen) && xorLen > 0) {
      queueDeferredSend(xorBuf, xorLen, txSf, delayMs, false);
      xorSent = true;
    }
  }
  if (!xorSent) {
    size_t decodedLen = 0;
    if (network_coding::getDecodedFromPending(buf, (size_t)len, hdr.from, hdr.to, hdr.pktId, s_decodedPending,
            &decodedLen) &&
        decodedLen > 0 && decodedLen <= sizeof(s_decodedPending)) {
      handlePacket(s_decodedPending, (int)decodedLen);
    }
    const size_t ttlOff = protocol::ttlFieldOffsetBytes(hdr);
    if (len > (int)ttlOff && (size_t)len <= sizeof(s_relayBuf)) {
      memcpy(s_relayBuf, buf, (size_t)len);
      s_relayBuf[ttlOff] = (uint8_t)(hdr.ttl - 1);
      queueDeferredRelay(s_relayBuf, (size_t)len, txSf, delayMs, hdr.from, relayPayloadHashVal, false);
    }
#ifndef RIFTLINK_SKIP_BLE
    if ((random(100)) < 35) {
      ble::notifyRelayProof(node::getId(), hdr.from, hdr.to, hdr.pktId, hdr.opcode);
    }
#endif
  }
}

static inline uint16_t helloSenderTag16(const uint8_t* nodeId) {
  if (!nodeId) return 0;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    h ^= nodeId[i];
    h *= 16777619u;
  }
  return (uint16_t)((h >> 16) ^ (h & 0xFFFFu));
}

static bool validateHelloSenderTag(const protocol::PacketHeader& hdr, const uint8_t* payload, size_t payloadLen) {
  if (!payload || payloadLen != 2) return false;
  uint16_t got = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
  return got == helloSenderTag16(hdr.from);
}

static bool sendHello() {
  uint8_t pkt[64];
  uint8_t helloPayload[2];
  uint16_t helloTag = helloSenderTag16(node::getId());
  helloPayload[0] = (uint8_t)(helloTag & 0xFF);
  helloPayload[1] = (uint8_t)(helloTag >> 8);
  size_t n = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_HELLO,
                                     helloPayload, sizeof(helloPayload), false, false, false, protocol::CHANNEL_DEFAULT, 0);
  if (n == 0) return false;
  const unsigned heapNow = (unsigned)heap_metrics_free_bytes();
  if (radio::send(pkt, n)) {
    s_lastHelloAirMs = millis();
    RIFTLINK_DIAG("HELLO", "event=HELLO_TX ok=1 heap=%u", heapNow);
    if (RIFTLINK_SERIAL_TX_HAS_SPACE()) Serial.printf("[RiftLink] HELLO sent heap=%u\n", heapNow);
    return true;
  }
  RIFTLINK_DIAG("HELLO", "event=HELLO_TX ok=0 err=%d heap=%u", (int)radio::getLastTxError(), heapNow);
  return false;
}

static void handlePacket(const uint8_t* buf, int len) {
  protocol::PacketHeader hdr;
  const uint8_t* payload;
  size_t payloadLen;

  if (!protocol::parsePacket(buf, len, &hdr, &payload, &payloadLen)) {
    static uint32_t s_lastParseFailLog = 0;
    const uint32_t t = millis();
    if (t - s_lastParseFailLog >= 15000) {
      s_lastParseFailLog = t;
      RIFTLINK_DIAG("PARSE", "event=RX_PARSE_FAIL len=%d b0=%02X b1=%02X b2=%02X (wrong SF/sync или мусор)", len,
          len > 0 ? buf[0] : 0, len > 1 ? buf[1] : 0, len > 2 ? buf[2] : 0);
    }
    return;
  }

  if (hdr.opcode == protocol::OP_XOR_RELAY) {
    size_t dlen = 0;
    if (network_coding::onXorRelayReceived(buf, (size_t)len, s_xorDecoded, &dlen) && dlen > 0 &&
        dlen <= sizeof(s_xorDecoded)) {
      handlePacket(s_xorDecoded, (int)dlen);
    }
    return;
  }

  if (hdr.opcode == protocol::OP_HELLO && !node::isBroadcast(hdr.to)) {
    RIFTLINK_DIAG("HELLO", "event=HELLO_DROP reason=to_not_broadcast from=%02X%02X", hdr.from[0], hdr.from[1]);
    return;
  }

  if (node::isBroadcast(hdr.from) || node::isInvalidNodeId(hdr.from)) return;

  /* Паритет с firmware/src/main.cpp: не разгонять петли hello/key из самослушания в эфире. */
  if (hdr.opcode == protocol::OP_KEY_EXCHANGE &&
      memcmp(hdr.from, node::getId(), protocol::NODE_ID_LEN) == 0) {
    return;
  }
  if (node::isForMe(hdr.from)) {
    RIFTLINK_DIAG("RADIO", "event=RX_DROP_DUP cause=self_from op=0x%02X pktId=%u", (unsigned)hdr.opcode,
        (unsigned)hdr.pktId);
    return;
  }

  const int rssi = radio::getLastRssi();
  processRelayPipeline(hdr, buf, len, payload, payloadLen, rssi);

  /* Как firmware/src/main.cpp ~1189–1231: unicast «не нам» — только ретрансляция/маршрут, без крипты. */
  if (!node::isForMe(hdr.to) && !node::isBroadcast(hdr.to)) {
    if (hdr.opcode == protocol::OP_ROUTE_REQ) {
      routing::onRouteReq(hdr.from, payload, payloadLen);
    } else if (hdr.opcode == protocol::OP_ROUTE_REPLY) {
      (void)routing::onRouteReply(hdr.from, hdr.to, payload, payloadLen);
    } else if (hdr.opcode == protocol::OP_KEY_EXCHANGE) {
      if (payloadLen == 32) {
        RIFTLINK_DIAG("KEY", "event=KEY_RX_SKIP reason=not_for_me from=%02X%02X to=%02X%02X pktId=%u",
            hdr.from[0], hdr.from[1], hdr.to[0], hdr.to[1], (unsigned)hdr.pktId);
      } else {
        RIFTLINK_DIAG("KEY", "event=KEY_RX_PARSE_FAIL cause=payload_len_ne_32 from=%02X%02X payload=%u pktId=%u",
            hdr.from[0], hdr.from[1], (unsigned)payloadLen, (unsigned)hdr.pktId);
      }
    }
    return;
  }

  neighbors::updateRssi(hdr.from, rssi);

  switch (hdr.opcode) {
    case protocol::OP_KEY_EXCHANGE:
      RIFTLINK_DIAG("KEY", "event=KEY_RX_RAW from=%02X%02X len=%u rssi=%d pktId=%u to_me=1",
          hdr.from[0], hdr.from[1], (unsigned)payloadLen, rssi, (unsigned)hdr.pktId);
      if (payloadLen != 32) {
        RIFTLINK_DIAG("KEY", "event=KEY_RX_BAD_LEN from=%02X%02X payloadLen=%u (expected 32)",
            hdr.from[0], hdr.from[1], (unsigned)payloadLen);
        break;
      }
      if (node::isBroadcast(hdr.from) || node::isInvalidNodeId(hdr.from)) break;
      (void)neighbors::onHello(hdr.from, rssi);
      {
        const bool keyMismatch = x25519_keys::isPeerPubKeyMismatch(hdr.from, payload);
        if (keyMismatch) {
          RIFTLINK_DIAG("KEY", "event=KEY_ROTATE peer=%02X%02X (pubkey changed, accepting)",
              hdr.from[0], hdr.from[1]);
        }
        const bool hadKey = x25519_keys::hasKeyFor(hdr.from);
        if (hadKey && !keyMismatch) {
          x25519_keys::sendKeyExchange(hdr.from, true, true, "key_rx_dup");
          break;
        }
        x25519_keys::onKeyExchange(hdr.from, payload);
        if (x25519_keys::hasKeyFor(hdr.from)) {
          /* Сначала ответный KEY в эфир, затем evt:neighbors (отложенно + ble::update). */
          x25519_keys::sendKeyExchange(hdr.from, true, false, "key_rx");
#ifndef RIFTLINK_SKIP_BLE
          s_bleNeighborsNotifyPending = true;
#endif
        }
      }
      break;

    case protocol::OP_HELLO:
      /* Неверный to отсекается до switch; здесь только валидный broadcast HELLO. */
      if (!validateHelloSenderTag(hdr, payload, payloadLen)) {
        RIFTLINK_DIAG("HELLO", "event=HELLO_DROP reason=tag_invalid from=%02X%02X len=%u", hdr.from[0], hdr.from[1],
            (unsigned)payloadLen);
        break;
      }
      {
        const bool newNeighbor = neighbors::onHello(hdr.from, rssi);
        /* Паритет firmware/src/main.cpp (V3/V4): HELLO_RX на каждый валидный HELLO, не только при новом слоте. */
        RIFTLINK_DIAG("HELLO", "event=HELLO_RX from=%02X%02X rssi=%d sf=%u heap=%u neighbor_new=%u", hdr.from[0],
            hdr.from[1], rssi, (unsigned)radio::getSpreadingFactor(), (unsigned)heap_metrics_free_bytes(),
            (unsigned)newNeighbor);
        if (newNeighbor) {
          if (display::isPresent()) {
            display::clear();
            display::setCursor(0, 0);
            display::print("Neighbors: ");
            display::print(neighbors::getCount());
            display::show();
          }
        }
        /* Как на ESP: обновлять evt:neighbors при каждом HELLO (RSSI + hasKey), не только при первом слоте. */
#ifndef RIFTLINK_SKIP_BLE
        s_bleNeighborsNotifyPending = true;
#endif
        clock_drift::onHelloReceived(hdr.from);
        beacon_sync::onBeaconReceived(hdr.from);
        offline_queue::onNodeOnline(hdr.from);
      }
      if (!x25519_keys::hasKeyFor(hdr.from)) {
        /* Случайный сдвиг относительно HELLO пира — меньше одновременный TX KEY с двух сторон. */
        delay((uint32_t)random(12, 96));
        yield();
        x25519_keys::sendKeyExchange(hdr.from, false, false, "hello_fwd");
      }
      break;

    case protocol::OP_MSG: {
      const bool forUs = node::isForMe(hdr.to) || node::isBroadcast(hdr.to);
      if (forUs && payloadLen > 0) {
        if (!protocol::isEncrypted(hdr)) {
          RIFTLINK_DIAG("PARSE", "event=RX_DROP_FAST reason=missing_encrypted_flag op=OP_MSG from=%02X%02X",
              hdr.from[0], hdr.from[1]);
          break;
        }
        uint8_t decBuf[256];
        uint8_t tmpBuf[256];
        size_t decLen = sizeof(decBuf);
        if (!crypto::decryptFrom(hdr.from, payload, payloadLen, decBuf, &decLen) || decLen >= 256) {
          const bool hasKeyNow = x25519_keys::hasKeyFor(hdr.from);
          RIFTLINK_DIAG("KEY", "event=KEY_DECRYPT_FAIL type=op_msg from=%02X%02X has_key=%u len=%u pktId=%u",
              hdr.from[0], hdr.from[1], (unsigned)hasKeyNow, (unsigned)payloadLen, (unsigned)hdr.pktId);
          x25519_keys::sendKeyExchange(hdr.from, true, hasKeyNow, hasKeyNow ? "decrypt_fail_msg" : "msg_no_key");
          break;
        }
        if (protocol::isCompressed(hdr)) {
          size_t d = compress::decompress(decBuf, decLen, tmpBuf, sizeof(tmpBuf));
          if (d == 0 || d >= 256) {
            RIFTLINK_DIAG("MSG", "event=MSG_DROP reason=decompress_fail from=%02X%02X", hdr.from[0], hdr.from[1]);
            break;
          }
          memcpy(decBuf, tmpBuf, d);
          decLen = d;
        }

        const bool isBroadcastMsg = node::isBroadcast(hdr.to);
        const bool ackEligible = protocol::isAckReq(hdr) && decLen >= MSG_ID_LEN && node::isForMe(hdr.to) &&
                                 !isBroadcastMsg;

        uint32_t msgId = 0;
        const char* msgPtr = nullptr;
        size_t msgLen = 0;

        if (ackEligible) {
          memcpy(&msgId, decBuf, MSG_ID_LEN);
          size_t msgOff = MSG_ID_LEN;
          if (decLen > msgOff) {
            uint8_t flags = decBuf[msgOff++];
            if ((flags & 0x01) != 0 && decLen >= msgOff + 1) {
              uint8_t piggyCount = decBuf[msgOff++];
              for (uint8_t pi = 0; pi < piggyCount && decLen >= msgOff + MSG_ID_LEN; pi++) {
                msgOff += MSG_ID_LEN;
              }
            }
          }
          msgPtr = (const char*)(decBuf + msgOff);
          msgLen = decLen > msgOff ? decLen - msgOff : 0;
        } else {
          if (protocol::isAckReq(hdr) && isBroadcastMsg) {
            RIFTLINK_DIAG("ACK", "event=ACK_SUPPRESSED type=op_msg mode=broadcast_echo_only from=%02X%02X",
                hdr.from[0], hdr.from[1]);
          }
          msgPtr = (const char*)decBuf;
          msgLen = decLen;
        }

        if (msgLen < 256) {
          char text[256];
          memcpy(text, msgPtr, msgLen);
          text[msgLen] = '\0';
          ble::notifyMsg(hdr.from, text, msgId, rssi, 0);
          if (display::isPresent()) {
            display::clear();
            display::setCursor(0, 0);
            display::print("MSG:");
            display::setCursor(0, 1);
            for (size_t i = 0; i < msgLen && i < 16; i++) display::print((char)text[i]);
            display::show();
          }
          if (protocol::isAckReq(hdr) && !node::isBroadcast(hdr.to) && ackEligible && decLen >= MSG_ID_LEN) {
            if (!x25519_keys::hasKeyFor(hdr.from)) {
              RIFTLINK_DIAG("ACK", "event=ACK_SUPPRESS reason=no_pairwise_key from=%02X%02X", hdr.from[0], hdr.from[1]);
            } else {
              uint8_t ackPlain[MSG_ID_LEN];
              memcpy(ackPlain, decBuf, MSG_ID_LEN);
              uint8_t ackEnc[MSG_ID_LEN + crypto::OVERHEAD];
              size_t ackEncLen = sizeof(ackEnc);
              if (crypto::encryptFor(hdr.from, ackPlain, MSG_ID_LEN, ackEnc, &ackEncLen)) {
                uint8_t ackPkt[protocol::PAYLOAD_OFFSET + MSG_ID_LEN + crypto::OVERHEAD + 16];
                size_t an = protocol::buildPacket(ackPkt, sizeof(ackPkt), node::getId(), hdr.from, 31,
                    protocol::OP_ACK, ackEnc, ackEncLen, true, false, false, protocol::CHANNEL_DEFAULT, 0);
                if (an > 0) (void)radio::send(ackPkt, an);
              } else {
                RIFTLINK_DIAG("ACK", "event=ACK_TX_FAIL reason=encrypt_fail from=%02X%02X", hdr.from[0], hdr.from[1]);
              }
            }
          }
        }
      }
      break;
    }

    case protocol::OP_GROUP_MSG: {
      const bool forUsGm = node::isForMe(hdr.to) || node::isBroadcast(hdr.to);
      if (forUsGm && payloadLen > 0 && protocol::isEncrypted(hdr) && !node::isForMe(hdr.from)) {
        uint8_t decBuf[256];
        uint8_t tmpBuf[256];
        size_t decLen = sizeof(decBuf);
        if (crypto::decrypt(payload, payloadLen, decBuf, &decLen) && decLen >= GROUP_ID_LEN) {
          if (protocol::isCompressed(hdr)) {
            size_t d = compress::decompress(decBuf, decLen, tmpBuf, sizeof(tmpBuf));
            if (d == 0 || d < GROUP_ID_LEN) {
              decLen = 0;
            } else {
              memcpy(decBuf, tmpBuf, d);
              decLen = d;
            }
          }
          if (decLen >= GROUP_ID_LEN) {
            uint32_t groupId = 0;
            memcpy(&groupId, decBuf, GROUP_ID_LEN);
            if (groupId == GROUP_ALL) {
              uint32_t appMsgId = 0;
              const char* msg = nullptr;
              size_t msgLen = 0;
              bool skipNotify = false;
              if (decLen >= GROUP_ID_LEN + MSG_ID_LEN) {
                memcpy(&appMsgId, decBuf + GROUP_ID_LEN, MSG_ID_LEN);
                if (bcDedupSeen(hdr.from, appMsgId)) {
                  RIFTLINK_DIAG("MSG", "event=GROUP_BCAST_DROP reason=dedup from=%02X%02X msgId=%u", hdr.from[0],
                      hdr.from[1], (unsigned)appMsgId);
                  skipNotify = true;
                } else {
                  bcDedupAdd(hdr.from, appMsgId);
                  msg = (const char*)(decBuf + GROUP_ID_LEN + MSG_ID_LEN);
                  msgLen = decLen - GROUP_ID_LEN - MSG_ID_LEN;
                }
              } else {
                msg = (const char*)(decBuf + GROUP_ID_LEN);
                msgLen = decLen - GROUP_ID_LEN;
              }
              if (!skipNotify && msgLen < 256) {
                char text[256];
                memcpy(text, msg, msgLen);
                text[msgLen] = '\0';
                ble::notifyMsg(hdr.from, text, appMsgId, rssi, 0);
              }
            }
          }
        }
      }
      break;
    }

    case protocol::OP_ACK:
      if (!node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) {
        RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=bad_direction from=%02X%02X", hdr.from[0], hdr.from[1]);
        break;
      }
      if (!protocol::isEncrypted(hdr)) {
        RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=strict_unencrypted_reject from=%02X%02X", hdr.from[0],
            hdr.from[1]);
        break;
      }
      if (payloadLen > 0) {
        uint8_t ackPlain[32];
        size_t ackPlainLen = sizeof(ackPlain);
        if (!crypto::decryptFrom(hdr.from, payload, payloadLen, ackPlain, &ackPlainLen) || ackPlainLen != MSG_ID_LEN) {
          RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=decrypt_or_len from=%02X%02X len=%u", hdr.from[0], hdr.from[1],
              (unsigned)payloadLen);
          bool hasKeyNow = x25519_keys::hasKeyFor(hdr.from);
          x25519_keys::sendKeyExchange(hdr.from, true, hasKeyNow, hasKeyNow ? "decrypt_fail_ack" : "ack_no_key");
          break;
        }
        uint32_t msgId = 0;
        memcpy(&msgId, ackPlain, MSG_ID_LEN);
        if (msg_queue::onAckReceived(hdr.from, ackPlain, ackPlainLen, false, true, true)) {
          ble::notifyDelivered(hdr.from, msgId, rssi);
        } else if (voice_frag::matchAck(hdr.from, msgId)) {
          ble::notifyDelivered(hdr.from, msgId, rssi);
        }
      }
      break;

    case protocol::OP_ACK_BATCH:
      if (!node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) {
        break;
      }
      if (!protocol::isEncrypted(hdr)) {
        break;
      }
      if (payloadLen > 0) {
        uint8_t batchPlain[64];
        size_t batchPlainLen = sizeof(batchPlain);
        if (crypto::decryptFrom(hdr.from, payload, payloadLen, batchPlain, &batchPlainLen)) {
          msg_queue::onAckBatchReceived(hdr.from, batchPlain, batchPlainLen, rssi, ble::notifyDelivered);
        }
      }
      break;

    case protocol::OP_ACK_SELECTIVE:
      if (!node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) break;
      if (!protocol::isEncrypted(hdr) || payloadLen == 0) break;
      {
        uint8_t selPlain[64];
        size_t selPlainLen = sizeof(selPlain);
        if (!crypto::decryptFrom(hdr.from, payload, payloadLen, selPlain, &selPlainLen) || selPlainLen < 5) break;
        if (selPlain[0] == 1) {
          uint16_t batchPktId = (uint16_t)selPlain[1] | ((uint16_t)selPlain[2] << 8);
          uint16_t ackBitmap = (uint16_t)selPlain[3] | ((uint16_t)selPlain[4] << 8);
          msg_queue::onSelectiveAckReceived(hdr.from, batchPktId, ackBitmap, rssi, ble::notifyDelivered);
        }
      }
      break;

    case protocol::OP_FRAG_CTRL:
      if (!node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) break;
      if (!protocol::isEncrypted(hdr) || payloadLen == 0) break;
      {
        uint8_t ctrlPlain[64];
        size_t ctrlPlainLen = sizeof(ctrlPlain);
        if (!crypto::decryptFrom(hdr.from, payload, payloadLen, ctrlPlain, &ctrlPlainLen) || ctrlPlainLen < 10) break;
        if (ctrlPlain[0] == 0x01) {
          uint32_t fragMsgId = 0;
          memcpy(&fragMsgId, ctrlPlain + 1, 4);
          uint8_t total = ctrlPlain[5];
          uint32_t mask = (uint32_t)ctrlPlain[6] | ((uint32_t)ctrlPlain[7] << 8) | ((uint32_t)ctrlPlain[8] << 16) |
                          ((uint32_t)ctrlPlain[9] << 24);
          frag::onFragCtrl(hdr.from, fragMsgId, total, mask);
        }
      }
      break;

    case protocol::OP_READ:
      if (node::isForMe(hdr.to) && payloadLen >= 4) {
        uint32_t msgId;
        memcpy(&msgId, payload, 4);
        ble::notifyRead(hdr.from, msgId, rssi);
      }
      break;

    case protocol::OP_PING:
      if (node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) {
        uint8_t pongPkt[protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID];
        size_t pongLen = protocol::buildPacket(pongPkt, sizeof(pongPkt), node::getId(), hdr.from, 31, protocol::OP_PONG,
            nullptr, 0, false, false, false, protocol::CHANNEL_DEFAULT, hdr.pktId);
        if (pongLen > 0) (void)radio::send(pongPkt, pongLen);
      }
      break;

    case protocol::OP_PONG:
      ble::clearPingRetryForPeer(hdr.from);
      ble::notifyPong(hdr.from, rssi, hdr.pktId);
      break;

    case protocol::OP_POLL:
      msg_queue::onPollReceived(hdr.from);
      break;

    case protocol::OP_TELEMETRY:
      if (payloadLen > 0 && protocol::isEncrypted(hdr)) {
        uint8_t tbuf[64];
        size_t tlen = 0;
        if (crypto::decrypt(payload, payloadLen, tbuf, &tlen) && tlen >= 4) {
          uint16_t batteryMv = 0;
          uint16_t heapKb = 0;
          memcpy(&batteryMv, tbuf, 2);
          memcpy(&heapKb, tbuf + 2, 2);
          neighbors::updateBattery(hdr.from, batteryMv);
          if (node::isForMe(hdr.from)) {
            ble::notifyTelemetry(hdr.from, batteryMv, heapKb, rssi);
          }
        }
      }
      break;

    case protocol::OP_LOCATION:
      if (payloadLen > 0 && protocol::isEncrypted(hdr)) {
        uint8_t decBuf[32];
        size_t decLen = 0;
        if (crypto::decrypt(payload, payloadLen, decBuf, &decLen) && decLen >= 10) {
          int32_t lat7, lon7;
          int16_t alt;
          memcpy(&lat7, decBuf, 4);
          memcpy(&lon7, decBuf + 4, 4);
          memcpy(&alt, decBuf + 8, 2);
          float lat = float(lat7) / 1e7f;
          float lon = float(lon7) / 1e7f;
          if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) break;
          if (decLen >= 16) {
            uint16_t radiusM = 0;
            uint32_t expiryEpochSec = 0;
            memcpy(&radiusM, decBuf + 10, 2);
            memcpy(&expiryEpochSec, decBuf + 12, 4);
            if (radiusM > GEOFENCE_RADIUS_MAX_M) break;
            if (expiryEpochSec > 0 && expiryEpochSec < 1700000000UL) break;
          }
          /* FakeTech: relay mesh — evt:location от соседей (GNSS на плате нет). В ESP main.cpp для BLE —
           * другой фильтр по from; здесь peer-only. */
          if (!node::isForMe(hdr.from) && !node::isInvalidNodeId(hdr.from)) {
            ble::notifyLocation(hdr.from, lat, lon, alt, rssi);
          }
        }
      }
      break;

    case protocol::OP_MSG_BATCH:
      if (payloadLen >= 1 && node::isForMe(hdr.to) && protocol::isEncrypted(hdr)) {
        uint8_t count = payload[0];
        uint8_t decBuf[256];
        uint8_t tmpBuf[256];
        size_t off = 1;
        if (count >= 1 && count <= 8) {
          for (uint8_t i = 0; i < count && off + 2 <= payloadLen; i++) {
            uint16_t encLen = (uint16_t)payload[off] | ((uint16_t)payload[off + 1] << 8);
            off += 2;
            if (encLen == 0 || off + encLen > payloadLen) break;
            const uint8_t* encPtr = payload + off;
            off += encLen;
            size_t decLen = sizeof(decBuf);
            if (!crypto::decryptFrom(hdr.from, encPtr, encLen, decBuf, &decLen) || decLen >= 256) continue;
            size_t d = compress::decompress(decBuf, decLen, tmpBuf, sizeof(tmpBuf));
            if (d > 0 && d < 256) {
              memcpy(decBuf, tmpBuf, d);
              decLen = d;
            }
            if (decLen >= MSG_ID_LEN + 1) {
              size_t msgOff = 0;
              uint32_t msgId = 0;
              memcpy(&msgId, decBuf + msgOff, MSG_ID_LEN);
              msgOff += MSG_ID_LEN;
              uint8_t flags = decBuf[msgOff++];
              if ((flags & 0x01) != 0 && decLen >= msgOff + 1) {
                uint8_t piggyCount = decBuf[msgOff++];
                for (uint8_t pi = 0; pi < piggyCount && decLen >= msgOff + MSG_ID_LEN; pi++) {
                  uint32_t piggyMsgId = 0;
                  memcpy(&piggyMsgId, decBuf + msgOff, MSG_ID_LEN);
                  msgOff += MSG_ID_LEN;
                  uint8_t singlePayload[MSG_ID_LEN];
                  memcpy(singlePayload, &piggyMsgId, MSG_ID_LEN);
                  if (msg_queue::onAckReceived(hdr.from, singlePayload, MSG_ID_LEN, false, true, true)) {
                    ble::notifyDelivered(hdr.from, piggyMsgId, rssi);
                  }
                }
              }
              size_t msgLen = decLen - msgOff;
              if (msgLen < sizeof(s_msgBatchText)) {
                memcpy(s_msgBatchText, decBuf + msgOff, msgLen);
                s_msgBatchText[msgLen] = '\0';
                ble::notifyMsg(hdr.from, s_msgBatchText, msgId, rssi, 0);
              }
            }
          }
        }
      }
      break;

    case protocol::OP_MSG_FRAG:
      if (payloadLen >= frag::FRAG_HEADER_LEN && protocol::isEncrypted(hdr)) {
        size_t outLen = 0;
        uint32_t fragMsgId = 0;
        if (frag::onFragment(hdr.from, hdr.to, payload, payloadLen, protocol::isCompressed(hdr), s_fragOutBuf,
                sizeof(s_fragOutBuf), &outLen, &fragMsgId) &&
            outLen > 0 && outLen < sizeof(s_fragOutBuf)) {
          s_fragOutBuf[outLen] = '\0';
          ble::notifyMsg(hdr.from, (const char*)s_fragOutBuf, fragMsgId, rssi, 0);
        }
      }
      break;

    case protocol::OP_VOICE_MSG:
    case protocol::OP_SOS:
    case protocol::OP_PARITY:
      if (!protocol::isEncrypted(hdr)) {
        RIFTLINK_DIAG("PARSE", "event=RX_DROP_FAST reason=missing_encrypted_flag op=0x%02X from=%02X%02X",
            (unsigned)hdr.opcode, hdr.from[0], hdr.from[1]);
        break;
      }
      break;

    case protocol::OP_ECHO:
      break;

    case protocol::OP_XOR_RELAY:
      break;

    case protocol::OP_ROUTE_REQ:
      routing::onRouteReq(hdr.from, payload, payloadLen);
      break;

    case protocol::OP_ROUTE_REPLY:
      if (routing::onRouteReply(hdr.from, hdr.to, payload, payloadLen) && node::isForMe(hdr.to)) {
        ble::notifyRoutes(0);
      }
      break;

    case protocol::OP_SF_BEACON:
      break;

    case protocol::OP_NACK:
      if (payloadLen >= 2 && node::isForMe(hdr.to) && neighbors::isOnline(hdr.from)) {
        uint16_t pktId = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
        (void)pkt_cache::retransmitOnNack(hdr.from, pktId);
      }
      break;

    default:
      break;
  }
}

/** Как main.cpp (Heltec): телефон → шифрованный broadcast OP_LOCATION (на плате GNSS нет). */
static void sendLocation(float lat, float lon, int16_t alt, uint16_t radiusM = 0, uint32_t expiryEpochSec = 0) {
  int32_t lat7 = (int32_t)(lat * 1e7f);
  int32_t lon7 = (int32_t)(lon * 1e7f);
  uint8_t plain[16];
  memcpy(plain, &lat7, 4);
  memcpy(plain + 4, &lon7, 4);
  memcpy(plain + 8, &alt, 2);
  memcpy(plain + 10, &radiusM, 2);
  memcpy(plain + 12, &expiryEpochSec, 4);

  uint8_t encBuf[protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t encLen = sizeof(encBuf);
  if (!crypto::encrypt(plain, sizeof(plain), encBuf, &encLen)) {
#ifndef RIFTLINK_SKIP_BLE
    ble::notifyError("location_encrypt", "Шифрование локации не удалось");
#endif
    return;
  }

  uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t plen = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_LOCATION,
      encBuf, encLen, true, false, false);
  if (plen > 0) {
    uint8_t txSf = neighbors::rssiToSf(neighbors::getMinRssi());
    char reasonBuf[40];
    if (!queueTxPacket(pkt, plen, txSf, false, TxRequestClass::data, reasonBuf, sizeof(reasonBuf))) {
      queueDeferredSend(pkt, plen, txSf, 120, true);
      RIFTLINK_DIAG("GEO", "event=LOCATION_TX_DEFER cause=%s", reasonBuf[0] ? reasonBuf : "?");
    }
  }
}

static void onBleSend(const uint8_t* to, const char* text, uint8_t ttlMinutes) {
  if (!msg_queue::enqueue(to, text, ttlMinutes, false, msg_queue::TRIGGER_NONE, 0)) {
    RIFTLINK_DIAG("SEND", "event=enqueue_fail reason=%u", (unsigned)msg_queue::getLastSendFailReason());
  }
}

void setup() {
  Serial.begin(115200);
#if defined(ARDUINO_ARCH_NRF52)
  /* Самый ранний маркер: при полной тишине UART зависание до setup() / Serial / USB CDC; иначе смотрите последнюю строку ниже. */
  Serial.println("[RiftLink] setup entry");
  Serial.flush();
  delay(80);
  yield();
  Serial.println();
  Serial.println("[RiftLink] USB CDC ok");
  Serial.flush();
  RIFTLINK_DIAG("BOOT", "event=USB_CDC ok=1 board=faketec");
#if defined(NRF_POWER)
  /* После «Disconnected / Reconnect» с падением t= и rx_pkt=0 — сравните raw с предыдущей сессией (DOG/LOCKUP vs VBUS/PIN). */
  {
    uint32_t rr = NRF_POWER->RESETREAS;
    RIFTLINK_DIAG("BOOT", "event=RESET_REASON raw=0x%08lx note=see_nrf52840_POWER_RESETREAS_DOG_LOCKUP_VBUS",
        (unsigned long)rr);
    NRF_POWER->RESETREAS = rr;
  }
#endif
  yield();
  delay(300);
  for (int i = 0; i < 3; i++) {
    Serial.println("[RiftLink] FakeTech V5");
    yield();
    delay(500);
  }
  RIFTLINK_DIAG("BOOT", "event=PRODUCT name=FakeTech_V5 board=faketec");
#else
  Serial.println("[RiftLink] FakeTech V5");
  RIFTLINK_DIAG("BOOT", "event=PRODUCT name=FakeTech_V5 board=faketec");
  yield();
#endif

  RIFTLINK_DIAG("BOOT", "event=INIT stage=storage");
  Serial.println("[RiftLink] faketec setup: storage::init");
  Serial.flush();
  yield();
  storage::init();
#if !defined(RIFTLINK_SKIP_INTERNALFS)
  RIFTLINK_DIAG("BOOT", "event=INIT stage=internalfs_mount note=first_erase_may_slow");
  Serial.println("[RiftLink] faketec setup: mountInternalFs (может долго при первом erase)");
  Serial.flush();
  yield();
  storage::mountInternalFs();
#endif
  RIFTLINK_DIAG("BOOT", "event=INIT stage=node");
  Serial.println("[RiftLink] faketec setup: node::init");
  yield();
  node::init();
  RIFTLINK_DIAG("BOOT", "event=INIT stage=region_crypto_x25519_neighbors");
  Serial.println("[RiftLink] faketec setup: region/crypto/x25519/neighbors");
  yield();
  region::init();
  crypto::init();
  x25519_keys::init();
  neighbors::init();

  RIFTLINK_DIAG("BOOT", "event=INIT stage=radio");
  Serial.println("[RiftLink] faketec setup: radio::init (SX1262 / SPI — при зависании смотрите сюда)");
  Serial.flush();
  yield();
  if (!radio::init()) {
    RIFTLINK_DIAG("RADIO", "event=INIT ok=0 cause=radio_fail heap=%u", (unsigned)heap_metrics_free_bytes());
    yield();
    for (;;) {
      delay(1000);
      yield();
    }
  }
#ifndef RIFTLINK_SKIP_BLE
  radio::setRxBlePoll(pollBleForRadio);
#else
  radio::setRxBlePoll(nullptr);
#endif

  RIFTLINK_DIAG("BOOT", "event=INIT stage=display");
  Serial.println("[RiftLink] faketec setup: display::init (I2C OLED)");
  Serial.flush();
  yield();
  display::init();
  Serial.printf("[RiftLink] Heap after radio+display: free=%u total=%u min=%u\n",
      (unsigned)heap_metrics_free_bytes(), (unsigned)heap_metrics_total_bytes(),
      (unsigned)heap_metrics_min_free_ever_bytes());

  ble::setOnSend(onBleSend);
  ble::setOnLocation(sendLocation);
#if defined(RIFTLINK_SKIP_BLE)
  RIFTLINK_DIAG("BLE", "event=INIT skip=1 reason=RIFTLINK_SKIP_BLE");
  Serial.println("[RiftLink] faketec setup: ble::init (stub, RIFTLINK_SKIP_BLE)");
  yield();
  ble::init();
#else
  /* BLE запускаем после delay(300) в конце setup — без отложенного старта в loop (иначе ~8 с без NUS/рекламы). */
#endif

  if (display::isPresent()) {
    display::clear();
    display::setCursor(0, 0);
    display::print("RiftLink FT");
    display::setCursor(0, 1);
    const uint8_t* id = node::getId();
    char buf[20];
    sprintf(buf, "%02X%02X...", id[0], id[1]);
    display::print(buf);
    display::show();
  }

  nextHelloDue = millis() + HELLO_FIRST_MS;
  Serial.println("[RiftLink] Ready");
  RIFTLINK_DIAG("BOOT", "event=READY ok=1 hello_first_ms=%lu hello_interval_ms=%lu heap=%u heap_total=%u",
      (unsigned long)HELLO_FIRST_MS, (unsigned long)HELLO_INTERVAL_MS,
      (unsigned)heap_metrics_free_bytes(), (unsigned)heap_metrics_total_bytes());
  yield();
#if defined(ARDUINO_ARCH_NRF52)
  delay(300);
  yield();
#endif
#ifndef RIFTLINK_SKIP_BLE
  RIFTLINK_DIAG("BLE", "event=INIT stage=setup_after_usb");
  Serial.println("[RiftLink] faketec setup: ble::init (SoftDevice/NUS — может долго)");
  Serial.flush();
  yield();
  ble::init();
  Serial.printf("[RiftLink] Heap after ble::init: free=%u min=%u\n",
      (unsigned)heap_metrics_free_bytes(), (unsigned)heap_metrics_min_free_ever_bytes());
  yield();
#endif

  randomSeed((uint32_t)micros() ^ (uint32_t)node::getId()[0]);
  Serial.println("[RiftLink] faketec setup: msg_queue / mesh modules");
  msg_queue::init();
  ack_coalesce::init();
  mab::init();
  collision_slots::init();
  beacon_sync::init();
  clock_drift::init();
  packet_fusion::init();
  packet_fusion::setOnBatchSent(
      [](const uint8_t* to, const uint32_t* msgIds, int count, uint16_t batchPktId) {
        msg_queue::registerBatchSent(to, msgIds, count, batchPktId);
      });
  packet_fusion::setOnSingleFlush([](const uint8_t* to, uint32_t msgId, const uint8_t* pkt, size_t pktLen, uint8_t txSf) {
    return msg_queue::registerPendingFromFusion(to, msgId, pkt, pktLen, txSf);
  });
  network_coding::init();
  msg_queue::setOnUnicastSent([](const uint8_t* to, uint32_t msgId) { ble::notifySent(to, msgId); });
  msg_queue::setOnUnicastUndelivered([](const uint8_t* to, uint32_t msgId) {
    radio::notifyCongestion();
    ble::notifyUndelivered(to, msgId);
  });
  msg_queue::setOnBroadcastSent([](uint32_t msgId) { ble::notifySent(protocol::BROADCAST_ID, msgId); });
  msg_queue::setOnBroadcastDelivery(
      [](uint32_t msgId, int delivered, int total) { ble::notifyBroadcastDelivery(msgId, delivered, total); });
  msg_queue::setOnTimeCapsuleReleased([](const uint8_t* to, uint32_t msgId, uint8_t triggerType) {
    ble::notifyTimeCapsuleReleased(to, msgId, triggerType);
  });
  frag::init();
  pkt_cache::init();
  groups::init();
  offline_queue::init();
  routing::init();
  telemetry_nrf::init();
  /* Без RIFTLINK_DIAG: гарантированная строка после полного setup (при позднем подключении к COM виден только хвост лога). */
  Serial.println("[RiftLink] Setup complete");
#if FAKETEC_NRF_WDT_ENABLED
  faketec_wdt_begin(FAKETEC_WDT_TIMEOUT_MS);
  RIFTLINK_DIAG("RUN", "event=WDT_ARMED timeout_ms=%u note=feed_each_loop_reset_on_hang", (unsigned)FAKETEC_WDT_TIMEOUT_MS);
#endif
  yield();
}

void loop() {
  uint32_t now = millis();

#ifndef RIFTLINK_SKIP_BLE
  /* BLE NUS до долгого radio::receive(): иначе команды с телефона не читаются, пока LoRa блокирует loop. */
  ble::update();
#endif

  asyncTxPoll();
  msg_queue::update();
  routing::update();
  telemetry_nrf::tick();

  int n = radio::receive(rxBuf, sizeof(rxBuf));
  /* receive() блокирует на ~5*TOA(кадр); millis() нужно обновить до HELLO/HEARTBEAT иначе тайминги уезжают. */
  now = millis();

  if (n > 0) {
    handlePacket(rxBuf, n);
    yield();
  }
  {
    const uint32_t rxRecSeq = radio::getRxRecoverySeq();
    if (rxRecSeq != s_mainLastRxRecoverySeq) {
      s_mainLastRxRecoverySeq = rxRecSeq;
      RIFTLINK_DIAG("TRACE",
          "main_after_rx_recovery seq=%lu last_recovery_ms=%lu note=watch_KEY_RX_RAW_KEY_STORE_OK_HELLO_RX",
          (unsigned long)rxRecSeq, (unsigned long)radio::getLastRxRecoveryMs());
    }
  }
#ifndef RIFTLINK_SKIP_BLE
  if (s_bleNeighborsNotifyPending) {
    s_bleNeighborsNotifyPending = false;
    RIFTLINK_DIAG("TRACE", "main_neighbor_evt_step=1_before_ble_update");
    ble::update();
    RIFTLINK_DIAG("TRACE", "main_neighbor_evt_step=2_before_notifyNeighbors");
    ble::notifyNeighbors(0);
    RIFTLINK_DIAG("TRACE", "main_neighbor_evt_step=3_after_notifyNeighbors");
  }
#endif

  static uint32_t lastSerialPing = 0;
  if (now - lastSerialPing >= 60000) {
    lastSerialPing = now;
    s_heartbeatEmittedThisLoop = true;
    const radio::RxDiagStats rx = radio::getRxDiagStats();
    RIFTLINK_DIAG("RUN",
        "event=HEARTBEAT ok=1 interval_ms=60000 heap=%u heap_min=%u rx_tmo=%lu rx_pkt=%lu rx_err=%lu rx_crc=%lu",
        (unsigned)heap_metrics_free_bytes(), (unsigned)heap_metrics_min_free_ever_bytes(),
        (unsigned long)rx.rxTimeouts, (unsigned long)rx.rxPackets, (unsigned long)rx.rxRadioErrors,
        (unsigned long)rx.rxCrcMismatch);
    /* Без Serial.flush(): на USB CDC flush может блокировать навсегда. Запись в Serial см. log.h (RIFTLINK_DIAG_USB_THROTTLE). */
    /* KEY_HINT см. конец loop — после offline_queue и BLE-хвоста, чтобы не казалось что «подвисает на строке KEY_HINT». */
  }

  {
    uint32_t keyTxMs = x25519_keys::getLastKeyTxReadyMs();
    if (keyTxMs != 0 && keyTxMs != s_lastObservedKeyTxMs) {
      s_lastObservedKeyTxMs = keyTxMs;
      uint32_t until = keyTxMs + HANDSHAKE_TRAFFIC_QUIET_MS;
      if ((int32_t)(until - s_handshakeQuietUntilMs) > 0) {
        s_handshakeQuietUntilMs = until;
        RIFTLINK_DIAG("HELLO", "event=HANDSHAKE_QUIET cause=key_tx quiet_ms=%lu",
            (unsigned long)(s_handshakeQuietUntilMs - now));
      }
    }
  }

  if ((int32_t)(now - nextHelloDue) >= 0) {
    if (isHandshakeQuietActive()) {
      nextHelloDue = s_handshakeQuietUntilMs + 1;
    } else {
      uint32_t lastKeyTxMs = x25519_keys::getLastKeyTxReadyMs();
      if (lastKeyTxMs != 0 && (now - lastKeyTxMs) < HELLO_QUIET_AFTER_KEY_MS) {
        nextHelloDue = lastKeyTxMs + HELLO_QUIET_AFTER_KEY_MS;
      } else if (s_lastHelloAirMs != 0 && (now - s_lastHelloAirMs) < HELLO_HARD_MIN_INTERVAL_MS) {
        nextHelloDue = s_lastHelloAirMs + HELLO_HARD_MIN_INTERVAL_MS;
      } else {
        int32_t jitter = (int32_t)random(-(int32_t)HELLO_JITTER_MS, (int32_t)HELLO_JITTER_MS + 1);
        uint32_t interval = (uint32_t)((int32_t)helloIntervalMsForNetwork() + jitter);
        nextHelloDue = now + interval;
        (void)sendHello();
      }
    }
  }

  /* Перед долгой записью InternalFS — опросить Serial/NUS/SoftDevice. */
  ble::update();
  /* Offline queue: большой setLargeBlob после HELLO — не блокировать начало цикла (receive/BLE). */
  offline_queue::update();

  ble::update();
  delay(5);
  yield();

  if (s_heartbeatEmittedThisLoop) {
    s_heartbeatEmittedThisLoop = false;
    const uint32_t tEnd = millis();
    RIFTLINK_DIAG("TRACE", "stage=heartbeat_tail_before_key_hint");
    if (tEnd - s_lastKeyPairingHintMs >= 120000) {
      bool anyNeighborNoKey = false;
      for (int i = 0; i < NEIGHBORS_MAX; i++) {
        uint8_t nid[protocol::NODE_ID_LEN];
        if (!neighbors::getId(i, nid)) continue;
        if (!x25519_keys::hasKeyFor(nid)) {
          anyNeighborNoKey = true;
          break;
        }
      }
      if (anyNeighborNoKey) {
        s_lastKeyPairingHintMs = tEnd;
        RIFTLINK_DIAG("KEY",
            "event=KEY_HINT neighbor_without_pairwise=1 note=KEY_EXCHANGE_unicast_direct_LoRa_V3V4_mesh_does_not_relay_KEY");
      }
    }
    RIFTLINK_DIAG("TRACE", "stage=loop_iteration_end");
  }

  /* Редкий маркер живости: по monotonic t= сопоставлять с логом Heltec; при фризе строки не будет. */
  static uint32_t s_lastAliveMs = 0;
  {
    const uint32_t tAlive = millis();
    if ((uint32_t)(tAlive - s_lastAliveMs) >= 30000u) {
      s_lastAliveMs = tAlive;
      RIFTLINK_DIAG("RUN", "event=ALIVE t=%lu heap=%u note=use_ms_for_dual_serial_correlation",
          (unsigned long)tAlive, (unsigned)heap_metrics_free_bytes());
    }
  }
#if FAKETEC_NRF_WDT_ENABLED
  faketec_wdt_feed();
#endif
}
