/**
 * RiftLink (RL) Firmware — Фаза 1
 * Heltec WiFi LoRa 32 (V3/V4): ESP32-S3, SX1262, OLED, BLE
 *
 * План: docs/CUSTOM_PROTOCOL_PLAN.md
 */

#include <Arduino.h>
#include <esp_random.h>

// Fallback: loopTask stack — build_flags -DARDUINO_LOOP_STACK_SIZE может не применяться к framework
#if defined(ESP_LOOP_TASK_STACK_SIZE)
ESP_LOOP_TASK_STACK_SIZE(32768);
#elif defined(SET_LOOP_TASK_STACK_SIZE)
SET_LOOP_TASK_STACK_SIZE(32768);
#endif
#include <nvs_flash.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_event.h>

#include "radio/radio.h"
#include "protocol/packet.h"
#include "node/node.h"
#include "ui/display.h"
#include "crypto/crypto.h"
#include "ble/ble.h"
#include "msg_queue/msg_queue.h"
#include "mab/mab.h"
#include "compress/compress.h"
#include "frag/frag.h"
#include "telemetry/telemetry.h"
#include "ota/ota.h"
#include "region/region.h"
#include "collision_slots/collision_slots.h"
#include "beacon_sync/beacon_sync.h"
#include "clock_drift/clock_drift.h"
#include "bls_n/bls_n.h"
#include "esp_now_slots/esp_now_slots.h"
#include "packet_fusion/packet_fusion.h"
#include "network_coding/network_coding.h"
#include "neighbors/neighbors.h"
#include "groups/groups.h"
#include "offline_queue/offline_queue.h"
#include "routing/routing.h"
#include "x25519_keys/x25519_keys.h"
#include "wifi/wifi.h"
#include "locale/locale.h"
#include "voice_frag/voice_frag.h"
#include "gps/gps.h"
#include "selftest/selftest.h"
#include "powersave/powersave.h"
#include "async_queues.h"
#include "async_tasks.h"
#include "led/led.h"
#include "duty_cycle/duty_cycle.h"
#include "pkt_cache/pkt_cache.h"
#include "log.h"

#define BUTTON_PIN         0    // Heltec USER_SW
#define MIN_PRESS_MS       80
#if defined(USE_EINK)
#define LONG_PRESS_MS      900  // e-ink: 500ms мало — пользователь ждёт отрисовку, часто ложно long
#else
#define LONG_PRESS_MS      500
#endif
#define POST_PRESS_DEBOUNCE_MS 400  // игнор дребезга после обработки — против двойного long press
#define PENDING_REDRAW_RETRY_MS 2500  // retry смены вкладки, если дисплей не принял за 2.5с

#define HELLO_INTERVAL_MS  30000  // 30с — спокойный режим (Meshtastic: position 15мин, telemetry 30мин)
#define HELLO_INTERVAL_AGGRESSIVE_MS 18000  // 18с — discovery при 0 соседях (меньше storm, фазирование по слоту)
#define HELLO_JITTER_MS    3000   // ±3с — при 1+ соседях
#define HELLO_JITTER_ZERO_MS 1000 // ±1с при 0 соседях (фазирование по nodeId даёт основной spread)
#define TELEM_INTERVAL_MS  60000
#define GPS_LOC_INTERVAL_MS 60000  // интервал отправки локации с GPS
#define SF_ADAPT_INTERVAL_MS 30000 // адаптивный SF по качеству связи
#define POLL_INTERVAL_MS   8000    // RIT: «присылайте пакеты для меня» каждые 8с

static uint32_t lastHello = 0;
static uint32_t lastPoll = 0;
static uint32_t lastTelemetry = 0;
static uint32_t lastGpsLoc = 0;
static uint32_t lastSfAdapt = 0;
static uint32_t s_helloCounter = 0;   // для SF12 HELLO: каждый N-й на SF12
static uint32_t s_rxCycleCounter = 0; // для SF12 RX: каждый N-й цикл слушать SF12
static uint32_t s_bootTime = 0;       // millis() при старте — агрессивный discovery первые 2 мин
static bool s_nextRxSf12 = false;     // слушать SF12 после drain HELLO на SF12
static uint8_t s_sf12BurstCount = 0;  // 2 цикла SF12 подряд каждые 30с
static uint32_t s_lastSf12Burst = 0;
static uint32_t s_lastKeyRetry = 0;   // retry KEY_EXCHANGE каждые 30с для соседей без ключа
static uint32_t s_loopCooldownUntil = 0;  // вместо delay(10) — throttle OTA и конец loop
static uint8_t s_bootPhase = 0;           // 0=ожидание 4с, 1=готов
static uint32_t s_bootPhaseStart = 0;
static uint8_t rxBuf[protocol::SYNC_LEN + protocol::HEADER_LEN + protocol::MAX_PAYLOAD + crypto::OVERHEAD];

#define BOOT_PHASE_WAIT_4S  0
#define BOOT_PHASE_DONE     1

#define BC_DEDUP_SIZE 32
struct BcDedupEntry { uint8_t from[2]; uint32_t msgId; };
static BcDedupEntry s_bcDedup[BC_DEDUP_SIZE];
static uint8_t s_bcDedupIdx = 0;

static bool bcDedupSeen(const uint8_t* from, uint32_t msgId) {
  for (int i = 0; i < BC_DEDUP_SIZE; i++) {
    if (s_bcDedup[i].from[0] == from[0] && s_bcDedup[i].from[1] == from[1] && s_bcDedup[i].msgId == msgId)
      return true;
  }
  return false;
}
static void bcDedupAdd(const uint8_t* from, uint32_t msgId) {
  s_bcDedup[s_bcDedupIdx].from[0] = from[0];
  s_bcDedup[s_bcDedupIdx].from[1] = from[1];
  s_bcDedup[s_bcDedupIdx].msgId = msgId;
  s_bcDedupIdx = (s_bcDedupIdx + 1) % BC_DEDUP_SIZE;
}

#define UC_DEDUP_SIZE 32
struct UcDedupEntry { uint8_t from[protocol::NODE_ID_LEN]; uint32_t msgId; };
static UcDedupEntry s_ucDedup[UC_DEDUP_SIZE];

