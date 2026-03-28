/**
 * RiftLink — точка входа nRF52840 (FakeTech V5 / Heltec T114).
 */

#include <Arduino.h>

#include "async_queues.h"
#include "async_tasks.h"
#include "protocol/packet.h"
#include "ack_coalesce/ack_coalesce.h"
#include "beacon_sync/beacon_sync.h"
#include "ble/ble.h"
#include "clock_drift/clock_drift.h"
#include "collision_slots/collision_slots.h"
#include "crypto/crypto.h"
#include "frag/frag.h"
#include "gps/gps.h"
#include "groups/groups.h"
#include "handle_packet_nrf.h"
#include "display_nrf.h"
#include "kv.h"
#include "mab/mab.h"
#include "msg_queue/msg_queue.h"
#include "packet_fusion/packet_fusion.h"
#include "neighbors/neighbors.h"
#include "network_coding/network_coding.h"
#include "node/node.h"
#include "offline_queue/offline_queue.h"
#include "pkt_cache/pkt_cache.h"
#include "locale/locale.h"
#include "log.h"
#include "radio/radio.h"
#include "region/region.h"
#include "routing/routing.h"
#include "selftest/selftest.h"
#include "memory_diag/memory_diag.h"
#include "telemetry/telemetry.h"
#include "mesh_hello_nrf.h"
#include "voice_buffers/voice_buffers.h"
#include "voice_frag/voice_frag.h"
#include "x25519_keys/x25519_keys.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

static uint8_t s_rxBuf[PACKET_BUF_SIZE];
static constexpr uint32_t TELEM_INTERVAL_MS = 60000;
static uint32_t s_lastTelemetryMs = 0;
/** Периодический лог в USB CDC: в setup всё уходит одним пакетом; монитор часто открывают позже — в loop видно «живость». */
static constexpr uint32_t SERIAL_HEARTBEAT_MS = 8000;
static uint32_t s_lastSerialHeartbeatMs = 0;

/** USB CDC на nRF часто не принимает данные, пока хост не открыл COM — ранний println «теряется». Дублируем на Serial1 (TX=GPIO6 в variant). */
#if !defined(RIFTLINK_NO_UART1_LOG)
static void uart1_begin() {
  Serial1.begin(115200);
}
static void log_line(const char* s) {
  Serial.println(s);
  Serial.flush();
  Serial1.println(s);
  Serial1.flush();
}
#else
static void uart1_begin() {}
static void log_line(const char* s) {
  Serial.println(s);
  Serial.flush();
}
#endif

/** Как sendMsg в main.cpp ESP: BLE-команда send → msg_queue (SOS → enqueueSos). */
static void sendMsg(const uint8_t* to, const char* text, uint8_t ttlMinutes, bool critical,
    uint8_t triggerType, uint32_t triggerValueMs, bool isSos) {
  if (text == nullptr || strlen(text) == 0) {
    ble::notifyError("send_empty", "Пустое сообщение не отправляется");
    return;
  }
  const bool isBroadcastTo =
      (to != nullptr && memcmp(to, protocol::BROADCAST_ID, protocol::NODE_ID_LEN) == 0);
  const bool ok = isSos ? msg_queue::enqueueSos(text)
                        : msg_queue::enqueue(to, text, ttlMinutes, critical,
                              (msg_queue::TriggerType)triggerType, triggerValueMs);
  if (!ok) {
    const msg_queue::SendFailReason reason = msg_queue::getLastSendFailReason();
    if (!isSos && !isBroadcastTo && to != nullptr && reason == msg_queue::SEND_FAIL_NO_KEY) {
      x25519_keys::sendKeyExchange(to, true, false, "send_wait_key");
      ble::notifyWaitingKey(to);
    } else if (!isSos && !isBroadcastTo && to != nullptr && reason == msg_queue::SEND_FAIL_KEY_BUSY) {
      ble::notifyError("send_key_busy", "Ключ занят, повторите отправку");
    } else if (reason == msg_queue::SEND_FAIL_FRAG_UNAVAILABLE) {
      ble::notifyError("send_too_long", "Сообщение слишком длинное: фрагментация недоступна (очередь или лимиты)");
    } else {
      ble::notifyError("send_queue_busy", "Очередь отправки занята, повторите");
    }
  }
}

/** Как sendLocation в main.cpp ESP: BLE location → OP_LOCATION broadcast. */
static void sendLocation(float lat, float lon, int16_t alt, uint16_t radiusM, uint32_t expiryEpochSec) {
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
    ble::notifyError("location_encrypt", "Шифрование локации не удалось");
    return;
  }

  uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), protocol::BROADCAST_ID, 31,
      protocol::OP_LOCATION, encBuf, encLen, true, false, false);
  if (len > 0) {
    uint8_t txSf = neighbors::rssiToSf(neighbors::getMinRssi());
    char reasonBuf[40];
    if (!queueTxPacket(pkt, len, txSf, false, TxRequestClass::data, reasonBuf, sizeof(reasonBuf))) {
      queueDeferredSend(pkt, len, txSf, 120, true);
    }
  }
}

