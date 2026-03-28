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
#include "telemetry/telemetry.h"
#include "voice_buffers/voice_buffers.h"
#include "voice_frag/voice_frag.h"
#include "x25519_keys/x25519_keys.h"

#include <cstring>

static uint8_t s_rxBuf[PACKET_BUF_SIZE];
static constexpr uint32_t TELEM_INTERVAL_MS = 60000;
static uint32_t s_lastTelemetryMs = 0;

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
  delay(300);
  Serial.println("[RiftLink] nRF52840 boot");

  if (!riftlink_kv::begin()) {
    Serial.println("[RiftLink] KV init failed");
  }
  locale::init();
  node::init();
  if (!crypto::init()) {
    Serial.println("[RiftLink] crypto init failed");
  }
  region::init();
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
  if (!radio::init()) {
    Serial.println("[RiftLink] radio init failed");
  } else {
    Serial.println("[RiftLink] boot: selftest::quickAntennaCheck");
    if (!selftest::quickAntennaCheck()) {
      Serial.println("[RiftLink] LoRa TX ping failed — проверьте антенну и регион");
    }
  }
  x25519_keys::init();
  ble::init();
  neighbors::init();

  Serial.println("[RiftLink] setup complete");
}

void loop() {
  display_nrf::poll();
  ble::update();
  msg_queue::update();
  routing::update();
  offline_queue::update();
  flushDeferredSends();

  if (radio::isReady() && (uint32_t)(millis() - s_lastTelemetryMs) >= TELEM_INTERVAL_MS) {
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

  delay(20);
}