// Relay dedup + rate limit (mesh storm protection)
#define RELAY_DEDUP_SIZE 24
#define RELAY_RATE_MAX 3
#define RELAY_RATE_WINDOW_MS 1000
struct RelayDedupEntry { uint8_t from[protocol::NODE_ID_LEN]; uint32_t payloadHash; };
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
    if (memcmp(s_relayDedup[i].from, from, protocol::NODE_ID_LEN) == 0 && s_relayDedup[i].payloadHash == hash)
      return true;
  }
  return false;
}
static void relayDedupAdd(const uint8_t* from, uint32_t hash) {
  memcpy(s_relayDedup[s_relayDedupIdx].from, from, protocol::NODE_ID_LEN);
  s_relayDedup[s_relayDedupIdx].payloadHash = hash;
  s_relayDedupIdx = (s_relayDedupIdx + 1) % RELAY_DEDUP_SIZE;
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
static uint8_t s_ucDedupIdx = 0;

static bool ucDedupSeen(const uint8_t* from, uint32_t msgId) {
  for (int i = 0; i < UC_DEDUP_SIZE; i++) {
    if (memcmp(s_ucDedup[i].from, from, protocol::NODE_ID_LEN) == 0 && s_ucDedup[i].msgId == msgId)
      return true;
  }
  return false;
}
static void ucDedupAdd(const uint8_t* from, uint32_t msgId) {
  memcpy(s_ucDedup[s_ucDedupIdx].from, from, protocol::NODE_ID_LEN);
  s_ucDedup[s_ucDedupIdx].msgId = msgId;
  s_ucDedupIdx = (s_ucDedupIdx + 1) % UC_DEDUP_SIZE;
}

/** Параметры следующего RX-слота для rxTask. Вызывать из rxTask. */
void getNextRxSlotParams(uint8_t* sfOut, uint32_t* slotMsOut) {
  bool aggressive = (millis() - s_bootTime) < 120000;
  int nNeigh = neighbors::getCount();
  uint8_t baseSf = 7;
#if !defined(SF_FORCE_7)
  if (s_sf12BurstCount == 0 && (millis() - s_lastSf12Burst) >= 30000) {
    s_sf12BurstCount = 2;
  }
  if (nNeigh > 0) {
    int avgRssi = neighbors::getAverageRssi();
    if (avgRssi < -110) baseSf = 12;
    else if (avgRssi < -80) baseSf = 9;
  }
  int minRssi = neighbors::getMinRssi();
  bool needSf10 = (nNeigh > 0 && minRssi != 0 && minRssi < -90);
  int rxMod = aggressive ? 2 : ((nNeigh == 0) ? 3 : (nNeigh >= 6 ? 4 : (nNeigh >= 3 ? 6 : 8)));
  uint8_t sf = 7;
  if (s_sf12BurstCount > 0) {
    sf = 12;
    s_sf12BurstCount--;
    if (s_sf12BurstCount == 0) s_lastSf12Burst = millis();
  } else if (s_nextRxSf12) {
    sf = 12;
    s_nextRxSf12 = false;
  } else {
    int mod = s_rxCycleCounter % rxMod;
    if (mod == 0) sf = 12;
    else if (needSf10 && mod == 1) sf = 10;
    else sf = baseSf;
  }
  s_rxCycleCounter++;
  if (sfOut) *sfOut = sf;
  // slotMs: SF12 ~1.1s для 54B, SF10 ~300ms, SF7 ~50ms. При 0 соседях — 1200ms.
  uint32_t slot = (nNeigh == 0) ? 1200 : ((sf >= 12) ? 1200 : ((sf >= 10) ? 400 : 100));
  if (slotMsOut) *slotMsOut = slot;
#else
  (void)aggressive;
  (void)nNeigh;
  if (sfOut) *sfOut = 7;
  if (slotMsOut) *slotMsOut = 200;  // 200ms — MSG ~80B на SF7 ~50ms, запас на коллизии
#endif
}

// Буферы для handlePacket (packetTask) — s_fragOutBuf/s_voiceOutBuf слишком велики для стека
static uint8_t s_fragOutBuf[frag::MAX_MSG_PLAIN];
static uint8_t s_voiceOutBuf[voice_frag::MAX_VOICE_PLAIN + 1024];

static bool s_lastButton = false;
static uint32_t s_pressStart = 0;
static uint32_t s_lastProcessedMs = 0;
static uint8_t s_pendingScreen = 0xFF;   // ожидаемая вкладка после REDRAW
static uint32_t s_pendingScreenTime = 0;

static void pollButtonAndQueue() {
  if (!displayQueue) return;
  uint32_t now = millis();

  // Retry: если дисплей не принял смену вкладки за 2.5с — отправить повторно
  if (s_pendingScreen != 0xFF) {
    if (displayGetCurrentScreen() == s_pendingScreen) {
      s_pendingScreen = 0xFF;
    } else if ((now - s_pendingScreenTime) >= PENDING_REDRAW_RETRY_MS) {
      queueDisplayRedraw(s_pendingScreen, true);
      s_pendingScreenTime = now;
    }
  }

  bool btn = (digitalRead(BUTTON_PIN) == LOW);  // active low
  if (btn) {
    if (!s_lastButton) { s_lastButton = true; s_pressStart = now; }
  } else if (s_lastButton) {
    uint32_t hold = now - s_pressStart;
    s_lastButton = false;
    if (hold < MIN_PRESS_MS) return;
    if (now - s_lastProcessedMs < POST_PRESS_DEBOUNCE_MS) return;  // дребезг — игнор
    s_lastProcessedMs = now;
    if (displayIsSleeping()) {
      queueDisplayWake();
      return;
    }
    int cur = displayGetCurrentScreen();
    if (hold >= LONG_PRESS_MS) {
      s_pendingScreen = 0xFF;  // long press — не смена вкладки
      queueDisplayLongPress((uint8_t)cur);
    } else {
      uint8_t next = (uint8_t)displayGetNextScreen(cur);
      queueDisplayRedraw(next, true);  // priority — смена вкладки кнопкой в начало очереди
      s_pendingScreen = next;
      s_pendingScreenTime = now;
    }
    queueDisplayLedBlink();
  }
}

void sendHello() {
  uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID,
      31, protocol::OP_HELLO, nullptr, 0);
  if (len == 0) return;

  int n = neighbors::getCount();
  bool manyNeighbors = (n >= 6);
  bool priority = manyNeighbors;  // HELLO в начало очереди при 6+ соседях

#if defined(SF_FORCE_7)
  (void)manyNeighbors;
  radio::send(pkt, len, 7, priority);
#else
  if (n > 0) {
    int minRssi = neighbors::getMinRssi();
    uint8_t txSf = neighbors::rssiToSf(minRssi);
    if (txSf >= 10) {
      uint8_t prev = radio::getSpreadingFactor();
      radio::setSpreadingFactor(txSf);
      uint32_t toa = radio::getTimeOnAir(len);
      radio::setSpreadingFactor(prev);
      if (!duty_cycle::canSend(toa)) txSf = neighbors::rssiToSf(-85);
    }
    if (radio::send(pkt, len, txSf, priority)) {}
    // Двойной HELLO при 6+ соседях: SF12 + SF7 — дальние и близкие слышат
    if (manyNeighbors && txSf != 7) {
      uint8_t prev = radio::getSpreadingFactor();
      radio::setSpreadingFactor(7);
      uint32_t toa7 = radio::getTimeOnAir(len);
      radio::setSpreadingFactor(prev);
      if (duty_cycle::canSend(toa7)) radio::send(pkt, len, 7, priority);
    }
  } else {
    bool aggressiveHello = (millis() - s_bootTime) < 120000;
    int mod = aggressiveHello ? 2 : 3;
    uint8_t txSf = (s_helloCounter % mod) == 0 ? 12 : 7;
    s_helloCounter++;
    if (txSf == 12) {
      uint8_t prev = radio::getSpreadingFactor();
      radio::setSpreadingFactor(12);
      uint32_t toa = radio::getTimeOnAir(len);
      radio::setSpreadingFactor(prev);
      if (!duty_cycle::canSend(toa)) txSf = 7;
    }
    radio::send(pkt, len, txSf, false);
  }
#endif
}

void sendPoll() {
  uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN_BROADCAST];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_POLL, nullptr, 0);
  if (len == 0) return;
  uint8_t txSf = neighbors::getCount() > 0
      ? neighbors::rssiToSf(neighbors::getMinRssi()) : 12;
  if (txSf == 0) txSf = 12;
  radio::send(pkt, len, txSf, false);
}

void sendMsg(const uint8_t* to, const char* text, uint8_t ttlMinutes) {
  if (text == nullptr || strlen(text) == 0) {
    ble::notifyError("send_empty", "Пустое сообщение не отправляется");
    return;
  }
  if (!msg_queue::enqueue(to, text, ttlMinutes)) {
    ble::notifyError("send_failed", "Очередь полна или нет ключа шифрования");
  }
  // broadcast "sent" с msgId — через setOnBroadcastSent (сопоставление с delivery)
}

void sendLocation(float lat, float lon, int16_t alt) {
  // Payload: lat (int32, 1e-7°), lon (int32), alt (int16) = 10 bytes
  int32_t lat7 = (int32_t)(lat * 1e7f);
  int32_t lon7 = (int32_t)(lon * 1e7f);
  uint8_t plain[10];
  memcpy(plain, &lat7, 4);
  memcpy(plain + 4, &lon7, 4);
  memcpy(plain + 8, &alt, 2);

  uint8_t encBuf[protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t encLen = sizeof(encBuf);
  if (!crypto::encrypt(plain, 10, encBuf, &encLen)) {
    RIFTLINK_LOG_ERR("[RiftLink] Location encrypt FAILED\n");
    ble::notifyError("location_encrypt", "Шифрование локации не удалось");
    return;
  }

  uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_LOCATION,
      encBuf, encLen, true, false, false);
  if (len > 0) {
    radio::send(pkt, len, neighbors::rssiToSf(neighbors::getMinRssi()));
  }
}

