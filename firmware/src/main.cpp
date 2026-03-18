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

#define HELLO_INTERVAL_MS  10000
#define HELLO_JITTER_MS    2000   // ±2s — чтобы два устройства не передавали одновременно
#define TELEM_INTERVAL_MS  60000
#define GPS_LOC_INTERVAL_MS 60000  // интервал отправки локации с GPS
#define SF_ADAPT_INTERVAL_MS 30000 // адаптивный SF по качеству связи
#define MAX_NEIGHBORS      8

static uint32_t lastHello = 0;
static uint32_t lastTelemetry = 0;
static uint32_t lastGpsLoc = 0;
static uint32_t lastSfAdapt = 0;
static uint8_t rxBuf[protocol::HEADER_LEN + protocol::MAX_PAYLOAD + crypto::OVERHEAD];

// Статические буферы — не на стеке, иначе handlePacket переполняет loopTask
static uint8_t s_decBuf[256];
static uint8_t s_tmpBuf[256];
static char s_msgStrBuf[256];
static uint8_t s_fragOutBuf[frag::MAX_MSG_PLAIN];
static uint8_t s_voiceOutBuf[voice_frag::MAX_VOICE_PLAIN + 1024];
static uint8_t s_fwdBuf[protocol::HEADER_LEN + protocol::MAX_PAYLOAD + crypto::OVERHEAD];

void sendHello() {
  uint8_t pkt[protocol::HEADER_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID,
      31, protocol::OP_HELLO, nullptr, 0);
  if (len > 0 && radio::send(pkt, len)) {
    Serial.println("[RiftLink] HELLO sent");
  }
}

void sendMsg(const uint8_t* to, const char* text, uint8_t ttlMinutes) {
  if (!msg_queue::enqueue(to, text, ttlMinutes)) {
    Serial.println("[RiftLink] Queue full or encrypt FAILED");
    ble::notifyError("send_failed", "Очередь полна или нет ключа шифрования");
  } else {
    Serial.printf("[RiftLink] MSG queued (%s)\n", node::isBroadcast(to) ? "broadcast" : "unicast");
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
    Serial.println("[RiftLink] Location encrypt FAILED");
    ble::notifyError("location_encrypt", "Шифрование локации не удалось");
    return;
  }

  uint8_t pkt[protocol::HEADER_LEN + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_LOCATION,
      encBuf, encLen, true, false, false);
  if (len > 0) {
    radio::send(pkt, len);
    Serial.printf("[RiftLink] LOCATION sent %.5f,%.5f\n", lat, lon);
  }
}