void setup() {
  Serial.begin(115200);
  uart1_begin();
  delay(300);

  // Пока хост не открёл виртуальный COM, TinyUSB CDC часто отбрасывает вывод — ждём до 15 с, затем первая строка уходит уже в открытый порт.
  uint32_t usb_wait_start = millis();
  while (!Serial && (uint32_t)(millis() - usb_wait_start) < 15000) {
    delay(10);
  }
  uint32_t usb_wait_ms = (uint32_t)(millis() - usb_wait_start);
  {
    char b[140];
    snprintf(b, sizeof(b), "[RiftLink] nRF52840 boot usb_wait_ms=%lu serial_host=%d | no USB text: UART TX GPIO6 115200",
        (unsigned long)usb_wait_ms, Serial ? 1 : 0);
    log_line(b);
  }

  log_line("[RiftLink] init: InternalFS (KV)");
  if (!riftlink_kv::begin()) {
    log_line("[RiftLink] KV init failed (InternalFS недоступен — см. format в kv.cpp / factory erase)");
  } else {
    log_line("[RiftLink] init: InternalFS ok");
  }

  log_line("[RiftLink] init: locale");
  locale::init();
  log_line("[RiftLink] init: locale done");
  log_line("[RiftLink] init: node");
  node::init();
  log_line("[RiftLink] init: node done");
  // crypto::init() (libsodium) — после ble::init(): первый sodium_init() идёт из ble (loadOrGenerateGroupOwnerSigningKey);
  // до SoftDevice/Bluefruit первый вызов sodium_init() на части nRF52 «висит».
  log_line("[RiftLink] init: region");
  region::init();
  log_line("[RiftLink] init: region done");
  pkt_cache::init();
  collision_slots::init();
  network_coding::init();
  msg_queue::init();
  mab::init();
  packet_fusion::init();
  packet_fusion::setOnBatchSent([](const uint8_t* to, const uint32_t* msgIds, int count, uint16_t batchPktId) {
    msg_queue::registerBatchSent(to, msgIds, count, batchPktId);
  });
  packet_fusion::setOnSingleFlush([](const uint8_t* to, uint32_t msgId, const uint8_t* pkt, size_t pktLen, uint8_t txSf) {
    return msg_queue::registerPendingFromFusion(to, msgId, pkt, pktLen, txSf);
  });
  msg_queue::setOnUnicastSent([](const uint8_t* to, uint32_t msgId) { ble::notifySent(to, msgId); });
  msg_queue::setOnUnicastUndelivered([](const uint8_t* to, uint32_t msgId) {
    radio::notifyCongestion();
    ble::notifyUndelivered(to, msgId);
  });
  msg_queue::setOnBroadcastSent([](uint32_t msgId) { ble::notifySent(protocol::BROADCAST_ID, msgId); });
  msg_queue::setOnBroadcastDelivery(
      [](uint32_t msgId, int d, int t) { ble::notifyBroadcastDelivery(msgId, d, t); });
  msg_queue::setOnTimeCapsuleReleased([](const uint8_t* to, uint32_t msgId, uint8_t triggerType) {
    ble::notifyTimeCapsuleReleased(to, msgId, triggerType);
  });
  ble::setOnSend(sendMsg);
  ble::setOnLocation(sendLocation);
  routing::init();
  ack_coalesce::init();
  beacon_sync::init();
  clock_drift::init();
  offline_queue::init();
  groups::init();
  gps::init();
  frag::init();
  voice_frag::init();
  (void)voice_buffers_init();
  telemetry::init();
  log_line("[RiftLink] init: mesh stack done (pkt..telemetry)");
  log_line("[RiftLink] init: radio");
  if (!radio::init()) {
    log_line("[RiftLink] radio init failed");
  } else {
    log_line("[RiftLink] boot: selftest::quickAntennaCheck");
    if (!selftest::quickAntennaCheck()) {
      log_line("[RiftLink] LoRa TX ping failed — проверьте антенну и регион");
    }
  }

  log_line("[RiftLink] init: BLE (Bluefruit)");
  if (!ble::init()) {
    log_line("[RiftLink] BLE init failed");
  }
  log_line("[RiftLink] init: crypto (libsodium, после BLE)");
  if (!crypto::init()) {
    log_line("[RiftLink] crypto init failed");
  }
  log_line("[RiftLink] init: crypto done");
  log_line("[RiftLink] init: x25519_keys");
  x25519_keys::init();
  log_line("[RiftLink] init: x25519_keys done");
  neighbors::init();
  mesh_hello_nrf_init();

  // OLED в конце: на nRF52 Wire без таймаута — неверные SDA/SCL зависают навсегда; до сюда уже видны LoRa/BLE в логе.
#if defined(RIFTLINK_SKIP_OLED_INIT)
  log_line("[RiftLink] OLED skipped (RIFTLINK_SKIP_OLED_INIT)");
#else
  log_line("[RiftLink] init: OLED (I2C) — если дальше нет строк, завис I2C: env faketec_v5 vs heltec_t114 или нет SSD1306");
  if (!display_nrf::init()) {
    log_line("[RiftLink] OLED init failed (проверьте I2C и env)");
  } else {
    display_nrf::show_boot("RiftLink", "nRF52840");
  }
#endif

  log_line("[RiftLink] setup complete");
}

