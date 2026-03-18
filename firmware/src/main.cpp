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

#include "radio/radio.h"
#include "protocol/packet.h"
#include "node/node.h"
#include "ui/display.h"
#include "crypto/crypto.h"
#include "ble/ble.h"
#include "msg_queue/msg_queue.h"
#include "compress/compress.h"
#include "frag/frag.h"
#include "telemetry/telemetry.h"
#include "ota/ota.h"
#include "region/region.h"
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
#include "duty_cycle/duty_cycle.h"
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
#define HELLO_INTERVAL_AGGRESSIVE_MS 15000  // 15с — discovery при 0 соседях
#define HELLO_JITTER_MS    3000   // ±3с — при 1+ соседях
#define HELLO_JITTER_ZERO_MS 2000 // ±2с при 0 соседях
#define TELEM_INTERVAL_MS  60000
#define GPS_LOC_INTERVAL_MS 60000  // интервал отправки локации с GPS
#define SF_ADAPT_INTERVAL_MS 30000 // адаптивный SF по качеству связи

static uint32_t lastHello = 0;
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
static uint8_t rxBuf[protocol::SYNC_LEN + protocol::HEADER_LEN + protocol::MAX_PAYLOAD + crypto::OVERHEAD];

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
#if defined(LED_PIN)
    digitalWrite(LED_PIN, HIGH);
    delay(20);
    digitalWrite(LED_PIN, LOW);
#endif
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