void handlePacket(const uint8_t* buf, size_t len) {
  int rssi = radio::getLastRssi();
  protocol::PacketHeader hdr;
  const uint8_t* payload;
  size_t payloadLen;
  if (!protocol::parsePacket(buf, len, &hdr, &payload, &payloadLen)) return;

  // Relay: unicast не для нас, или GROUP_MSG (broadcast) с TTL>0
  // ROUTE_REQ/REPLY обрабатываются модулем routing
  bool needRelay = (hdr.ttl > 0) && (hdr.opcode != protocol::OP_ROUTE_REQ) &&
      (hdr.opcode != protocol::OP_ROUTE_REPLY) && (
      (!node::isForMe(hdr.to) && !node::isBroadcast(hdr.to)) ||
      (hdr.opcode == protocol::OP_GROUP_MSG) ||
      (hdr.opcode == protocol::OP_VOICE_MSG));
  if (needRelay) {
    memcpy(s_fwdBuf, buf, len);
    s_fwdBuf[1 + protocol::NODE_ID_LEN * 2]--;
    radio::send(s_fwdBuf, len);
    Serial.printf("[RiftLink] Relay from %02X%02X\n", hdr.from[0], hdr.from[1]);
  }

  if (!node::isForMe(hdr.to) && !node::isBroadcast(hdr.to)) {
    if (hdr.opcode == protocol::OP_ROUTE_REQ) {
      routing::onRouteReq(hdr.from, payload, payloadLen);
    } else if (hdr.opcode == protocol::OP_ROUTE_REPLY) {
      routing::onRouteReply(hdr.from, hdr.to, payload, payloadLen);
    } else if (hdr.opcode == protocol::OP_KEY_EXCHANGE && payloadLen >= 32) {
      x25519_keys::onKeyExchange(hdr.from, payload);
    }
    return;
  }

  neighbors::updateRssi(hdr.from, rssi);

  switch (hdr.opcode) {
    case protocol::OP_KEY_EXCHANGE:
      if (payloadLen >= 32) {
        x25519_keys::onKeyExchange(hdr.from, payload);
        x25519_keys::sendKeyExchange(hdr.from);
      }
      break;

    case protocol::OP_HELLO:
      offline_queue::onNodeOnline(hdr.from);
      if (neighbors::onHello(hdr.from, rssi)) ble::requestNeighborsNotify();
      if (!x25519_keys::hasKeyFor(hdr.from)) x25519_keys::sendKeyExchange(hdr.from);
      Serial.printf("[RiftLink] HELLO from %02X%02X%02X%02X\n",
          hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3]);
      break;

    case protocol::OP_MSG:
      if (payloadLen > 0) {
        if (protocol::isEncrypted(hdr)) {
          size_t decLen = 0;
          if (!crypto::decryptFrom(hdr.from, payload, payloadLen, s_decBuf, &decLen) || decLen >= 256) {
            Serial.println("[RiftLink] Decrypt FAILED");
            break;
          }
          if (protocol::isCompressed(hdr)) {
            size_t d = compress::decompress(s_decBuf, decLen, s_tmpBuf, sizeof(s_tmpBuf));
            if (d == 0 || d >= 256) {
              Serial.println("[RiftLink] Decompress FAILED");
              break;
            }
            memcpy(s_decBuf, s_tmpBuf, d);
            decLen = d;
          }
          const char* msg;
          size_t msgLen;
          uint32_t msgId = 0;
          uint8_t ttlMinutes = 0;
          size_t off = 0;
          if (decLen >= 6 && s_decBuf[0] >= 1 && s_decBuf[0] <= 60) {
            ttlMinutes = s_decBuf[0];
            off = 1;
          }
          if (protocol::isAckReq(hdr) && decLen >= off + msg_queue::MSG_ID_LEN && node::isForMe(hdr.to)) {
            memcpy(&msgId, s_decBuf + off, msg_queue::MSG_ID_LEN);
            uint8_t ackPayload[msg_queue::MSG_ID_LEN];
            memcpy(ackPayload, s_decBuf + off, msg_queue::MSG_ID_LEN);
            uint8_t ackPkt[protocol::HEADER_LEN + msg_queue::MSG_ID_LEN];
            size_t ackLen = protocol::buildPacket(ackPkt, sizeof(ackPkt),
                node::getId(), hdr.from, 31, protocol::OP_ACK,
                ackPayload, msg_queue::MSG_ID_LEN, false, false);
            if (ackLen > 0) radio::send(ackPkt, ackLen);
            msg = (const char*)(s_decBuf + off + msg_queue::MSG_ID_LEN);
            msgLen = decLen - off - msg_queue::MSG_ID_LEN;
          } else {
            msg = (const char*)(s_decBuf + off);
            msgLen = decLen - off;
          }
          if (msgLen < 256 && msgLen > 0) {
            memcpy(s_msgStrBuf, msg, msgLen);
            s_msgStrBuf[msgLen] = '\0';
            Serial.printf("[RiftLink] MSG from %02X%02X: %s\n", hdr.from[0], hdr.from[1], s_msgStrBuf);
            ble::requestMsgNotify(hdr.from, s_msgStrBuf, msgId, rssi, ttlMinutes);
            char fromHex[17];
            snprintf(fromHex, sizeof(fromHex), "%02X%02X%02X%02X", hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3]);
            displaySetLastMsg(fromHex, s_msgStrBuf);
          }
        } else {
          if (payloadLen < 256) {
            memcpy(s_msgStrBuf, payload, payloadLen);
            s_msgStrBuf[payloadLen] = '\0';
            Serial.printf("[RiftLink] MSG from %02X%02X: %s (plain)\n", hdr.from[0], hdr.from[1], s_msgStrBuf);
            ble::requestMsgNotify(hdr.from, s_msgStrBuf, 0, rssi);
            char fromHex[17];
            snprintf(fromHex, sizeof(fromHex), "%02X%02X%02X%02X", hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3]);
            displaySetLastMsg(fromHex, s_msgStrBuf);
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
        Serial.println("[RiftLink] ACK received");
      }
      break;

    case protocol::OP_READ:
      if (payloadLen >= msg_queue::MSG_ID_LEN) {
        uint32_t msgId;
        memcpy(&msgId, payload, msg_queue::MSG_ID_LEN);
        ble::notifyRead(hdr.from, msgId, rssi);
        Serial.printf("[RiftLink] READ from %02X%02X msgId=%u\n", hdr.from[0], hdr.from[1], (unsigned)msgId);
      }
      break;

    case protocol::OP_TELEMETRY:
      if (payloadLen > 0 && protocol::isEncrypted(hdr)) {
        uint8_t decBuf[16];
        size_t decLen = 0;
        if (crypto::decrypt(payload, payloadLen, decBuf, &decLen) && decLen >= 4) {
          uint16_t batMv, heapKb;
          memcpy(&batMv, decBuf, 2);
          memcpy(&heapKb, decBuf + 2, 2);
          Serial.printf("[RiftLink] TELEMETRY from %02X%02X: %u mV, %u KB\n", hdr.from[0], hdr.from[1], batMv, heapKb);
          ble::notifyTelemetry(hdr.from, batMv, heapKb, rssi);
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
          Serial.printf("[RiftLink] LOCATION from %02X%02X: %.5f,%.5f\n", hdr.from[0], hdr.from[1], lat, lon);
          ble::notifyLocation(hdr.from, lat, lon, alt, rssi);
        }
      }
      break;

    case protocol::OP_PING:
      if (node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) {
        uint8_t pongPkt[protocol::HEADER_LEN];
        size_t pongLen = protocol::buildPacket(pongPkt, sizeof(pongPkt),
            node::getId(), hdr.from, 31, protocol::OP_PONG, nullptr, 0);
        if (pongLen > 0) radio::send(pongPkt, pongLen);
        Serial.printf("[RiftLink] PING from %02X%02X -> PONG sent\n", hdr.from[0], hdr.from[1]);
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
      Serial.printf("[RiftLink] PONG from %02X%02X%02X%02X\n",
          hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3]);
      ble::notifyPong(hdr.from, rssi);
      break;

    case protocol::OP_GROUP_MSG:
      if (payloadLen > 0 && protocol::isEncrypted(hdr)) {
        size_t decLen = 0;
        if (!crypto::decrypt(payload, payloadLen, s_decBuf, &decLen) || decLen < GROUP_ID_LEN) break;
        if (protocol::isCompressed(hdr)) {
          size_t d = compress::decompress(s_decBuf, decLen, s_tmpBuf, sizeof(s_tmpBuf));
          if (d == 0 || d < GROUP_ID_LEN) break;
          memcpy(s_decBuf, s_tmpBuf, d);
          decLen = d;
        }
        uint32_t groupId;
        memcpy(&groupId, s_decBuf, GROUP_ID_LEN);
        if (!groups::isInGroup(groupId)) break;
        const char* msg = (const char*)(s_decBuf + GROUP_ID_LEN);
        size_t msgLen = decLen - GROUP_ID_LEN;
        if (msgLen < 256) {
          memcpy(s_msgStrBuf, msg, msgLen);
          s_msgStrBuf[msgLen] = '\0';
          Serial.printf("[RiftLink] GROUP_MSG grp%u from %02X%02X: %s\n", (unsigned)groupId, hdr.from[0], hdr.from[1], s_msgStrBuf);
          ble::requestMsgNotify(hdr.from, s_msgStrBuf, 0, rssi);
          char fromHex[17];
          snprintf(fromHex, sizeof(fromHex), "grp%u %02X%02X", (unsigned)groupId, hdr.from[0], hdr.from[1]);
          displaySetLastMsg(fromHex, s_msgStrBuf);
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
            Serial.printf("[RiftLink] MSG_FRAG from %02X%02X: %s\n", hdr.from[0], hdr.from[1], msgStr);
            ble::requestMsgNotify(hdr.from, msgStr, 0, rssi);
            char fromHex[17];
            snprintf(fromHex, sizeof(fromHex), "%02X%02X%02X%02X", hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3]);
            displaySetLastMsg(fromHex, msgStr);
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
      Serial.printf("[RiftLink] Unknown opcode 0x%02X\n", hdr.opcode);
  }
}