void loop() {
  {
    uint32_t now = millis();
    if ((uint32_t)(now - s_lastSerialHeartbeatMs) >= SERIAL_HEARTBEAT_MS) {
      s_lastSerialHeartbeatMs = now;
      char hb[120];
      snprintf(hb, sizeof(hb), "[RiftLink] heartbeat uptime_s=%lu radio_ok=%d ble=%s", (unsigned long)(now / 1000UL),
          radio::isReady() ? 1 : 0, ble::isConnected() ? "conn" : "adv");
      log_line(hb);
    }
  }

  display_nrf::poll();
  ble::update();
  msg_queue::update();
  routing::update();
  offline_queue::update();
  flushDeferredSends();

  mesh_hello_nrf_loop();

  if (radio::isReady() && !mesh_hello_is_handshake_quiet_active() &&
      (uint32_t)(millis() - s_lastTelemetryMs) >= TELEM_INTERVAL_MS) {
    telemetry::send();
    s_lastTelemetryMs = millis();
  }

  if (!radio::isReady()) {
    delay(200);
    return;
  }

  if (radio::takeMutex(pdMS_TO_TICKS(250))) {
    int n = radio::receive(s_rxBuf, sizeof(s_rxBuf));
    int rssi = radio::getLastRssi();
    uint8_t sf = radio::getSpreadingFactor();
    radio::releaseMutex();
    if (n > 0) {
      RIFTLINK_DIAG("MAIN", "event=RX_RAW len=%d rssi=%d sf=%u", n, rssi, (unsigned)sf);
      handlePacket(s_rxBuf, (size_t)n, rssi, sf);
    }
  }

  if (Serial.available()) {
    Serial.setTimeout(50);
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    auto parseNodeIdHex16 = [](const String& hex, uint8_t out[protocol::NODE_ID_LEN]) -> bool {
      if (hex.length() != (int)(protocol::NODE_ID_LEN * 2)) return false;
      for (int i = 0; i < (int)hex.length(); i++) {
        char c = hex.charAt(i);
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
      }
      for (int i = 0; i < (int)protocol::NODE_ID_LEN; i++) {
        out[i] = (uint8_t)strtoul(hex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
      }
      return true;
    };
    if (cmd.startsWith("send ")) {
      String rest = cmd.substring(5);
      int sp = rest.indexOf(' ');
      String tok1 = sp >= 0 ? rest.substring(0, sp) : rest;
      String text = sp >= 0 ? rest.substring(sp + 1) : "";
      uint8_t to[protocol::NODE_ID_LEN];
      bool isHex16 = parseNodeIdHex16(tok1, to);
      if (isHex16 && text.length() > 0) {
        sendMsg(to, text.c_str(), 0, false, (uint8_t)msg_queue::TRIGGER_NONE, 0, false);
      } else if (rest.length() > 0) {
        sendMsg(protocol::BROADCAST_ID, rest.c_str(), 0, false, (uint8_t)msg_queue::TRIGGER_NONE, 0, false);
      }
    } else if (cmd.startsWith("ping ")) {
      String hex16 = cmd.substring(5);
      hex16.trim();
      uint8_t to[protocol::NODE_ID_LEN];
      if (parseNodeIdHex16(hex16, to)) {
        uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN];
        size_t plen = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), to, 31, protocol::OP_PING, nullptr, 0);
        if (plen > 0) {
          uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(to));
          char reasonBuf[40];
          if (!queueTxPacket(pkt, plen, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
            queueDeferredSend(pkt, plen, txSf, 60, true);
            RIFTLINK_DIAG("PING", "event=PING_TX_DEFER mode=serial to=%02X%02X cause=%s", to[0], to[1],
                reasonBuf[0] ? reasonBuf : "?");
          }
          Serial.printf("[RiftLink] PING sent to %s\n", hex16.c_str());
        }
      } else {
        Serial.println("[RiftLink] ping <hex16>");
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
      String hex16 = cmd.substring(6);
      hex16.trim();
      uint8_t target[protocol::NODE_ID_LEN];
      if (parseNodeIdHex16(hex16, target)) {
        routing::requestRoute(target);
      } else {
        Serial.println("[RiftLink] route <hex16>");
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
    } else if (cmd == "memdiag") {
      memoryDiagLog("serial");
    }
  }

  delay(20);
}