void sendMsg(const uint8_t* to, const char* text, uint8_t ttlMinutes) {
  if (text == nullptr || strlen(text) == 0) {
    ble::notifyError("send_empty", "Пустое сообщение не отправляется");
    return;
  }
  if (!msg_queue::enqueue(to, text, ttlMinutes)) {
    RIFTLINK_LOG_ERR("[RiftLink] Queue full or encrypt FAILED\n");
    ble::notifyError("send_failed", "Очередь полна или нет ключа шифрования");
  } else if (node::isBroadcast(to)) {
    ble::notifySent(protocol::BROADCAST_ID, 0);  // broadcast — без msgId, чтобы вкладка сообщений показывала отправку
  }
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

  // Self-reception: свой KEY_EXCHANGE (обрезанный до 13B) — отбросить без лога
  if (len == 13 && buf[0] == protocol::SYNC_BYTE && (buf[1] & 0xF0) == 0x20 &&
      buf[2] == protocol::OP_KEY_EXCHANGE && !(buf[1] & 0x01) &&  // unicast
      memcmp(buf + 3, node::getId(), protocol::NODE_ID_LEN) == 0) {
    return;  // наш исходящий KEY_EXCHANGE, принятый по эху/отражению
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
      uint8_t from[protocol::NODE_ID_LEN];
      memcpy(from, buf + 3, protocol::NODE_ID_LEN);
      if (!node::isBroadcast(from) && !node::isInvalidNodeId(from)) {
        neighbors::onHello(from, rssi);
      }
      return;
    } else {
      // len==13 и opcode MSG/TELEMETRY/GROUP_MSG/KEY_EXCHANGE — обрезанный пакет (коллизия/SF), не спамить
      bool truncated = (len == 13 && buf[0] == protocol::SYNC_BYTE && (buf[1] & 0xF0) == 0x20 &&
          (buf[2] == protocol::OP_MSG || buf[2] == protocol::OP_TELEMETRY || buf[2] == protocol::OP_GROUP_MSG ||
           buf[2] == protocol::OP_KEY_EXCHANGE));
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

  // HELLO всегда broadcast — иначе сдвиг/коррупция (ghost-соседи)
  if (hdr.opcode == protocol::OP_HELLO && !node::isBroadcast(hdr.to)) return;

  // Relay: unicast не для нас, или GROUP_MSG (broadcast) с TTL>0
  // HELLO — всегда broadcast, не ретранслируем (защита от парсинга с перепутанным to)
  // ROUTE_REQ/REPLY обрабатываются модулем routing
  bool needRelay = (hdr.ttl > 0) && (hdr.opcode != protocol::OP_ROUTE_REQ) &&
      (hdr.opcode != protocol::OP_ROUTE_REPLY) && (hdr.opcode != protocol::OP_HELLO) && (
      (!node::isForMe(hdr.to) && !node::isBroadcast(hdr.to)) ||
      (hdr.opcode == protocol::OP_GROUP_MSG) ||
      (hdr.opcode == protocol::OP_VOICE_MSG));
  if (needRelay) {
    memcpy(fwdBuf, buf, len);
    size_t ttlOff;
    if (buf[0] == protocol::SYNC_BYTE && (buf[1] & 0xF0) == 0x20) {
      ttlOff = (buf[1] & 0x01) ? 11 : 19;  // v2: broadcast=11, unicast=19
    } else {
      ttlOff = (buf[0] == protocol::SYNC_BYTE) ? (protocol::SYNC_LEN + 1 + protocol::NODE_ID_LEN * 2) : (1 + protocol::NODE_ID_LEN * 2);
    }
    if (ttlOff < len) fwdBuf[ttlOff]--;
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
    }
    radio::send(fwdBuf, len, txSf);
  }

  if (!node::isForMe(hdr.to) && !node::isBroadcast(hdr.to)) {
    if (hdr.opcode == protocol::OP_ROUTE_REQ) {
      routing::onRouteReq(hdr.from, payload, payloadLen);
    } else if (hdr.opcode == protocol::OP_ROUTE_REPLY) {
      routing::onRouteReply(hdr.from, hdr.to, payload, payloadLen);
    } else if (hdr.opcode == protocol::OP_KEY_EXCHANGE && payloadLen >= 32) {
      if (neighbors::onHello(hdr.from, rssi)) {
        queueDisplayRequestInfoRedraw();  // Paper: обновить вкладку Info
        RIFTLINK_LOG_EVENT("[RiftLink] Neighbor: %02X%02X\n", hdr.from[0], hdr.from[1]);
      }
      x25519_keys::onKeyExchange(hdr.from, payload);
      { bool useSf12 = (rssi < -90) || (sf == 12); x25519_keys::sendKeyExchange(hdr.from, useSf12); }
    } else if (hdr.opcode == protocol::OP_HELLO) {
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
          queueDisplayRequestInfoRedraw();  // Paper: обновить вкладку Info
          RIFTLINK_LOG_EVENT("[RiftLink] Neighbor: %02X%02X\n", hdr.from[0], hdr.from[1]);
        }
        x25519_keys::onKeyExchange(hdr.from, payload);
        { bool useSf12 = (rssi < -90) || (sf == 12); x25519_keys::sendKeyExchange(hdr.from, useSf12); }
      }
      break;

    case protocol::OP_HELLO:
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
            if (ackLen > 0) radio::send(ackPkt, ackLen, neighbors::rssiToSf(neighbors::getRssiFor(hdr.from)));
            msg = (const char*)(decBuf + off + msg_queue::MSG_ID_LEN);
            msgLen = decLen - off - msg_queue::MSG_ID_LEN;
          } else {
            msg = (const char*)(decBuf + off);
            msgLen = decLen - off;
          }
          if (msgLen < 256) {
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

    case protocol::OP_ACK:
      if (payloadLen >= msg_queue::MSG_ID_LEN) {
        uint32_t msgId;
        memcpy(&msgId, payload, msg_queue::MSG_ID_LEN);
        msg_queue::onAckReceived(payload, payloadLen);
        ble::notifyDelivered(hdr.from, msgId, rssi);
      }
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
        if (pongLen > 0) radio::send(pongPkt, pongLen, neighbors::rssiToSf(neighbors::getRssiFor(hdr.from)));
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
        const char* msg = (const char*)(decBuf + GROUP_ID_LEN);
        size_t msgLen = decLen - GROUP_ID_LEN;
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

#if defined(USE_EINK)
#define VEXT_PIN 45
#define VEXT_ON_LEVEL LOW
#endif

void setup() {
  Serial.begin(115200);
  delay(300);  // дать USB CDC / UART стабилизироваться
  Serial.println("[RiftLink] boot...");
  pinMode(BUTTON_PIN, INPUT_PULLUP);  // USER_SW — до displayInit, чтобы кнопка работала на Paper
  pinMode(LED_PIN, OUTPUT);
#if defined(USE_EINK)
  // VEXT — питание E-Ink (как Meshtastic: variant.h VEXT_ENABLE, main.cpp)
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, VEXT_ON_LEVEL);
  delay(300);  // стабилизация питания E-Ink перед инициализацией
#endif
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);
    delay(100);
  }
  delay(200);

  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs = nvs_flash_init();
  }
  if (nvs != ESP_OK) {
    RIFTLINK_LOG_ERR("[RiftLink] NVS init FAILED: %s (0x%x) — настройки не сохранятся\n",
        esp_err_to_name(nvs), (unsigned)nvs);
  }
  locale::init();

  displayInit();
  displayShowBootScreen();
  for (int i = 0; i < 8; i++) { delay(500); yield(); }  // 4s с yield — против watchdog
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
  if (!radio::init()) {
    RIFTLINK_LOG_ERR("[RiftLink] Radio init FAILED\n");
    displayText(0, 10, locale::getForDisplay("radio_fail"));
    displayShow();
  } else {
    displayText(0, 10, locale::getForDisplay("radio_ok"));
    displayShow();
  }

  if (!region::isSet()) {
    displayShowRegionPicker();
  }

  if (!ble::init()) {
    RIFTLINK_LOG_ERR("[RiftLink] BLE init FAILED — устройство не будет видно в скане\n");
  }
  ble::setOnSend(sendMsg);
  ble::setOnLocation(sendLocation);
  msg_queue::init();
  msg_queue::setOnUnicastSent([](const uint8_t* to, uint32_t msgId) { ble::notifySent(to, msgId); });
  frag::init();
  telemetry::init();
  neighbors::init();
  groups::init();
  offline_queue::init();
  routing::init();
  voice_frag::init();
  powersave::init();
  wifi::init();
  gps::init();
  if (wifi::hasCredentials()) wifi::connect();

  // Paper: первый кадр — full refresh (бут/язык/инит/регион → меню), до asyncTasksStart (гонка)
  displayShowScreenForceFull(0);

  if (!asyncQueuesInit()) {
    RIFTLINK_LOG_ERR("[RiftLink] Async queues init FAILED\n");
  } else {
    radio::setAsyncMode(true);
    displaySetButtonPolledExternally(true);
    asyncTasksStart();
  }

  s_bootTime = millis();
  s_lastSf12Burst = millis();
  // Фаза по nodeId — разные узлы не стартуют KEY_EXCHANGE retry одновременно
  s_lastKeyRetry = millis() + (node::getId()[0] % 16) * 500;
}