#define LED_PIN 35  // Heltec V3/V4

#if defined(USE_EINK)
#define VEXT_PIN 45
#define VEXT_ON_LEVEL LOW
#endif

void setup() {
#if defined(USE_EINK)
  // VEXT — питание E-Ink (как Meshtastic: variant.h VEXT_ENABLE, main.cpp)
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, VEXT_ON_LEVEL);
  delay(300);  // стабилизация питания E-Ink перед инициализацией
#endif
  pinMode(LED_PIN, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);
    delay(100);
  }
  Serial.begin(115200);
  delay(500);

  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.println("[RiftLink] NVS: перезапись раздела (no free pages / new version)");
    nvs_flash_erase();
    nvs = nvs_flash_init();
  }
  if (nvs != ESP_OK) {
    Serial.printf("[RiftLink] NVS init FAILED: %s (0x%x) — настройки не сохранятся\n",
        esp_err_to_name(nvs), (unsigned)nvs);
  }
  locale::init();

  displayInit();
  Serial.println("[RiftLink] displayInit done");
  // Бутскрин — сразу после инициализации, до выбора языка
  displayShowBootScreen();
  Serial.println("[RiftLink] boot screen done");
  for (int i = 0; i < 8; i++) { delay(500); yield(); }  // 4s с yield — против watchdog
  if (!locale::isSet()) {
    displayShowLanguagePicker();
  }

  displayClear();
  displaySetTextSize(1);
  displayText(0, 0, locale::getForDisplay("init"));
  Serial.println("[RiftLink] displayShow init...");
  displayShow();
  Serial.println("[RiftLink] displayShow init done");

  Serial.println("[RiftLink] node::init...");
  node::init();
  region::init();
  crypto::init();
  x25519_keys::init();
  Serial.println("[RiftLink] radio::init...");
  if (!radio::init()) {
    Serial.println("[RiftLink] Radio init FAILED");
    displayText(0, 10, locale::getForDisplay("radio_fail"));
    displayShow();
  } else {
    Serial.println("[RiftLink] radio ok, displayShow...");
    displayText(0, 10, locale::getForDisplay("radio_ok"));
    displayShow();
    Serial.println("[RiftLink] radio displayShow done");
  }

  if (!region::isSet()) {
    displayShowRegionPicker();
  }

  const uint8_t* id = node::getId();
  Serial.printf("[RiftLink] Node ID: %02X%02X%02X%02X...\n", id[0], id[1], id[2], id[3]);
  Serial.printf("[RiftLink] Region: %s (%.1f MHz, %d dBm)\n",
      region::getCode(), region::getFreq(), region::getPower());

  if (!ble::init()) {
    Serial.println("[RiftLink] BLE init FAILED — устройство не будет видно в скане");
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

  Serial.println("[RiftLink] Phase 5 - Ready (BLE + E2E + ACK + LZ4 + FRAG + LOC + TELEM + OTA + Region + ROUTE + VOICE + WiFi + GPS)");
  Serial.println("Serial: send | region | channel | nickname | ping | route | lang | gps | powersave | selftest");

  displayShowScreen(0);
}