void handlePacket(const uint8_t* buf, size_t len, int rssi, uint8_t sf) {
  uint8_t decBuf[256];
  uint8_t tmpBuf[256];
  char msgStrBuf[256];
  uint8_t fwdBuf[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];

  // Self-reception: свой KEY_EXCHANGE (обрезанный) — отбросить без лога
  if (buf[0] == protocol::SYNC_BYTE && buf[2] == protocol::OP_KEY_EXCHANGE && !(buf[1] & 0x01)) {
    const uint8_t* fromPtr = (buf[1] & 0xF0) == 0x30 ? (buf + 5) : (buf + 3);
    if (len >= 13 && memcmp(fromPtr, node::getId(), protocol::NODE_ID_LEN) == 0) {
      return;  // наш исходящий KEY_EXCHANGE, принятый по эху/отражению
    }
  }

  protocol::PacketHeader hdr;
  const uint8_t* payload;
  size_t payloadLen;
  if (!protocol::parsePacket(buf, len, &hdr, &payload, &payloadLen)) {
    // Fallback: коллизия/перегрузка — HELLO (13B) + мусор. Пробуем распарсить только первые 13 байт
    if (len >= 13 && buf[0] == protocol::SYNC_BYTE && (buf[1] & 0xF0) == 0x20 &&
        buf[2] == protocol::OP_HELLO && (buf[1] & 0x01)) {  // broadcast
      if (protocol::parsePacket(buf, 13, &hdr, &payload, &payloadLen)) {
        payloadLen = 0;
        payload = nullptr;
        // продолжаем обработку ниже
      } else {
#if defined(DEBUG_PACKET_DUMP)
        Serial.printf("[RiftLink] Parse FAIL len=%u rssi=%d hex=", (unsigned)len, rssi);
        for (size_t i = 0; i < len && i < 32; i++) Serial.printf("%02X", buf[i]);
        Serial.println();
#endif
        return;
      }
    } else if (len == 13 && buf[0] == protocol::SYNC_BYTE && (buf[1] & 0xF0) == 0x20 &&
               buf[2] == protocol::OP_KEY_EXCHANGE && !(buf[1] & 0x01)) {
      // Обрезанный KEY_EXCHANGE (коллизия/half-duplex) — извлечь from, добавить в соседи
      radio::notifyCongestion();  // коллизия → BEB увеличит CW
      uint8_t from[protocol::NODE_ID_LEN];
      memcpy(from, buf + 3, protocol::NODE_ID_LEN);
      if (!node::isBroadcast(from) && !node::isInvalidNodeId(from)) {
        neighbors::onHello(from, rssi);
      }
      return;
    } else if (len >= 13 && buf[0] == protocol::SYNC_BYTE && (buf[1] & 0xF0) == 0x30 &&
               !(buf[1] & 0x01)) {
      // v2.1 с pktId — обрезанный пакет (13 B = sync+ver+opcode+pktId+from), отправить NACK для retransmit
      radio::notifyCongestion();  // коллизия → BEB
      uint16_t pktId = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
      uint8_t from[protocol::NODE_ID_LEN];
      memcpy(from, buf + 5, protocol::NODE_ID_LEN);
      if (!node::isBroadcast(from) && !node::isInvalidNodeId(from) &&
          memcmp(from, node::getId(), protocol::NODE_ID_LEN) != 0) {
        neighbors::onHello(from, rssi);
        if (pktId != 0) {
          uint8_t nackPayload[2] = {(uint8_t)(pktId & 0xFF), (uint8_t)(pktId >> 8)};
          uint8_t nackPkt[protocol::PAYLOAD_OFFSET + 2];
          size_t nackLen = protocol::buildPacket(nackPkt, sizeof(nackPkt),
              node::getId(), from, 31, protocol::OP_NACK, nackPayload, 2);
          if (nackLen > 0) radio::send(nackPkt, nackLen, neighbors::rssiToSf(neighbors::getRssiFor(from)), true);
        }
      }
      return;
    } else {
      // len==13 и opcode MSG/TELEMETRY/GROUP_MSG/KEY_EXCHANGE — обрезанный пакет (коллизия/SF), не спамить
      // broadcast (buf[1]&0x01) MSG/GROUP_MSG/TELEMETRY с len<45 — частично обрезан (min payload 32), не спамить
      // opcode 0x0D (NACK) с len!=23 — скорее всего коррупция KEY_EXCHANGE, не спамить
      // opcode 0x02 (ACK) с len!=25 (unicast) или len!=17 (broadcast) — коллизия/слияние пакетов
      bool truncated = (len == 13 && buf[0] == protocol::SYNC_BYTE && (buf[1] & 0xF0) == 0x20 &&
          (buf[2] == protocol::OP_MSG || buf[2] == protocol::OP_TELEMETRY || buf[2] == protocol::OP_GROUP_MSG ||
           buf[2] == protocol::OP_KEY_EXCHANGE)) ||
          (len < 45 && buf[0] == protocol::SYNC_BYTE && (buf[1] & 0xF0) == 0x20 && (buf[1] & 0x01) &&
          (buf[2] == protocol::OP_MSG || buf[2] == protocol::OP_TELEMETRY || buf[2] == protocol::OP_GROUP_MSG)) ||
          (buf[0] == protocol::SYNC_BYTE && (buf[1] & 0xF0) == 0x20 && buf[2] == protocol::OP_NACK && len != 23) ||
          (buf[0] == protocol::SYNC_BYTE && (buf[1] & 0xF0) == 0x20 && buf[2] == protocol::OP_ACK &&
          len != 25 && len != 17);  // ACK: unicast 25B, broadcast 17B
      if (truncated) {
        radio::notifyCongestion();  // коллизия/обрезанный пакет → BEB
        region::switchChannelOnCongestion();  // Channel Hopping: EU 868.1→868.3→868.5
        collision_slots::recordCollision();  // Predictive Slot Avoidance
      }
#if defined(DEBUG_PACKET_DUMP)
      if (!truncated) {
        Serial.printf("[RiftLink] Parse FAIL len=%u rssi=%d hex=", (unsigned)len, rssi);
        for (size_t i = 0; i < len && i < 32; i++) Serial.printf("%02X", buf[i]);
        Serial.println();
      }
#endif
      return;
    }
  }

#if defined(DEBUG_PACKET_DUMP)
  Serial.printf("[PKT] len=%u rssi=%d sf=%u hex=", (unsigned)len, rssi, sf);
  for (size_t i = 0; i < len && i < 64; i++) Serial.printf("%02X", buf[i]);
  if (len > 64) Serial.print("...");
  Serial.println();
#endif

  // from=broadcast — некорректно, отбрасываем
  if (node::isBroadcast(hdr.from) || node::isInvalidNodeId(hdr.from)) return;

  // OP_XOR_RELAY: попытка декодировать (если есть один из пакетов в кэше)
  if (hdr.opcode == protocol::OP_XOR_RELAY) {
    uint8_t decodedBuf[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
    size_t decodedLen = 0;
    if (network_coding::onXorRelayReceived(buf, len, decodedBuf, &decodedLen) && decodedLen > 0) {
      if (packetQueue && decodedLen <= PACKET_BUF_SIZE) {
        PacketQueueItem item;
        memcpy(item.buf, decodedBuf, decodedLen);
        item.len = (uint16_t)decodedLen;
        item.rssi = (int8_t)rssi;
        item.sf = sf;
        if (xQueueSend(packetQueue, &item, 0) != pdTRUE) {
          handlePacket(decodedBuf, decodedLen, rssi, sf);  // fallback при полной очереди
        }
      } else {
        handlePacket(decodedBuf, decodedLen, rssi, sf);
      }
    }
    return;
  }

  // HELLO всегда broadcast — иначе сдвиг/коррупция (ghost-соседи)
  if (hdr.opcode == protocol::OP_HELLO && !node::isBroadcast(hdr.to)) return;

  // Relay: unicast не для нас, или GROUP_MSG (broadcast) с TTL>0
  // HELLO — всегда broadcast, не ретранслируем (защита от парсинга с перепутанным to)
  // ROUTE_REQ/REPLY обрабатываются модулем routing
  // Broadcast relay только при 2+ соседях — иначе ping-pong между двумя устройствами, HELLO не проходят
  bool needRelay = (hdr.ttl > 0) && (hdr.opcode != protocol::OP_ROUTE_REQ) &&
      (hdr.opcode != protocol::OP_ROUTE_REPLY) && (hdr.opcode != protocol::OP_HELLO) &&
      (hdr.opcode != protocol::OP_NACK) && (
      (!node::isForMe(hdr.to) && !node::isBroadcast(hdr.to)) ||
      ((hdr.opcode == protocol::OP_GROUP_MSG || hdr.opcode == protocol::OP_VOICE_MSG) &&
       neighbors::getCount() >= 2));
  uint32_t relayPayloadHashVal = 0;
  if (needRelay) {
    relayPayloadHashVal = relayPayloadHash(payload ? payload : (const uint8_t*)"", payloadLen);
    if (relayDedupSeen(hdr.from, relayPayloadHashVal)) {
      needRelay = false;
      relayHeard(hdr.from, relayPayloadHashVal);
    }
    if (needRelay && relayRateLimitExceeded()) needRelay = false;
  }
  if (needRelay) {
    relayDedupAdd(hdr.from, relayPayloadHashVal);
    relayRateRecord();
    if (hdr.opcode == protocol::OP_MSG && hdr.pktId != 0 && !node::isBroadcast(hdr.to)) {
      pkt_cache::addOverheard(hdr.from, hdr.to, hdr.pktId, buf, len);
    }

    uint8_t txSf = 0;
    if (node::isBroadcast(hdr.to)) {
      int minRssi = neighbors::getMinRssi();
      txSf = neighbors::rssiToSf(minRssi);
    } else {
      uint8_t nextHop[protocol::NODE_ID_LEN];
      if (routing::getNextHop(hdr.to, nextHop)) {
        int r = neighbors::getRssiFor(nextHop);
        txSf = neighbors::rssiToSf(r);
      }
      if (txSf == 0) txSf = 12;
    }
    int rssiClamp = (rssi < -120) ? -120 : (rssi > -40 ? -40 : rssi);
    int32_t d = 10 * (rssiClamp + 100);
    uint32_t delayMs = (d <= 0) ? 0 : ((d > 500) ? 500 : (uint32_t)d);

    bool xorSent = false;
    if (network_coding::addForXor(buf, len, hdr.from, hdr.to)) {
      uint8_t otherFrom[protocol::NODE_ID_LEN];
      uint32_t otherHash;
      network_coding::getLastPairOther(otherFrom, &otherHash);
      relayHeard(otherFrom, otherHash);
      uint8_t xorBuf[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
      size_t xorLen = 0;
      if (network_coding::getXorPacket(xorBuf, sizeof(xorBuf), &xorLen)) {
        queueDeferredSend(xorBuf, xorLen, txSf, delayMs);
        xorSent = true;
      }
    }
    if (!xorSent) {
      uint8_t decodedBuf[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
      size_t decodedLen = 0;
        if (network_coding::getDecodedFromPending(buf, len, hdr.from, hdr.to, hdr.pktId,
                                                  decodedBuf, &decodedLen) && decodedLen > 0) {
        if (packetQueue && decodedLen <= PACKET_BUF_SIZE) {
          PacketQueueItem item;
          memcpy(item.buf, decodedBuf, decodedLen);
          item.len = (uint16_t)decodedLen;
          item.rssi = (int8_t)rssi;
          item.sf = sf;
          if (xQueueSend(packetQueue, &item, 0) != pdTRUE) {
            handlePacket(decodedBuf, decodedLen, rssi, sf);  // fallback при полной очереди
          }
        } else {
          handlePacket(decodedBuf, decodedLen, rssi, sf);
        }
      }
      memcpy(fwdBuf, buf, len);
      size_t ttlOff;
      if (buf[0] == protocol::SYNC_BYTE && ((buf[1] & 0xF0) == 0x20 || (buf[1] & 0xF0) == 0x30)) {
        bool hasPktId = (buf[1] & 0xF0) == 0x30;
        ttlOff = (buf[1] & 0x01) ? (hasPktId ? 13 : 11) : (hasPktId ? 21 : 19);
      } else {
        ttlOff = (buf[0] == protocol::SYNC_BYTE) ? (protocol::SYNC_LEN + 1 + protocol::NODE_ID_LEN * 2) : (1 + protocol::NODE_ID_LEN * 2);
      }
      if (ttlOff < len) fwdBuf[ttlOff]--;
      queueDeferredRelay(fwdBuf, len, txSf, delayMs, hdr.from, relayPayloadHashVal);
    }
  }

  if (!node::isForMe(hdr.to) && !node::isBroadcast(hdr.to)) {
    if (hdr.opcode == protocol::OP_NACK && payloadLen >= 2) {
      uint16_t pktId = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
      if (pkt_cache::retransmitOverheard(hdr.from, hdr.to, pktId)) {
        RIFTLINK_LOG_EVENT("[RiftLink] Overhear retransmit pktId=%u\n", (unsigned)pktId);
      }
    }
    if (hdr.opcode == protocol::OP_ROUTE_REQ) {
      routing::onRouteReq(hdr.from, payload, payloadLen);
    } else if (hdr.opcode == protocol::OP_ROUTE_REPLY) {
      routing::onRouteReply(hdr.from, hdr.to, payload, payloadLen);
    } else if (hdr.opcode == protocol::OP_KEY_EXCHANGE && payloadLen >= 32) {
      if (neighbors::onHello(hdr.from, rssi)) {
        queueDisplayRequestInfoRedraw();  // Paper: обновить вкладку Info
        RIFTLINK_LOG_EVENT("[RiftLink] Neighbor: %02X%02X\n", hdr.from[0], hdr.from[1]);
      }
      bool hadKey = x25519_keys::hasKeyFor(hdr.from);
      x25519_keys::onKeyExchange(hdr.from, payload);
      if (!hadKey) {  // ответ только если ключа не было — иначе KEY_EXCHANGE storm
        bool useSf12 = (rssi < -90) || (sf == 12);
        x25519_keys::sendKeyExchange(hdr.from, useSf12, true, false);
      }
    } else if (hdr.opcode == protocol::OP_HELLO) {
      beacon_sync::onBeaconReceived(hdr.from);
      offline_queue::onNodeOnline(hdr.from);
      if (neighbors::onHello(hdr.from, rssi)) {
        ble::requestNeighborsNotify();
        queueDisplayRequestInfoRedraw();  // Paper: обновить вкладку Info при новом соседе
        RIFTLINK_LOG_EVENT("[RiftLink] Neighbor: %02X%02X\n", hdr.from[0], hdr.from[1]);
      }
      if (!x25519_keys::hasKeyFor(hdr.from)) { bool useSf12 = (rssi < -90) || (sf == 12); x25519_keys::sendKeyExchange(hdr.from, useSf12); }
    }
    return;
  }

  neighbors::updateRssi(hdr.from, rssi);

  switch (hdr.opcode) {
    case protocol::OP_KEY_EXCHANGE:
      if (payloadLen >= 32) {
        if (neighbors::onHello(hdr.from, rssi)) {
          ble::requestNeighborsNotify();
          queueDisplayRequestInfoRedraw();  // Paper: обновить вкладку Info
          RIFTLINK_LOG_EVENT("[RiftLink] Neighbor: %02X%02X\n", hdr.from[0], hdr.from[1]);
        }
        bool hadKey = x25519_keys::hasKeyFor(hdr.from);
        x25519_keys::onKeyExchange(hdr.from, payload);
        if (!hadKey) {
          ble::requestNeighborsNotify();  // приложение: обновить hasKey (ожидание ключа → готов)
          bool useSf12 = (rssi < -90) || (sf == 12);
          x25519_keys::sendKeyExchange(hdr.from, useSf12, true, false);
        }
      }
      break;

    case protocol::OP_HELLO:
      clock_drift::onHelloReceived(hdr.from);
      offline_queue::onNodeOnline(hdr.from);
      if (neighbors::onHello(hdr.from, rssi)) {
        ble::requestNeighborsNotify();
        queueDisplayRequestInfoRedraw();  // Paper: обновить вкладку Info при новом соседе
        RIFTLINK_LOG_EVENT("[RiftLink] Neighbor: %02X%02X\n", hdr.from[0], hdr.from[1]);
      }
      if (!x25519_keys::hasKeyFor(hdr.from)) { bool useSf12 = (rssi < -90) || (sf == 12); x25519_keys::sendKeyExchange(hdr.from, useSf12); }
      break;

    case protocol::OP_MSG:
      if (payloadLen > 0) {
        if (protocol::isEncrypted(hdr)) {
          size_t decLen = 0;
          if (!crypto::decryptFrom(hdr.from, payload, payloadLen, decBuf, &decLen) || decLen >= 256) {
            RIFTLINK_LOG_ERR("[RiftLink] Decrypt FAILED (from %02X%02X — no key?)\n", hdr.from[0], hdr.from[1]);
            if (!x25519_keys::hasKeyFor(hdr.from)) { bool useSf12 = (rssi < -90) || (sf == 12); x25519_keys::sendKeyExchange(hdr.from, useSf12); }
            break;
          }
          if (protocol::isCompressed(hdr)) {
            size_t d = compress::decompress(decBuf, decLen, tmpBuf, sizeof(tmpBuf));
            if (d == 0 || d >= 256) {
              RIFTLINK_LOG_ERR("[RiftLink] Decompress FAILED\n");
              break;
            }
            memcpy(decBuf, tmpBuf, d);
            decLen = d;
          }
          const char* msg;
          size_t msgLen;
          uint32_t msgId = 0;
          uint8_t ttlMinutes = 0;
          size_t off = 0;
          if (decLen >= 6 && decBuf[0] >= 1 && decBuf[0] <= 60) {
            ttlMinutes = decBuf[0];
            off = 1;
          }
          if (protocol::isAckReq(hdr) && decLen >= off + msg_queue::MSG_ID_LEN && node::isForMe(hdr.to)) {
            memcpy(&msgId, decBuf + off, msg_queue::MSG_ID_LEN);
            uint8_t ackPayload[msg_queue::MSG_ID_LEN];
            memcpy(ackPayload, decBuf + off, msg_queue::MSG_ID_LEN);
            uint8_t ackPkt[protocol::PAYLOAD_OFFSET + msg_queue::MSG_ID_LEN];
            size_t ackLen = protocol::buildPacket(ackPkt, sizeof(ackPkt),
                node::getId(), hdr.from, 31, protocol::OP_ACK,
                ackPayload, msg_queue::MSG_ID_LEN, false, false);
            if (ackLen > 0) {
              uint8_t txSf = neighbors::rssiToSfOrthogonal(hdr.from);
              if (txSf == 0) txSf = 12;
              queueDeferredAck(ackPkt, ackLen, txSf, 80);  // без блокировки — RX window для след. MSG
            }
            // Echo Protocol: broadcast echo для Witness ACK (отправитель может услышать даже при коллизии ACK)
            uint8_t echoPayload[12];
            memcpy(echoPayload, decBuf + off, msg_queue::MSG_ID_LEN);
            memcpy(echoPayload + msg_queue::MSG_ID_LEN, hdr.from, protocol::NODE_ID_LEN);
            uint8_t echoPkt[protocol::PAYLOAD_OFFSET + 32];
            size_t echoLen = protocol::buildPacket(echoPkt, sizeof(echoPkt),
                node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_ECHO,
                echoPayload, 12, false, false);
            if (echoLen > 0) {
              uint8_t echoSf = neighbors::rssiToSf(neighbors::getMinRssi());
              if (echoSf == 0) echoSf = 12;
              queueDeferredSend(echoPkt, echoLen, echoSf, 120);  // 120 ms после ACK — не коллидировать
            }
            msg = (const char*)(decBuf + off + msg_queue::MSG_ID_LEN);
            msgLen = decLen - off - msg_queue::MSG_ID_LEN;
          } else {
            msg = (const char*)(decBuf + off);
            msgLen = decLen - off;
          }
          if (msgLen < 256) {
            bool skipDisplay = (msgId != 0 && ucDedupSeen(hdr.from, msgId));
            if (!skipDisplay) {
              if (msgId != 0) ucDedupAdd(hdr.from, msgId);
              if (msgLen > 0) {
                memcpy(msgStrBuf, msg, msgLen);
                msgStrBuf[msgLen] = '\0';
              } else {
                strncpy(msgStrBuf, "(пустое)", sizeof(msgStrBuf) - 1);
                msgStrBuf[sizeof(msgStrBuf) - 1] = '\0';
              }
              ble::requestMsgNotify(hdr.from, msgStrBuf, msgId, rssi, ttlMinutes);
              char fromHex[17];
              snprintf(fromHex, sizeof(fromHex), "%02X%02X%02X%02X", hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3]);
              queueDisplayLastMsg(fromHex, msgStrBuf);
            }
          }
        } else {
          if (payloadLen < 256) {
            if (payloadLen > 0) {
              memcpy(msgStrBuf, payload, payloadLen);
              msgStrBuf[payloadLen] = '\0';
            } else {
              strncpy(msgStrBuf, "(пустое)", sizeof(msgStrBuf) - 1);
              msgStrBuf[sizeof(msgStrBuf) - 1] = '\0';
            }
            ble::requestMsgNotify(hdr.from, msgStrBuf, 0, rssi);
            char fromHex[17];
            snprintf(fromHex, sizeof(fromHex), "%02X%02X%02X%02X", hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3]);
            queueDisplayLastMsg(fromHex, msgStrBuf);
          }
        }
      }
      break;

    case protocol::OP_MSG_BATCH:
      if (payloadLen >= 1 && node::isForMe(hdr.to)) {
        uint8_t count = payload[0];
        if (count >= 1 && count <= 4) {
          size_t off = 1;
          for (uint8_t i = 0; i < count && off + 2 <= payloadLen; i++) {
            uint16_t encLen = (uint16_t)payload[off] | ((uint16_t)payload[off + 1] << 8);
            off += 2;
            if (encLen == 0 || off + encLen > payloadLen) break;
            size_t decLen = 0;
            if (!crypto::decryptFrom(hdr.from, payload + off, encLen, decBuf, &decLen) || decLen >= 256) break;
            off += encLen;
            size_t d = compress::decompress(decBuf, decLen, tmpBuf, sizeof(tmpBuf));
            if (d > 0 && d < 256) {
              memcpy(decBuf, tmpBuf, d);
              decLen = d;
            }
            if (decLen >= 6 && decBuf[0] >= 1 && decBuf[0] <= 60) {
              size_t msgOff = 1;
              uint32_t msgId = 0;
              memcpy(&msgId, decBuf + msgOff, msg_queue::MSG_ID_LEN);
              msgOff += msg_queue::MSG_ID_LEN;
              uint8_t ackPayload[msg_queue::MSG_ID_LEN];
              memcpy(ackPayload, &msgId, msg_queue::MSG_ID_LEN);
              uint8_t ackPkt[protocol::PAYLOAD_OFFSET + msg_queue::MSG_ID_LEN];
              size_t ackLen = protocol::buildPacket(ackPkt, sizeof(ackPkt),
                  node::getId(), hdr.from, 31, protocol::OP_ACK,
                  ackPayload, msg_queue::MSG_ID_LEN, false, false);
              if (ackLen > 0) {
                uint8_t txSf = neighbors::rssiToSfOrthogonal(hdr.from);
                if (txSf == 0) txSf = 12;
                queueDeferredAck(ackPkt, ackLen, txSf, 80 + (i * 50));
              }
              size_t msgLen = decLen - msgOff;
              if (msgLen < 256 && !ucDedupSeen(hdr.from, msgId)) {
                ucDedupAdd(hdr.from, msgId);
                memcpy(msgStrBuf, decBuf + msgOff, msgLen);
                msgStrBuf[msgLen] = '\0';
                ble::requestMsgNotify(hdr.from, msgStrBuf, msgId, rssi, decBuf[0]);
                char fromHex[17];
                snprintf(fromHex, sizeof(fromHex), "%02X%02X%02X%02X", hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3]);
                queueDisplayLastMsg(fromHex, msgStrBuf);
              }
            }
          }
        }
      }
      break;

    case protocol::OP_ACK:
      if (payloadLen >= msg_queue::MSG_ID_LEN) {
        uint32_t msgId;
        memcpy(&msgId, payload, msg_queue::MSG_ID_LEN);
        if (msg_queue::onAckReceived(hdr.from, payload, payloadLen)) {
          ble::notifyDelivered(hdr.from, msgId, rssi);
        }
      }
      break;

    case protocol::OP_ECHO:
      // Echo Protocol: broadcast msgId+originalFrom — отправитель принимает как ACK
      if (payloadLen >= 12 && memcmp(payload + msg_queue::MSG_ID_LEN, node::getId(), protocol::NODE_ID_LEN) == 0) {
        uint32_t msgId;
        memcpy(&msgId, payload, msg_queue::MSG_ID_LEN);
        if (msg_queue::onAckReceived(hdr.from, payload, msg_queue::MSG_ID_LEN)) {
          ble::notifyDelivered(hdr.from, msgId, rssi);
        }
      }
      break;

    case protocol::OP_POLL:
      // RIT: получатель шлёт «присылайте пакеты» — ускоряем отправку pending для него
      msg_queue::onPollReceived(hdr.from);
      break;

    case protocol::OP_READ:
      if (payloadLen >= msg_queue::MSG_ID_LEN) {
        uint32_t msgId;
        memcpy(&msgId, payload, msg_queue::MSG_ID_LEN);
        ble::notifyRead(hdr.from, msgId, rssi);
      }
      break;

    case protocol::OP_TELEMETRY:
      // Системная инфо (батарея, heap) — не отправляем в приложение, не для чатов
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
          ble::notifyLocation(hdr.from, lat, lon, alt, rssi);
        }
      }
      break;

    case protocol::OP_PING:
      if (node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) {
        uint8_t pongPkt[protocol::SYNC_LEN + protocol::HEADER_LEN];
        size_t pongLen = protocol::buildPacket(pongPkt, sizeof(pongPkt),
            node::getId(), hdr.from, 31, protocol::OP_PONG, nullptr, 0);
        if (pongLen > 0) {
          uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(hdr.from));
          if (txSf == 0) txSf = 12;
          if (!radio::send(pongPkt, pongLen, txSf, true)) {
            queueDeferredSend(pongPkt, pongLen, txSf, 50);  // fallback при полной sendQueue
          }
        }
      }
      break;

    case protocol::OP_ROUTE_REQ:
      routing::onRouteReq(hdr.from, payload, payloadLen);
      break;

    case protocol::OP_ROUTE_REPLY:
      if (routing::onRouteReply(hdr.from, hdr.to, payload, payloadLen) && node::isForMe(hdr.to)) {
        ble::notifyRoutes();
      }
      break;

    case protocol::OP_PONG:
      ble::notifyPong(hdr.from, rssi);
      break;

    case protocol::OP_NACK:
      if (payloadLen >= 2 && node::isForMe(hdr.to)) {
        uint16_t pktId = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
        if (pkt_cache::retransmitOnNack(hdr.from, pktId)) {
          region::switchChannelOnCongestion();  // Channel Hopping при NACK
          RIFTLINK_LOG_EVENT("[RiftLink] NACK retransmit pktId=%u to %02X%02X\n",
              (unsigned)pktId, hdr.from[0], hdr.from[1]);
        }
      }
      break;

    case protocol::OP_GROUP_MSG:
      if (payloadLen > 0 && protocol::isEncrypted(hdr)) {
        if (node::isForMe(hdr.from)) break;  // своё сообщение (relay echo) — пропуск
        size_t decLen = 0;
        if (!crypto::decrypt(payload, payloadLen, decBuf, &decLen) || decLen < GROUP_ID_LEN) break;
        if (protocol::isCompressed(hdr)) {
          size_t d = compress::decompress(decBuf, decLen, tmpBuf, sizeof(tmpBuf));
          if (d == 0 || d < GROUP_ID_LEN) break;
          memcpy(decBuf, tmpBuf, d);
          decLen = d;
        }
        uint32_t groupId;
        memcpy(&groupId, decBuf, GROUP_ID_LEN);
        if (!groups::isInGroup(groupId)) break;
        const char* msg;
        size_t msgLen;
        if (decLen >= GROUP_ID_LEN + msg_queue::MSG_ID_LEN) {
          uint32_t bcMsgId;
          memcpy(&bcMsgId, decBuf + GROUP_ID_LEN, msg_queue::MSG_ID_LEN);
          if (bcDedupSeen(hdr.from, bcMsgId)) break;  // дубликат broadcast (3x repeat)
          bcDedupAdd(hdr.from, bcMsgId);
          msg = (const char*)(decBuf + GROUP_ID_LEN + msg_queue::MSG_ID_LEN);
          msgLen = decLen - GROUP_ID_LEN - msg_queue::MSG_ID_LEN;
          // ACK отправителю — для статуса delivered X/Y
          uint8_t ackPayload[msg_queue::MSG_ID_LEN];
          memcpy(ackPayload, &bcMsgId, msg_queue::MSG_ID_LEN);
          uint8_t ackPkt[protocol::PAYLOAD_OFFSET + msg_queue::MSG_ID_LEN];
          size_t ackLen = protocol::buildPacket(ackPkt, sizeof(ackPkt),
              node::getId(), hdr.from, 31, protocol::OP_ACK, ackPayload, msg_queue::MSG_ID_LEN, false, false);
          if (ackLen > 0) {
            uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(hdr.from));
            // 250–400 ms — после burst копий 1–2 (0, 220ms), jitter чтобы несколько получателей не коллидировали
            uint32_t ackDelay = 250 + (esp_random() % 150);
            queueDeferredAck(ackPkt, ackLen, txSf, ackDelay);
          }
        } else {
          msg = (const char*)(decBuf + GROUP_ID_LEN);
          msgLen = decLen - GROUP_ID_LEN;
        }
        if (msgLen < 256) {
          memcpy(msgStrBuf, msg, msgLen);
          msgStrBuf[msgLen] = '\0';
          ble::requestMsgNotify(hdr.from, msgStrBuf, 0, rssi);
          char fromHex[17];
          snprintf(fromHex, sizeof(fromHex), "grp%u %02X%02X", (unsigned)groupId, hdr.from[0], hdr.from[1]);
          queueDisplayLastMsg(fromHex, msgStrBuf);
        }
      }
      break;

    case protocol::OP_MSG_FRAG:
      if (payloadLen >= 6) {
        size_t outLen = 0;
        if (frag::onFragment(hdr.from, hdr.to, payload, payloadLen,
                             protocol::isCompressed(hdr), s_fragOutBuf, sizeof(s_fragOutBuf), &outLen)) {
          if (outLen > 0 && outLen < sizeof(s_fragOutBuf)) {
            s_fragOutBuf[outLen] = '\0';
            const char* msgStr = (const char*)s_fragOutBuf;
            ble::requestMsgNotify(hdr.from, msgStr, 0, rssi);
            char fromHex[17];
            snprintf(fromHex, sizeof(fromHex), "%02X%02X%02X%02X", hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3]);
            queueDisplayLastMsg(fromHex, msgStr);
          }
        }
      }
      break;

    case protocol::OP_VOICE_MSG:
      if (payloadLen >= 6 && node::isForMe(hdr.to)) {
        size_t outLen = 0;
        if (voice_frag::onFragment(hdr.from, hdr.to, payload, payloadLen,
                                  s_voiceOutBuf, sizeof(s_voiceOutBuf), &outLen)) {
          if (outLen > 0) {
            ble::notifyVoice(hdr.from, s_voiceOutBuf, outLen);
          }
        }
      }
      break;

    default:
      break;
  }
}

