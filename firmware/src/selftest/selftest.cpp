/**
 * RiftLink — самотестирование
 * Радио: TX PING broadcast. Дисплей: clear + text + show (ESP). Батарея, heap.
 */

#include "selftest.h"
#include "radio/radio.h"
#include "protocol/packet.h"
#include "node/node.h"
#ifndef RIFTLINK_NRF52
#include "ui/display.h"
#else
#include "faketec/display_nrf.h"
#endif
#include "telemetry/telemetry.h"
#include "duty_cycle/duty_cycle.h"
#include <Arduino.h>
#include <cstdio>

#if defined(RIFTLINK_NRF52)
#include "region/region.h"
extern "C" size_t xPortGetFreeHeapSize(void);
#endif

namespace selftest {

namespace {

bool txPingBroadcastSkipCad() {
  if (!radio::isReady()) return false;
  duty_cycle::reset();
  uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_PING,
      nullptr, 0);
  if (len == 0) return false;
  if (!radio::takeMutex(pdMS_TO_TICKS(500))) return false;
  bool ok = radio::sendDirectInternal(pkt, len, nullptr, 0, true);
  radio::releaseMutex();
  return ok;
}

}  // namespace

bool quickAntennaCheck() {
  if (!radio::isReady()) {
    Serial.println("[RiftLink] LoRa TX ping skipped: radio init failed (не SPI дисплея — отдельная шина I2C)");
    return false;
  }
  bool ok = txPingBroadcastSkipCad();
  Serial.printf("[RiftLink] LoRa TX ping (selftest, no CAD): %s\n", ok ? "OK" : "FAIL");
  return ok;
}

void run(Result* out) {
  Result r = {false, false, false, 0, 0};

#if defined(RIFTLINK_NRF52)
  Serial.println("[RiftLink] Selftest.run: start (nRF)");
  Serial.flush();

  if (!display_nrf::is_ready()) {
    Serial.println("[RiftLink] Selftest: OLED not ready, display_nrf::init()...");
    Serial.flush();
    const bool dispInited = display_nrf::init();
    Serial.printf("[RiftLink] Selftest: display_nrf::init -> %s\n", dispInited ? "OK" : "FAIL");
    Serial.flush();
  } else {
    Serial.println("[RiftLink] Selftest: OLED already ready");
    Serial.flush();
  }

  Serial.printf("[RiftLink] Selftest: radio::isReady=%d\n", radio::isReady() ? 1 : 0);
  if (radio::isReady()) {
    Serial.printf("[RiftLink] Selftest: LoRa freq_MHz=%.3f dBm=%d preset=%s SF%u BW%.0f CR4/%u\n",
        (double)region::getFreq(), region::getPower(), radio::modemPresetName(radio::getModemPreset()),
        (unsigned)radio::getSpreadingFactor(), (double)radio::getBandwidth(), (unsigned)radio::getCodingRate());
  }
  Serial.flush();

  const uint8_t* nid = node::getId();
  Serial.printf("[RiftLink] Selftest: node_id=%02X%02X%02X%02X%02X%02X%02X%02X heap_free=%u (before ping)\n", nid[0], nid[1],
      nid[2], nid[3], nid[4], nid[5], nid[6], nid[7], (unsigned)xPortGetFreeHeapSize());
  Serial.println("[RiftLink] Selftest: TX PING broadcast (skip CAD)...");
  Serial.flush();

  r.radioOk = txPingBroadcastSkipCad();
  r.antennaOk = r.radioOk;
  Serial.printf("[RiftLink] Selftest: ping_tx -> %s\n", r.radioOk ? "OK" : "FAIL");
  Serial.flush();

  r.batteryMv = telemetry::readBatteryMv();
  r.heapFree = (uint32_t)xPortGetFreeHeapSize();
  r.displayOk = display_nrf::is_ready();
  if (r.displayOk) display_nrf::show_selftest_summary(r.radioOk, r.antennaOk, r.batteryMv, r.heapFree);

  Serial.printf("[RiftLink] Selftest: summary radio=%s ant=%s bat=%umV heap=%u disp=%s\n", r.radioOk ? "OK" : "FAIL",
      r.antennaOk ? "OK" : "WARN", (unsigned)r.batteryMv, (unsigned)r.heapFree, r.displayOk ? "OK" : "no");
  Serial.printf("[RiftLink] Selftest.run: done (battery ADC — если 0, датчик может быть не подключён на плате)\n");
  Serial.flush();
#else
  duty_cycle::reset();
  uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_PING,
      nullptr, 0);
  r.radioOk = (len > 0 && radio::send(pkt, len));
  r.antennaOk = r.radioOk;

  r.batteryMv = telemetry::readBatteryMv();
  r.heapFree = ESP.getFreeHeap();
  r.displayOk = true;

  displayShowSelftestSummary(r.radioOk, r.antennaOk, r.batteryMv, r.heapFree);

  Serial.printf("[RiftLink] Selftest: radio=%s ant=%s bat=%umV heap=%u\n", r.radioOk ? "OK" : "FAIL",
      r.antennaOk ? "OK" : "WARN", (unsigned)r.batteryMv, (unsigned)r.heapFree);