void loop() {
  if (ota::isActive()) {
    ota::update();
    displayUpdate();
    delay(10);
    return;
  }

  // HELLO с джиттером — устройства не синхронизируются и не блокируют друг друга при TX
  uint32_t jitter = (esp_random() % (HELLO_JITTER_MS * 2)) - HELLO_JITTER_MS;
  if (millis() - lastHello > (HELLO_INTERVAL_MS + (int32_t)jitter)) {
    sendHello();
    lastHello = millis();
  }
  if (millis() - lastTelemetry > TELEM_INTERVAL_MS) {
    telemetry::send();
    lastTelemetry = millis();
  }

  // Адаптивный SF: по среднему RSSI соседей — SF7 (хорошая связь), SF9 (средняя), SF12 (слабая)
  if (millis() - lastSfAdapt > SF_ADAPT_INTERVAL_MS) {
    int avgRssi = neighbors::getAverageRssi();
    uint8_t sf = 7;
    if (avgRssi < -110) sf = 12;
    else if (avgRssi < -80) sf = 9;
    radio::setSpreadingFactor(sf);
    lastSfAdapt = millis();
  }

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
        uint8_t pkt[protocol::HEADER_LEN];
        size_t len = protocol::buildPacket(pkt, sizeof(pkt),
            node::getId(), to, 31, protocol::OP_PING, nullptr, 0);
        if (len > 0 && radio::send(pkt, len)) {
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
      } else {
        Serial.println("[RiftLink] Region: EU|UK|RU|US|AU");
      }
    } else if (cmd.startsWith("channel ")) {
      if (region::getChannelCount() > 0) {
        int ch = cmd.substring(8).toInt();
        if (ch >= 0 && ch <= 2 && region::setChannel(ch)) {
          Serial.printf("[RiftLink] Channel: %d (%.1f MHz)\n", ch, region::getFreq());
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
    } else if (cmd == "gps off") {
      gps::setEnabled(false);
      Serial.println("[RiftLink] GPS off");
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
    } else if (cmd == "sf" || cmd == "radio") {
      Serial.printf("[RiftLink] SF=%u, %.1f MHz, neighbors=%d\n",
          (unsigned)radio::getSpreadingFactor(), region::getFreq(), neighbors::getCount());
    }
  }

  int n;
  if (powersave::canSleep()) {
    radio::startReceiveWithTimeout(1000);
    n = powersave::sleepUntilPacketOrTimeout(rxBuf, sizeof(rxBuf));
  } else {
    // Окно приёма: при 0 соседей — 400ms (максимум для первого контакта), иначе 80ms (BLE) / 150ms (без BLE)
    uint32_t rxMs = 150;
    if (ble::isConnected()) rxMs = 80;
    if (neighbors::getCount() == 0) rxMs = 400;
    radio::startReceiveWithTimeout(rxMs);
    // Чанкованное ожидание — опрос кнопки каждые ~50ms, иначе кнопка не реагирует при длинном RX
    uint32_t t0 = millis();
    while (millis() - t0 < rxMs) {
      yield();
      displayUpdate();
      uint32_t elapsed = millis() - t0;
      if (elapsed >= rxMs) break;
      uint32_t chunk = (rxMs - elapsed) > 50 ? 50 : (rxMs - elapsed);
      delay(chunk);
    }
    n = radio::receiveAsync(rxBuf, sizeof(rxBuf));
  }
  if (n > 0) {
    int rssi = radio::getLastRssi();
    Serial.printf("[RiftLink] RX %d bytes, RSSI=%d\n", n, rssi);
    handlePacket(rxBuf, (size_t)n);
  }

  ble::update();
  msg_queue::update();
  routing::update();
  displayUpdate();
  if (!powersave::canSleep()) {
    delay(10);
  }
}