#define LED_PIN 35  // Heltec V3/V4

#define DRAIN_TASK_STACK 4096
#define DRAIN_TASK_PRIO 2

static void drainTask(void* arg) {
  vTaskDelay(pdMS_TO_TICKS(500));  // дать setup завершиться
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(50 + (esp_random() % 51)));  // jitter — не блокирует loop
    flushDeferredSends();
    if (!sendQueue || uxQueueMessagesWaiting(sendQueue) == 0) continue;  // нечего слать — не держим mutex, RX свободен
    if (radio::takeMutex(pdMS_TO_TICKS(200)) != pdTRUE) continue;
    SendQueueItem item;
#define DRAIN_PACKETS_PER_CYCLE 4   // было 3 — быстрее освобождаем очередь
#if defined(SF_FORCE_7)
    for (int i = 0; i < DRAIN_PACKETS_PER_CYCLE && xQueueReceive(sendQueue, &item, 0) == pdTRUE; i++) {
      if (item.len >= 54 && item.buf[0] == protocol::SYNC_BYTE && item.buf[2] == protocol::OP_KEY_EXCHANGE) {
        queueDeferredSend(item.buf, item.len, item.txSf, 500 + (esp_random() % 1501));
        continue;
      }
      radio::sendDirectInternal(item.buf, item.len);
      if (item.buf[0] == protocol::SYNC_BYTE && item.buf[2] == protocol::OP_MSG) {
        vTaskDelay(pdMS_TO_TICKS(60));  // окно RX — дать получателю отправить ACK
      }
    }