void loop() {
  bool aggressive = (millis() - s_bootTime) < 120000;  // первые 2 мин — агрессивный discovery

  if (ota::isActive()) {
    ota::update();
    pollButtonAndQueue();
    displayUpdate();
    delay(10);
    return;
  }

  // HELLO: 0 сосед — 15с (aggressive discovery); 1+ сосед — 30с; 6+ — 24с (меньше спама)
  int nNeigh = neighbors::getCount();
  uint32_t helloInterval = (nNeigh == 0 && aggressive) ? HELLO_INTERVAL_AGGRESSIVE_MS
      : ((nNeigh == 0) ? HELLO_INTERVAL_AGGRESSIVE_MS : (nNeigh >= 6 ? 24000 : HELLO_INTERVAL_MS));
  uint32_t jitterMs = (nNeigh == 0) ? HELLO_JITTER_ZERO_MS : HELLO_JITTER_MS;
  uint32_t jitter = (esp_random() % (jitterMs * 2)) - (int32_t)jitterMs;
  if (millis() - lastHello > (helloInterval + (int32_t)jitter)) {
    sendHello();
    lastHello = millis();
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

  // SF для RX — rxTask вызывает getNextRxSlotParams; loop только drain sendQueue

  pollButtonAndQueue();  // опрос до drain — не пропустить нажатие во время долгого TX
  // Drain sendQueue — TX с mutex (rxTask держит радио, мы ждём до 200ms)
  if (sendQueue && radio::takeMutex(pdMS_TO_TICKS(200))) {
    if (uxQueueMessagesWaiting(sendQueue) > 0) {
      delay(50 + (esp_random() % 51));  // jitter 50–100 ms — снижает одновременный старт (COLLISION_AVOIDANCE)
    }
    SendQueueItem item;
#if defined(SF_FORCE_7)
    for (int i = 0; i < 2 && xQueueReceive(sendQueue, &item, 0) == pdTRUE; i++) {
      if (item.len >= 54 && item.buf[0] == protocol::SYNC_BYTE && item.buf[2] == protocol::OP_KEY_EXCHANGE) {
        delay(500 + (esp_random() % 1501));
      }
      radio::sendDirectInternal(item.buf, item.len);
    }
#else
    int highSfDrained = 0;
    for (int i = 0; i < 2 && xQueueReceive(sendQueue, &item, 0) == pdTRUE; i++) {
      if (item.txSf >= 10 && highSfDrained >= 1) {
        xQueueSendToFront(sendQueue, &item, 0);
        break;
      }
      // KEY_EXCHANGE: jitter 500–2000 ms — half-duplex: при одновременном TX оба не получают ответ
      if (item.len >= 54 && item.buf[0] == protocol::SYNC_BYTE && item.buf[2] == protocol::OP_KEY_EXCHANGE) {
        delay(500 + (esp_random() % 1501));
      }
      if (item.txSf >= 7 && item.txSf <= 12) {
        radio::setSpreadingFactor(item.txSf);
        if (item.txSf >= 10) {
          highSfDrained++;
          s_nextRxSf12 = true;
        }
      }
      radio::sendDirectInternal(item.buf, item.len);
      // rxTask установит SF при следующем слоте — restore не нужен
    }
#endif
    radio::releaseMutex();
  }

#if defined(USE_EINK)
  // Paper: loop также обрабатывает packetQueue — fallback если packetTask не успевает/заблокирован
  if (packetQueue) {
    PacketQueueItem pitem;
    for (int i = 0; i < 8 && xQueueReceive(packetQueue, &pitem, 0) == pdTRUE; i++) {
      handlePacket(pitem.buf, pitem.len, (int)pitem.rssi, pitem.sf);
    }
  }
#endif

  // Retry KEY_EXCHANGE: каждые 25–35с (jitter!) — иначе оба узла синхронно → коллизии → deadlock
  // Фаза по nodeId[0]: разные узлы смещены — меньше шанс одновременного TX
  #define KEY_RETRY_BASE_MS  25000
  #define KEY_RETRY_JITTER_MS 10000
  if (millis() >= s_lastKeyRetry) {  // s_lastKeyRetry = время следующего retry (0 = сразу при старте)
    uint32_t phase = (node::getId()[0] % 16) * 500;  // 0–8 с смещение по ID
    s_lastKeyRetry = millis() + KEY_RETRY_BASE_MS + (esp_random() % KEY_RETRY_JITTER_MS) + phase;
    int n = neighbors::getCount();
    for (int i = 0; i < n; i++) {
      uint8_t peerId[protocol::NODE_ID_LEN];
      if (neighbors::getId(i, peerId) && !x25519_keys::hasKeyFor(peerId)) {
        x25519_keys::sendKeyExchange(peerId, true);
        delay(800 + (esp_random() % 400));  // 0.8–1.2 с между KEY_EXCHANGE — не коллидировать с собой
        break;  // один за цикл — остальные в следующий раз
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
    delay(10);
  }
}