#endif

  if (out) *out = r;
}

int modemScan(ScanResult* results, int maxResults) {
  if (!results || maxResults <= 0) return 0;

  uint8_t origSf = radio::getSpreadingFactor();
  float origBw = radio::getBandwidth();
  uint8_t origCr = radio::getCodingRate();

  const float bws[] = {125.0f, 250.0f};
  const uint8_t sfs[] = {7, 8, 9, 10, 11, 12};
  const uint32_t LISTEN_MS = 3000;

  int found = 0;

  if (!radio::takeMutex(pdMS_TO_TICKS(2000))) return 0;

  static uint8_t rxBuf[256];

  for (int bi = 0; bi < 2 && found < maxResults; bi++) {
    for (int si = 0; si < 6 && found < maxResults; si++) {
      radio::applyHardwareModem(sfs[si], bws[bi], 5);
      delay(10);

      Serial.printf("[Scan] SF%u BW%.0f ...\n", (unsigned)sfs[si], (double)bws[bi]);

      if (!radio::startReceiveWithTimeout(LISTEN_MS)) {
        Serial.printf("[Scan] startReceive failed\n");
        continue;
      }
      radio::setRxListenActive(true);
      delay(LISTEN_MS);
      int len = radio::receiveAsync(rxBuf, sizeof(rxBuf));
      radio::setRxListenActive(false);
      if (len > 0) {
        results[found].sf = sfs[si];
        results[found].bw = bws[bi];
        results[found].rssi = radio::getLastRssi();
        found++;
        Serial.printf("[Scan] FOUND: SF%u BW%.0f RSSI=%d\n", (unsigned)sfs[si], (double)bws[bi], results[found - 1].rssi);
      }
    }
  }

  radio::applyHardwareModem(origSf, origBw, origCr);
  radio::releaseMutex();

  return found;
}

int modemScanQuick(ScanResult* results, int maxResults) {
  if (!results || maxResults <= 0) return 0;

  uint8_t origSf = radio::getSpreadingFactor();
  float origBw = radio::getBandwidth();
  uint8_t origCr = radio::getCodingRate();

  const float bw = 125.0f;
  const uint8_t sfs[] = {7, 9, 11};
  const uint32_t LISTEN_MS = 650;
  int found = 0;

  if (!radio::takeMutex(pdMS_TO_TICKS(2000))) return 0;

  static uint8_t rxBuf[256];

  for (size_t si = 0; si < sizeof(sfs) / sizeof(sfs[0]) && found < maxResults; si++) {
    radio::applyHardwareModem(sfs[si], bw, 5);
    delay(10);

    Serial.printf("[ScanQuick] SF%u BW125 ...\n", (unsigned)sfs[si]);

    if (!radio::startReceiveWithTimeout(LISTEN_MS)) {
      Serial.printf("[ScanQuick] startReceive failed\n");
      continue;
    }
    radio::setRxListenActive(true);
    delay(LISTEN_MS);
    int len = radio::receiveAsync(rxBuf, sizeof(rxBuf));
    radio::setRxListenActive(false);
    if (len > 0) {
      results[found].sf = sfs[si];
      results[found].bw = bw;
      results[found].rssi = radio::getLastRssi();
      found++;
      Serial.printf("[ScanQuick] FOUND: SF%u RSSI=%d\n", (unsigned)sfs[si], results[found - 1].rssi);
    }
  }

  radio::applyHardwareModem(origSf, origBw, origCr);
  radio::releaseMutex();

  return found;
}

}  // namespace selftest