#else
    int highSfDrained = 0;
    for (int i = 0; i < DRAIN_PACKETS_PER_CYCLE && xQueueReceive(sendQueue, &item, 0) == pdTRUE; i++) {
      if (item.txSf >= 10 && highSfDrained >= 2) {
        xQueueSendToFront(sendQueue, &item, 0);
        break;
      }
      if (item.len >= 54 && item.buf[0] == protocol::SYNC_BYTE && item.buf[2] == protocol::OP_KEY_EXCHANGE) {
        queueDeferredSend(item.buf, item.len, item.txSf, 500 + (esp_random() % 1501));
        continue;
      }
      if (item.txSf >= 7 && item.txSf <= 12) {
        radio::setSpreadingFactor(item.txSf);
        if (item.txSf >= 10) {
          highSfDrained++;
          s_nextRxSf12 = true;
        }
      }
      radio::sendDirectInternal(item.buf, item.len);
      if (item.buf[0] == protocol::SYNC_BYTE && item.buf[2] == protocol::OP_MSG) {
        vTaskDelay(pdMS_TO_TICKS(60));  // окно RX — дать получателю отправить ACK
      }
    }
#endif
    radio::releaseMutex();
  }
}

#if defined(USE_EINK)
#define VEXT_PIN 45
#define VEXT_ON_LEVEL LOW
#endif

