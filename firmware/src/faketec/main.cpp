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
#include "board_pins.h"
#include "voice_buffers/voice_buffers.h"
#include "voice_frag/voice_frag.h"
#include "x25519_keys/x25519_keys.h"
#include "version.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

#if __has_include(<nrf_sdh.h>)
#include <nrf_sdh.h>
#define RIFTLINK_HAS_NRF_SDH 1
#else
#define RIFTLINK_HAS_NRF_SDH 0
#endif

extern "C" __attribute__((weak)) size_t xPortGetFreeHeapSize(void);

static uint32_t nrf_heap_kb() {
  size_t f = xPortGetFreeHeapSize();
#if RIFTLINK_HAS_NRF_SDH
  uint32_t sd = nrf_sdh_get_free_heap_size();
  if (sd > f) f = sd;
#endif
  return (uint32_t)(f / 1024U);
}

static void nrf_system_reset_now() {
  Serial.flush();
  delay(30);
  __DSB();
  *((volatile uint32_t*)0xE000ED0C) = 0x5FA0004U;
  __DSB();
  for (;;) {
  }
}

static int parse_modem_preset_arg(const String& arg) {
  String t = arg;
  t.trim();
  t.toLowerCase();
  if (t.length() == 0) return -1;
  if (t.length() == 1 && t[0] >= '0' && t[0] <= '3') return t[0] - '0';
  if (t == "speed" || t == "spaid") return 0;
  if (t == "normal") return 1;
  if (t == "range") return 2;
  if (t == "maxrange" || t == "max_range") return 3;
  return -1;
}

static uint16_t s_serialDiagPktId = 1;

/** Паритет с BLE `signalTest`: PING всем соседям (до 8). */
static void nrf_serial_signal_test() {
  int n = neighbors::getCount();
  if (n == 0) {
    Serial.println("[RiftLink] signalTest: no neighbors");
    return;
  }
  int sent = 0;
  const int nMax = (n > 8) ? 8 : n;
  for (int i = 0; i < nMax; i++) {
    uint8_t peerId[protocol::NODE_ID_LEN];
    if (!neighbors::getId(i, peerId)) continue;
    uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID + 64];
    uint16_t pktId = ++s_serialDiagPktId;
    size_t plen = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), peerId, 31, protocol::OP_PING, nullptr, 0, false,
        false, false, protocol::CHANNEL_DEFAULT, pktId);
    if (plen > 0) {
      uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(peerId));
      char reasonBuf[40];
      uint32_t delayMs = 140u + (uint32_t)(i * 220u) + (uint32_t)(random(90));
      if (!queueTxPacket(pkt, plen, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
        queueDeferredSend(pkt, plen, txSf, delayMs, true);
      }
      sent++;
    }
  }
  Serial.printf("[RiftLink] signalTest: queued %d ping(s) (neighbors=%d)\n", sent, n);
}

/** Несколько экранов статуса на ST7789/OLED (кнопка T114 или Serial `status`/`dash`). */
static uint8_t s_statusDashPage = 0;

static void nrf_refresh_status_dashboard(uint8_t page) {
  if (!display_nrf::is_ready()) return;
  if (page > 2) page = 0;
  char l1[32], l2[32], l3[40], l4[40];
  const uint8_t* nid = node::getId();
  char idshort[12];
  snprintf(idshort, sizeof(idshort), "%02X%02X%02X%02X", nid[0], nid[1], nid[2], nid[3]);
  if (page == 0) {
    snprintf(l1, sizeof(l1), "RiftLink %s", RIFTLINK_VERSION);
    snprintf(l2, sizeof(l2), "ID %s..", idshort);
    snprintf(l3, sizeof(l3), "%s %.2f ch%d", region::getCode(), (double)region::getFreq(), region::getChannel());
    snprintf(l4, sizeof(l4), "SF%u n=%d %s", (unsigned)radio::getSpreadingFactor(), neighbors::getCount(),
        radio::modemPresetName(radio::getModemPreset()));
  } else if (page == 1) {
    snprintf(l1, sizeof(l1), "Mesh / BLE");
    snprintf(l2, sizeof(l2), "n=%d", neighbors::getCount());
    snprintf(l3, sizeof(l3), "%s", ble::isConnected() ? "BLE connected" : "BLE advertising");
    if (neighbors::getCount() <= 0) {
      snprintf(l4, sizeof(l4), "minRSSI --");
    } else {
      int mr = neighbors::getMinRssi();
      snprintf(l4, sizeof(l4), "minRSSI %d", mr < 0 ? mr : -120);
    }
  } else {
    uint16_t mv = telemetry::readBatteryMv();
    snprintf(l1, sizeof(l1), "Power / time");
    snprintf(l2, sizeof(l2), "Bat %umV", (unsigned)mv);
    snprintf(l3, sizeof(l3), "heap %u kB", (unsigned)nrf_heap_kb());
    snprintf(l4, sizeof(l4), "up %lus", (unsigned long)(millis() / 1000UL));
  }
  display_nrf::show_status_screen(l1, l2, l3, l4);
}