/** Задержка с yield — не блокировать планировщик */
static void delayYield(uint32_t ms) {
  for (uint32_t t = millis(); millis() - t < ms;) yield();
}

void setup() {
  Serial.begin(115200);
  delayYield(300);  // дать USB CDC / UART стабилизироваться
  Serial.println("[RiftLink] boot...");
  pinMode(BUTTON_PIN, INPUT_PULLUP);  // USER_SW — до displayInit, чтобы кнопка работала на Paper
  ledInit(LED_PIN);
#if defined(USE_EINK)
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, VEXT_ON_LEVEL);
  delayYield(300);  // стабилизация питания E-Ink
#endif
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH);
    delayYield(100);
    digitalWrite(LED_PIN, LOW);
    delayYield(100);
  }
  delayYield(200);

  // Default event loop до любого esp_wifi_* — иначе "failed to post WiFi event=2 ret=259" (WIFI_EVENT_259_ANALYSIS.md)
  esp_err_t ev = esp_event_loop_create_default();
  if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) {
    Serial.printf("[RiftLink] Event loop: %s\n", esp_err_to_name(ev));
  }

  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs = nvs_flash_init();
  }
  if (nvs != ESP_OK) {
    RIFTLINK_LOG_ERR("[RiftLink] NVS init FAILED: %s (0x%x) — настройки не сохранятся\n",
        esp_err_to_name(nvs), (unsigned)nvs);
  }
#if defined(USE_EINK)
  wifi::init();  // Paper: до displayInit (макс. heap), при OOM — WiFi/OTA отключены
  Serial.printf("[RiftLink] Heap after wifi::init: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
  locale::init();
  displayInit();
#if defined(USE_EINK)
  Serial.printf("[RiftLink] Heap after displayInit: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
  displayShowBootScreen();
  s_bootPhase = BOOT_PHASE_WAIT_4S;
  s_bootPhaseStart = millis();
  // 4s ожидание перенесено в loop — displayUpdate() во время загрузки
}

static void runBootStateMachine() {
  if (s_bootPhase != BOOT_PHASE_WAIT_4S) return;
  if (millis() - s_bootPhaseStart < 4000) {
    displayUpdate();
    return;
  }
  s_bootPhase = BOOT_PHASE_DONE;
  if (!locale::isSet()) {
    displayShowLanguagePicker();
  }
  displayClear();
  displaySetTextSize(1);
  displayText(0, 0, locale::getForDisplay("init"));
  displayShow();
  node::init();
  region::init();
  crypto::init();
  x25519_keys::init();
#if !defined(USE_EINK)
  wifi::init();  // OLED: после crypto
#endif
  if (!radio::init()) {
    RIFTLINK_LOG_ERR("[RiftLink] Radio init FAILED\n");
    displayText(0, 10, locale::getForDisplay("radio_fail"));
    displayShow();
  } else {
    displayText(0, 10, locale::getForDisplay("radio_ok"));
    displayShow();
  }
#if defined(USE_EINK)
  Serial.printf("[RiftLink] Heap after radio::init: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
  if (!region::isSet()) {
    displayShowRegionPicker();
  }
  if (!ble::init()) {
    RIFTLINK_LOG_ERR("[RiftLink] BLE init FAILED — устройство не будет видно в скане\n");
  }
#if defined(USE_EINK)
  Serial.printf("[RiftLink] Heap after ble::init: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
  ble::setOnSend(sendMsg);
  ble::setOnLocation(sendLocation);
  msg_queue::init();
  mab::init();
  collision_slots::init();
  beacon_sync::init();
  clock_drift::init();
  bls_n::init();
  packet_fusion::init();
  packet_fusion::setOnBatchSent([](const uint8_t* to, const uint32_t* msgIds, int count) {
    msg_queue::registerBatchSent(to, msgIds, count);
  });
  packet_fusion::setOnSingleFlush([](const uint8_t* to, uint32_t msgId, const uint8_t* pkt, size_t pktLen, uint8_t txSf) {
    return msg_queue::registerPendingFromFusion(to, msgId, pkt, pktLen, txSf);
  });
  network_coding::init();
#if defined(USE_EINK)
  Serial.printf("[RiftLink] Heap after packet_fusion+network_coding: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
  msg_queue::setOnUnicastSent([](const uint8_t* to, uint32_t msgId) { ble::notifySent(to, msgId); });
  msg_queue::setOnUnicastUndelivered([](const uint8_t* to, uint32_t msgId) {
    radio::notifyCongestion();
    ble::notifyUndelivered(to, msgId);
  });
  msg_queue::setOnBroadcastSent([](uint32_t msgId) { ble::notifySent(protocol::BROADCAST_ID, msgId); });
  msg_queue::setOnBroadcastDelivery([](uint32_t msgId, int d, int t) { ble::notifyBroadcastDelivery(msgId, d, t); });
  frag::init();
#if defined(USE_EINK)
  Serial.printf("[RiftLink] Heap after frag::init: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
  telemetry::init();
  neighbors::init();
#if defined(USE_EINK)
  Serial.printf("[RiftLink] Heap after neighbors::init: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
  pkt_cache::init();
  groups::init();
  offline_queue::init();
  routing::init();
#if defined(USE_EINK)
  Serial.printf("[RiftLink] Heap after routing::init: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
  voice_frag::init();
#if defined(USE_EINK)
  Serial.printf("[RiftLink] Heap after voice_frag::init: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
  powersave::init();
  // Очереди, затем WiFi/ESP-NOW до async tasks — event loop WiFi нужен heap (~4KB task)
#if defined(USE_EINK)
  Serial.printf("[RiftLink] Heap before asyncQueues: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
  if (!asyncQueuesInit()) {
    RIFTLINK_LOG_ERR("[RiftLink] Async queues init FAILED\n");
  }
  if (wifi::isAvailable()) {
    Serial.printf("[RiftLink] Heap before ESP-NOW: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    esp_now_slots::init();
    if (wifi::hasCredentials()) wifi::connect();
  }
  gps::init();
  if (packetQueue && sendQueue && displayQueue) {
    radio::setAsyncMode(true);
    displaySetButtonPolledExternally(true);
    asyncTasksStart();
#if defined(USE_EINK)
    xTaskCreatePinnedToCore(drainTask, "drain", DRAIN_TASK_STACK, nullptr, DRAIN_TASK_PRIO, nullptr, 1);
#else
    xTaskCreate(drainTask, "drain", DRAIN_TASK_STACK, nullptr, DRAIN_TASK_PRIO, nullptr);
#endif
  }
  displayShowScreenForceFull(0);
  s_bootTime = millis();
  s_lastSf12Burst = millis();
  s_lastKeyRetry = millis() + (node::getId()[0] % 16) * 500;
}

void loop() {
  if (s_bootPhase != BOOT_PHASE_DONE) {
    runBootStateMachine();
    return;
  }
  if (millis() < s_loopCooldownUntil) {
    vTaskDelay(pdMS_TO_TICKS(1));
    return;
  }
  bool aggressive = (millis() - s_bootTime) < 120000;  // первые 2 мин — агрессивный discovery

  if (ota::isActive()) {
    ota::update();
    pollButtonAndQueue();
    displayUpdate();
    s_loopCooldownUntil = millis() + 10;
    return;
  }

  // HELLO: 0 сосед — 18с (aggressive discovery); 1+ сосед — 30с; 6+ — 24с (меньше спама)
  // Фазирование по слоту (beacon_sync): при 0 соседях каждый узел смещён на свой слот — меньше storm
  int nNeigh = neighbors::getCount();
  uint32_t helloInterval = (nNeigh == 0 && aggressive) ? HELLO_INTERVAL_AGGRESSIVE_MS
      : ((nNeigh == 0) ? HELLO_INTERVAL_AGGRESSIVE_MS : (nNeigh >= 6 ? 24000 : HELLO_INTERVAL_MS));
  uint32_t jitterMs = (nNeigh == 0) ? HELLO_JITTER_ZERO_MS : HELLO_JITTER_MS;
  uint32_t jitter = (esp_random() % (jitterMs * 2)) - (int32_t)jitterMs;
  uint32_t phaseOffset = (nNeigh == 0) ? (beacon_sync::getSlotFor(node::getId()) * 400u) : 0;
  if (millis() - lastHello > (helloInterval + (int32_t)jitter + phaseOffset)) {
    sendHello();
    lastHello = millis();
  }
  if (millis() - lastPoll > POLL_INTERVAL_MS) {
    sendPoll();
    lastPoll = millis();
  }
  if (millis() - lastTelemetry > TELEM_INTERVAL_MS) {
    telemetry::send();
    lastTelemetry = millis();
  }

  // Адаптивный SF: по среднему RSSI соседей — SF7 (хорошая связь), SF9 (средняя), SF12 (слабая)
#if !defined(SF_FORCE_7)
  if (millis() - lastSfAdapt > SF_ADAPT_INTERVAL_MS) {
    int avgRssi = neighbors::getAverageRssi();
    uint8_t sf = 7;
    if (neighbors::getCount() > 0) {
      if (avgRssi < -110) sf = 12;
      else if (avgRssi < -80) sf = 9;
    }
    radio::setSpreadingFactor(sf);
    lastSfAdapt = millis();
  }
#endif

  gps::update();
  if (gps::isPresent() && gps::isEnabled() && gps::hasFix() &&
      millis() - lastGpsLoc > GPS_LOC_INTERVAL_MS) {
    sendLocation(gps::getLat(), gps::getLon(), gps::getAlt());
    lastGpsLoc = millis();
  }

  // Serial: "send <text>" = broadcast; "send <hex8> <text>" = to node
  if (Serial.available()) {
    Serial.setTimeout(50);  // не блокировать 1s — displayUpdate должен вызываться чаще
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.startsWith("send ")) {
      String rest = cmd.substring(5);
      int sp = rest.indexOf(' ');
      String tok1 = sp >= 0 ? rest.substring(0, sp) : rest;
      String text = sp >= 0 ? rest.substring(sp + 1) : "";
      bool isHex = (tok1.length() == 8);
      for (int i = 0; isHex && i < 8; i++) {
        char c = tok1.charAt(i);
        isHex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
      }
      if (isHex && text.length() > 0) {
        uint8_t to[protocol::NODE_ID_LEN];
        memset(to, 0xFF, protocol::NODE_ID_LEN);
        for (int i = 0; i < 4; i++) {
          to[i] = (uint8_t)strtoul(tok1.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
        }
        sendMsg(to, text.c_str(), 0);
      } else if (rest.length() > 0) {
        sendMsg(protocol::BROADCAST_ID, rest.c_str(), 0);
      }
    } else if (cmd.startsWith("ping ")) {
      String hex8 = cmd.substring(5);
      hex8.trim();
      if (hex8.length() == 8) {
        bool ok = true;
        uint8_t to[protocol::NODE_ID_LEN];
        memset(to, 0xFF, protocol::NODE_ID_LEN);
        for (int i = 0; i < 4; i++) {
          to[i] = (uint8_t)strtoul(hex8.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
        }
        uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN];
        size_t len = protocol::buildPacket(pkt, sizeof(pkt),
            node::getId(), to, 31, protocol::OP_PING, nullptr, 0);
        if (len > 0 && radio::send(pkt, len, neighbors::rssiToSf(neighbors::getRssiFor(to)))) {
          Serial.printf("[RiftLink] PING sent to %s\n", hex8.c_str());
        } else {
          Serial.println("[RiftLink] PING failed");
        }
      } else {
        Serial.println("[RiftLink] ping <hex8>");
      }
    } else if (cmd.startsWith("region ")) {
      String r = cmd.substring(7);
      r.trim();
      if (region::setRegion(r.c_str())) {
        Serial.printf("[RiftLink] Region: %s (%.1f MHz)\n", region::getCode(), region::getFreq());
        queueDisplayRedraw(displayGetCurrentScreen());
      } else {
        Serial.println("[RiftLink] Region: EU|UK|RU|US|AU");
      }
    } else if (cmd.startsWith("channel ")) {
      if (region::getChannelCount() > 0) {
        int ch = cmd.substring(8).toInt();
        if (ch >= 0 && ch <= 2 && region::setChannel(ch)) {
          Serial.printf("[RiftLink] Channel: %d (%.1f MHz)\n", ch, region::getFreq());
          queueDisplayRedraw(displayGetCurrentScreen());
        } else {
          Serial.println("[RiftLink] channel 0|1|2 (EU/UK only)");
        }
      } else {
        Serial.println("[RiftLink] channel 0|1|2 (EU/UK only)");
      }
    } else if (cmd.startsWith("nickname ")) {
      String nick = cmd.substring(9);
      nick.trim();
      if (nick.length() <= 16 && node::setNickname(nick.c_str())) {
        Serial.printf("[RiftLink] Nickname: %s\n", nick.c_str());
      } else {
        Serial.println("[RiftLink] nickname <name> (max 16 chars)");
      }
    } else if (cmd.startsWith("route ")) {
      String hex8 = cmd.substring(6);
      hex8.trim();
      if (hex8.length() == 8) {
        uint8_t target[protocol::NODE_ID_LEN];
        memset(target, 0xFF, protocol::NODE_ID_LEN);
        for (int i = 0; i < 4; i++) {
          target[i] = (uint8_t)strtoul(hex8.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
        }
        routing::requestRoute(target);
      } else {
        Serial.println("[RiftLink] route <hex8>");
      }
    } else if (cmd.startsWith("lang ")) {
      String l = cmd.substring(5);
      l.trim();
      l.toLowerCase();
      int lang = (l == "ru") ? LANG_RU : LANG_EN;
      if (l == "en" || l == "ru") {
        locale::setLang(lang);
        Serial.printf("[RiftLink] Language: %s\n", l.c_str());
        queueDisplayRedraw(displayGetCurrentScreen());
      } else {
        Serial.println("[RiftLink] lang en|ru");
      }
    } else if (cmd == "gps") {
      int rx, tx, en;
      gps::getPins(&rx, &tx, &en);
      Serial.printf("[RiftLink] GPS: %s %s rx=%d tx=%d en=%d\n",
          gps::isPresent() ? "present" : "absent",
          gps::isEnabled() ? "on" : "off", rx, tx, en);
      if (gps::hasFix()) {
        Serial.printf("[RiftLink] Fix: %.5f, %.5f\n", gps::getLat(), gps::getLon());
      }
    } else if (cmd == "gps on") {
      gps::setEnabled(true);
      Serial.println("[RiftLink] GPS on");
      queueDisplayRedraw(displayGetCurrentScreen());
    } else if (cmd == "gps off") {
      gps::setEnabled(false);
      Serial.println("[RiftLink] GPS off");
      queueDisplayRedraw(displayGetCurrentScreen());
    } else if (cmd.startsWith("gps pins ")) {
      int rx = -1, tx = -1, en = -1;
      if (sscanf(cmd.c_str() + 9, "%d %d %d", &rx, &tx, &en) >= 2) {
        gps::setPins(rx, tx, en);
        gps::saveConfig();
        Serial.printf("[RiftLink] GPS pins rx=%d tx=%d en=%d\n", rx, tx, en);
      } else {
        Serial.println("[RiftLink] gps pins <rx> <tx> [en]");
      }
    } else if (cmd == "powersave") {
      Serial.printf("[RiftLink] Power save: %s (enabled=%s, BLE %s, OTA %s)\n",
          powersave::canSleep() ? "active" : "inactive",
          powersave::isEnabled() ? "yes" : "no",
          ble::isConnected() ? "connected" : "disconnected",
          ota::isActive() ? "active" : "idle");
    } else if (cmd == "powersave on") {
      powersave::setEnabled(true);
      Serial.println("[RiftLink] Power save enabled");
    } else if (cmd == "powersave off") {
      powersave::setEnabled(false);
      Serial.println("[RiftLink] Power save disabled");
    } else if (cmd == "selftest" || cmd == "test") {
      selftest::Result r;
      selftest::run(&r);
      queueDisplayRedraw(displayGetCurrentScreen());
    } else if (cmd == "sf" || cmd == "radio") {
      Serial.printf("[RiftLink] SF=%u, %.1f MHz, neighbors=%d\n",
          (unsigned)radio::getSpreadingFactor(), region::getFreq(), neighbors::getCount());
    }
  }

  pollButtonAndQueue();  // drain вынесен в drainTask — loop не блокируется

  // Fallback: loop помогает drain packetQueue если packetTask не успевает (V3/V4/Paper)
  if (packetQueue && uxQueueMessagesWaiting(packetQueue) > 8) {
    PacketQueueItem pitem;
    for (int i = 0; i < 8 && xQueueReceive(packetQueue, &pitem, 0) == pdTRUE; i++) {
      handlePacket(pitem.buf, pitem.len, (int)pitem.rssi, pitem.sf);
    }
  }

  // Retry KEY_EXCHANGE: каждые 25–35с (jitter!) — иначе оба узла синхронно → коллизии → deadlock
  // Фаза по nodeId[0]: разные узлы смещены — меньше шанс одновременного TX
  // Доп. jitter 0–3 с — ещё меньше синхронных TX при плотной сети
  #define KEY_RETRY_BASE_MS  25000
  #define KEY_RETRY_JITTER_MS 10000
  #define KEY_RETRY_EXTRA_JITTER_MS 3000
  static uint32_t s_keyRetryCooldownUntil = 0;  // без блокировки loop
  if (millis() >= s_lastKeyRetry && millis() >= s_keyRetryCooldownUntil) {
    uint32_t phase = (node::getId()[0] % 16) * 500;  // 0–8 с смещение по ID
    uint32_t extraJitter = esp_random() % KEY_RETRY_EXTRA_JITTER_MS;
    s_lastKeyRetry = millis() + KEY_RETRY_BASE_MS + (esp_random() % KEY_RETRY_JITTER_MS) + phase + extraJitter;
    int n = neighbors::getCount();
    for (int i = 0; i < n; i++) {
      uint8_t peerId[protocol::NODE_ID_LEN];
      if (neighbors::getId(i, peerId) && !x25519_keys::hasKeyFor(peerId)) {
        x25519_keys::sendKeyExchange(peerId, true);
        s_keyRetryCooldownUntil = millis() + 800 + (esp_random() % 400);  // 0.8–1.2 с, без delay()
        break;
      }
    }
  }

  // RX: powersave — loop (держим mutex, rxTask при canSleep не трогает радио); иначе — rxTask
  int n = 0;
  if (powersave::canSleep()) {
    if (radio::takeMutex(pdMS_TO_TICKS(100))) {
      radio::setSpreadingFactor(7);
      radio::startReceiveWithTimeout(1000);
      n = powersave::sleepUntilPacketOrTimeout(rxBuf, sizeof(rxBuf));  // sleep + receiveAsync внутри
      radio::releaseMutex();
    }
    if (n > 0) {
      int rssi = radio::getLastRssi();
      if (packetQueue) {
        PacketQueueItem item;
        if ((size_t)n <= sizeof(item.buf)) {
          memcpy(item.buf, rxBuf, (size_t)n);
          item.len = (uint16_t)n;
          item.rssi = (int8_t)rssi;
          item.sf = 7;
          xQueueSend(packetQueue, &item, pdMS_TO_TICKS(30));
        }
      } else {
        handlePacket(rxBuf, (size_t)n, rssi, 7);
      }
    }
  }

  ble::update();
  msg_queue::update();
  routing::update();
  offline_queue::update();
  pollButtonAndQueue();
  if (!displayQueue) displayUpdate();
  if (!powersave::canSleep()) {
    s_loopCooldownUntil = millis() + 10;
  }
}