static uint8_t s_rxBuf[PACKET_BUF_SIZE];
static constexpr uint32_t TELEM_INTERVAL_MS = 60000;
static uint32_t s_lastTelemetryMs = 0;
/** Периодический лог в USB CDC: в setup всё уходит одним пакетом; монитор часто открывают позже — в loop видно «живость». */
static constexpr uint32_t SERIAL_HEARTBEAT_MS = 8000;
static uint32_t s_lastSerialHeartbeatMs = 0;
/** Паритет с ESP `main.cpp`: периодический KEY_EXCHANGE соседям без ключа (плотная сеть / редкий mesh). */
static uint32_t s_lastKeyRetry = 0;
static int s_keyRetryRoundRobin = 0;
static uint32_t s_keyRetryCooldownUntil = 0;
static uint32_t s_lastDiagSnapshotMs = 0;
/** Интервалы KEY retry — те же, что в ESP `main.cpp`. */
static constexpr uint32_t KEY_RETRY_BASE_MS = 45000;
static constexpr uint32_t KEY_RETRY_JITTER_MS = 15000;
static constexpr uint32_t KEY_RETRY_EXTRA_JITTER_MS = 3000;
static constexpr uint32_t KEY_RETRY_SMALL_MESH_BASE_MS = 12000;
static constexpr uint32_t KEY_RETRY_SMALL_MESH_JITTER_MS = 4000;
static constexpr uint32_t KEY_RETRY_SMALL_MESH_EXTRA_JITTER_MS = 1500;
#if defined(RIFTLINK_BOARD_HELTEC_T114)
static bool s_t114BtnPrev = false;
#endif

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
  s_lastKeyRetry = millis() + (node::getId()[0] % 16) * 500;

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

#if defined(RIFTLINK_BOARD_HELTEC_T114)
  pinMode(T114_LED_PIN, OUTPUT);
  digitalWrite(T114_LED_PIN, T114_LED_ON);
  pinMode(T114_BUTTON_PIN, INPUT_PULLUP);
  log_line("[RiftLink] T114: LED/button pins (Meshtastic variant)");
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

  // Паритет с ESP: периодический retry KEY_EXCHANGE (см. main.cpp KEY_RETRY_*).
  if (millis() >= s_lastKeyRetry && millis() >= s_keyRetryCooldownUntil) {
    int n = neighbors::getCount();
    const bool smallMesh = (n <= 2);
    uint32_t base = smallMesh ? KEY_RETRY_SMALL_MESH_BASE_MS : KEY_RETRY_BASE_MS;
    uint32_t jitSpan = smallMesh ? KEY_RETRY_SMALL_MESH_JITTER_MS : KEY_RETRY_JITTER_MS;
    uint32_t extraSpan = smallMesh ? KEY_RETRY_SMALL_MESH_EXTRA_JITTER_MS : KEY_RETRY_EXTRA_JITTER_MS;
    uint32_t phase = smallMesh ? ((node::getId()[0] % 8) * 200u) : ((node::getId()[0] % 16) * 500u);
    uint32_t extraJitter = (uint32_t)random((long)extraSpan);
    s_lastKeyRetry = millis() + base + (uint32_t)random((long)jitSpan) + phase + extraJitter;
    RIFTLINK_DIAG("KEY", "event=KEY_RETRY_TICK neighbors=%d next_retry_in_ms=%lu cooldown_until=%lu small_mesh=%u", n,
        (unsigned long)(s_lastKeyRetry - millis()), (unsigned long)s_keyRetryCooldownUntil, (unsigned)smallMesh);
    if (n > 0) {
      s_keyRetryRoundRobin %= n;
      for (int k = 0; k < n; k++) {
        int i = (s_keyRetryRoundRobin + k) % n;
        uint8_t peerId[protocol::NODE_ID_LEN];
        if (neighbors::getId(i, peerId) && !x25519_keys::hasKeyFor(peerId)) {
          RIFTLINK_DIAG("KEY", "event=KEY_RETRY_TARGET peer=%02X%02X idx=%d rr=%d", peerId[0], peerId[1], i,
              s_keyRetryRoundRobin);
          x25519_keys::sendKeyExchange(peerId, false, false, "retry");
          s_keyRetryRoundRobin = (i + 1) % n;
          s_keyRetryCooldownUntil = millis() + 800 + (uint32_t)random(400);
          break;
        }
      }
    }
  }

  if (millis() - s_lastDiagSnapshotMs >= 60000) {
    s_lastDiagSnapshotMs = millis();
    RIFTLINK_DIAG("STATE",
        "event=MODEM_SNAPSHOT region=%s freq_mhz=%.1f sf=%u bw=%.1f cr=%u neighbors=%d ps_enabled=%u ps_can=%u",
        region::getCode(), region::getFreq(), (unsigned)radio::getSpreadingFactor(), radio::getBandwidth(),
        (unsigned)radio::getCodingRate(), neighbors::getCount(), 0u, 0u);
  }

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
        bool pingSent = false;
        if (plen > 0) {
          uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(to));
          char reasonBuf[40];
          if (queueTxPacket(pkt, plen, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
            pingSent = true;
          } else {
            queueDeferredSend(pkt, plen, txSf, 60, true);
            RIFTLINK_DIAG("PING", "event=PING_TX_DEFER mode=serial to=%02X%02X cause=%s", to[0], to[1],
                reasonBuf[0] ? reasonBuf : "?");
            pingSent = true;
          }
        }
        if (pingSent) {
          Serial.printf("[RiftLink] PING sent to %s\n", hex16.c_str());
        } else {
          Serial.println("[RiftLink] PING failed");
        }
      } else {
        Serial.println("[RiftLink] ping <hex16>");
      }
    } else if (cmd == "region") {
      Serial.printf("[RiftLink] Region: %s (%.1f MHz) channel=%d\n", region::getCode(), (double)region::getFreq(),
          region::getChannel());
    } else if (cmd.startsWith("region ")) {
      String r = cmd.substring(7);
      r.trim();
      if (region::setRegion(r.c_str())) {
        Serial.printf("[RiftLink] Region: %s (%.1f MHz)\n", region::getCode(), region::getFreq());
      } else {
        Serial.println("[RiftLink] Region: EU|UK|RU|US|AU");
      }
    } else if (cmd == "channel") {
      if (region::getChannelCount() > 0) {
        Serial.printf("[RiftLink] Channel: %d (%.1f MHz)\n", region::getChannel(), (double)region::getFreq());
      } else {
        Serial.println("[RiftLink] channel: только EU/UK (см. region)");
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
        Serial.println("[RiftLink] route: request sent");
      } else {
        Serial.println("[RiftLink] route <hex16>");
      }
    } else if (cmd.startsWith("traceroute ")) {
      String hex16 = cmd.substring(11);
      hex16.trim();
      uint8_t target[protocol::NODE_ID_LEN];
      if (parseNodeIdHex16(hex16, target)) {
        routing::requestRoute(target);
        Serial.println("[RiftLink] traceroute: request sent (same as route)");
      } else {
        Serial.println("[RiftLink] traceroute <hex16>");
      }
    } else if (cmd.startsWith("read ")) {
      String rest = cmd.substring(5);
      rest.trim();
      int sp = rest.indexOf(' ');
      if (sp < 0) {
        Serial.println("[RiftLink] read <hex16> <msgId>");
      } else {
        String hex16 = rest.substring(0, sp);
        uint32_t msgId = (uint32_t)rest.substring(sp + 1).toInt();
        hex16.trim();
        uint8_t to[protocol::NODE_ID_LEN];
        if (!parseNodeIdHex16(hex16, to) || msgId == 0) {
          Serial.println("[RiftLink] read <hex16> <msgId>");
        } else {
          uint8_t payload[4];
          memcpy(payload, &msgId, 4);
          uint8_t pkt[protocol::PAYLOAD_OFFSET + 4];
          size_t pktLen =
              protocol::buildPacket(pkt, sizeof(pkt), node::getId(), to, 31, protocol::OP_READ, payload, 4, false, false);
          if (pktLen > 0) {
            uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(to));
            char reasonBuf[40];
            if (!queueTxPacket(pkt, pktLen, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
              queueDeferredSend(pkt, pktLen, txSf, 60 + (uint32_t)(random(40)), true);
            }
            Serial.printf("[RiftLink] READ sent msgId=%lu\n", (unsigned long)msgId);
          } else {
            Serial.println("[RiftLink] READ build failed");
          }
        }
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
    } else if (cmd == "bat" || cmd == "battery") {
      uint16_t mv = telemetry::readBatteryMv();
      int pct = telemetry::batteryPercent();
      Serial.printf("[RiftLink] Battery: %umV", (unsigned)mv);
      if (pct >= 0) Serial.printf(" ~%d%%", pct);
      Serial.printf(" charging=%s\n", telemetry::isCharging() ? "yes" : "no");
    } else if (cmd == "node" || cmd == "id") {
      const uint8_t* nid = node::getId();
      char hex[protocol::NODE_ID_LEN * 2 + 1];
      for (int i = 0; i < (int)protocol::NODE_ID_LEN; i++) {
        snprintf(hex + i * 2, 3, "%02X", nid[i]);
      }
      char nick[20];
      node::getNickname(nick, sizeof(nick));
      Serial.printf("[RiftLink] node_id=%s nickname=%s\n", hex, nick);
    } else if (cmd == "info") {
      const uint8_t* nid = node::getId();
      char idfull[protocol::NODE_ID_LEN * 2 + 1];
      for (int i = 0; i < (int)protocol::NODE_ID_LEN; i++) {
        snprintf(idfull + i * 2, 3, "%02X", nid[i]);
      }
      char nick[20];
      node::getNickname(nick, sizeof(nick));
      Serial.printf("[RiftLink] info id=%s nick=%s\n", idfull, nick);
      Serial.printf("[RiftLink]   region=%s %.2fMHz ch=%d tx=%ddBm\n", region::getCode(), (double)region::getFreq(),
          region::getChannel(), region::getPower());
      Serial.printf("[RiftLink]   modem=%s SF%u bw=%.0f cr=%u\n", radio::modemPresetName(radio::getModemPreset()),
          (unsigned)radio::getSpreadingFactor(), (double)radio::getBandwidth(), (unsigned)radio::getCodingRate());
      Serial.printf("[RiftLink]   ble=%s neighbors=%d\n", ble::isConnected() ? "connected" : "advertising",
          neighbors::getCount());
    } else if (cmd.startsWith("espnow ")) {
      Serial.println("[RiftLink] ESP-NOW не на nRF52840 (см. docs/API.md, Wi‑Fi не используется)");
    } else if (cmd == "espnow") {
      Serial.println("[RiftLink] ESP-NOW не на nRF52840");
    } else if (cmd == "wifi" || cmd.startsWith("wifi ")) {
      Serial.println("[RiftLink] Wi‑Fi не на nRF52840 (см. docs/API.md)");
    } else if (cmd == "ota" || cmd.startsWith("ota ")) {
      Serial.println("[RiftLink] OTA по Wi‑Fi AP не на nRF; прошивка: DFU/USB — docs/flasher/NRF52.md");
    } else if (cmd == "ws" || cmd == "websocket" || cmd.startsWith("websocket")) {
      Serial.println("[RiftLink] WebSocket/Web OTA на nRF не реализовано");
    } else if (cmd == "neighbors" || cmd == "nb") {
      int n = neighbors::getCount();
      Serial.printf("[RiftLink] neighbors: %d\n", n);
      for (int i = 0; i < n; i++) {
        uint8_t id[protocol::NODE_ID_LEN];
        char hex[24];
        if (!neighbors::getId(i, id)) continue;
        neighbors::getIdHex(i, hex);
        const bool hasKey = x25519_keys::hasKeyFor(id);
        const int batMv = neighbors::getBatteryMv(id);
        Serial.printf("  %d: %s rssi=%d key=%s bat_mv=%d\n", i, hex, neighbors::getRssi(i), hasKey ? "yes" : "no",
            batMv);
      }
    } else if (cmd == "gps") {
      int rx = 0, tx = 0, en = 0;
      gps::getPins(&rx, &tx, &en);
      Serial.printf("[RiftLink] GPS: %s %s rx=%d tx=%d en=%d\n", gps::isPresent() ? "present" : "absent",
          gps::isEnabled() ? "on" : "off", rx, tx, en);
      if (gps::hasFix()) {
        Serial.printf("[RiftLink] Fix: %.5f, %.5f\n", (double)gps::getLat(), (double)gps::getLon());
      }
    } else if (cmd == "gps on") {
      gps::setEnabled(true);
      Serial.println("[RiftLink] GPS on");
    } else if (cmd == "gps off") {
      gps::setEnabled(false);
      Serial.println("[RiftLink] GPS off");
    } else if (cmd.startsWith("gps pins ")) {
      int rx = -1, tx = -1, en = -1;
      int n = sscanf(cmd.c_str() + 9, "%d %d %d", &rx, &tx, &en);
      if (n >= 2) {
        if (n < 3) en = -1;
        gps::setPins(rx, tx, en);
        gps::saveConfig();
        Serial.printf("[RiftLink] GPS pins rx=%d tx=%d en=%d\n", rx, tx, en);
      } else {
        Serial.println("[RiftLink] gps pins <rx> <tx> [en]");
      }
    } else if (cmd == "selftest" || cmd == "test") {
      selftest::Result r;
      selftest::run(&r);
    } else if (cmd == "sf" || cmd == "radio") {
      Serial.printf("[RiftLink] SF=%u, %.1f MHz, bw=%.1f kHz, cr=%u, neighbors=%d\n",
          (unsigned)radio::getSpreadingFactor(), (double)region::getFreq(), (double)radio::getBandwidth(),
          (unsigned)radio::getCodingRate(), neighbors::getCount());
    } else if (cmd == "version" || cmd == "ver") {
      Serial.printf("[RiftLink] RiftLink %s nRF52840 build " __DATE__ " " __TIME__ "\n", RIFTLINK_VERSION);
    } else if (cmd == "uptime") {
      Serial.printf("[RiftLink] uptime_s=%lu\n", (unsigned long)(millis() / 1000UL));
    } else if (cmd == "modemscan" || cmd == "modemscan quick") {
      selftest::ScanResult sr[8];
      int n = selftest::modemScanQuick(sr, 8);
      Serial.printf("[RiftLink] modemScanQuick: found %d\n", n);
    } else if (cmd == "modemscan full") {
      selftest::ScanResult sr[16];
      int n = selftest::modemScan(sr, 16);
      Serial.printf("[RiftLink] modemScan: found %d\n", n);
    } else if (cmd.startsWith("modemCustom ")) {
      int sf = -1, cr = -1;
      float bw = -1.0f;
      int nscan = sscanf(cmd.c_str() + 12, "%d %f %d", &sf, &bw, &cr);
      if (nscan == 3 && sf >= 7 && sf <= 12 && bw > 0 && cr >= 5 && cr <= 8) {
        if (radio::requestCustomModem((uint8_t)sf, bw, (uint8_t)cr)) {
          Serial.printf("[RiftLink] modemCustom: SF%u BW%.1f CR%u\n", (unsigned)sf, (double)bw, (unsigned)cr);
        } else {
          Serial.println("[RiftLink] modemCustom: radio busy");
        }
      } else {
        Serial.println("[RiftLink] modemCustom <sf 7-12> <bw_kHz> <cr 5-8>");
      }
    } else if (cmd == "modem" || cmd.startsWith("modem ")) {
      if (cmd.length() <= 5) {
        Serial.printf("[RiftLink] modem: %s SF%u bw=%.0f cr=%u\n", radio::modemPresetName(radio::getModemPreset()),
            (unsigned)radio::getSpreadingFactor(), (double)radio::getBandwidth(), (unsigned)radio::getCodingRate());
      } else {
        int p = parse_modem_preset_arg(cmd.substring(6));
        if (p >= 0 && p < 4) {
          if (radio::requestModemPreset((radio::ModemPreset)p)) {
            Serial.printf("[RiftLink] modem preset %d OK\n", p);
          } else {
            Serial.println("[RiftLink] modem: radio busy");
          }
        } else {
          Serial.println("[RiftLink] modem [0..3|speed|normal|range|maxrange]");
        }
      }
    } else if (cmd.startsWith("sf ")) {
      int sf = cmd.substring(3).toInt();
      if (sf >= 7 && sf <= 12) {
        if (radio::requestSpreadingFactor((uint8_t)sf)) {
          Serial.printf("[RiftLink] SF=%u OK\n", (unsigned)sf);
        } else {
          Serial.println("[RiftLink] sf: radio busy");
        }
      } else {
        Serial.println("[RiftLink] sf <7..12>");
      }
    } else if (cmd == "loraScan") {
      selftest::ScanResult sr[8];
      int n = selftest::modemScanQuick(sr, 8);
      Serial.printf("[RiftLink] loraScan: found %d (BLE parity)\n", n);
    } else if (cmd == "signalTest") {
      nrf_serial_signal_test();
    } else if (cmd == "powersave") {
      Serial.printf("[RiftLink] Power save: не на nRF52840 (BLE %s)\n", ble::isConnected() ? "connected" : "disconnected");
    } else if (cmd == "powersave on" || cmd == "powersave off") {
      Serial.println("[RiftLink] powersave on/off: на nRF52840 не реализовано (см. docs/API.md)");
    } else if (cmd.startsWith("powersave ")) {
      Serial.println("[RiftLink] powersave: на nRF52840 не реализовано (см. docs/API.md, evt:error powersave_unsupported)");
    } else if (cmd == "status" || cmd.startsWith("status ")) {
      int pg = 0;
      if (cmd.startsWith("status ")) pg = cmd.substring(7).toInt();
      if (pg < 0 || pg > 2) pg = 0;
      s_statusDashPage = (uint8_t)pg;
      nrf_refresh_status_dashboard(s_statusDashPage);
      Serial.printf("[RiftLink] status page %u (0=RF 1=mesh 2=bat)\n", (unsigned)s_statusDashPage);
    } else if (cmd == "dash") {
      s_statusDashPage = 0;
      nrf_refresh_status_dashboard(0);
      Serial.println("[RiftLink] status page 0");
    } else if (cmd == "reboot" || cmd == "rst") {
      Serial.println("[RiftLink] reboot...");
      nrf_system_reset_now();
    } else if (cmd == "help" || cmd == "?") {
      Serial.println(
          "[RiftLink] Serial: send|ping|info|node|region|channel|nickname|route|traceroute|read|lang|memdiag|bat|"
          "neighbors|gps|selftest|sf|sf N|modem|modemCustom|modemscan|loraScan|signalTest|powersave|espnow|wifi|ota|ws|"
          "status|dash|reboot|version|uptime|help");
    }
  }

#if defined(RIFTLINK_BOARD_HELTEC_T114)
  {
    bool pressed = digitalRead(T114_BUTTON_PIN) == LOW;
    if (pressed && !s_t114BtnPrev) {
      Serial.println("[RiftLink] T114 button: next screen");
      s_statusDashPage = (uint8_t)((s_statusDashPage + 1) % 3);
      nrf_refresh_status_dashboard(s_statusDashPage);
    }
    s_t114BtnPrev = pressed;
  }
#endif

  delay(20);
}
