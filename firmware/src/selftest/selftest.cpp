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
#endif
#include "telemetry/telemetry.h"
#include "duty_cycle/duty_cycle.h"
#include <Arduino.h>
#include <cstdio>

#if defined(RIFTLINK_NRF52)
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
  r.radioOk = txPingBroadcastSkipCad();
  r.antennaOk = r.radioOk;
  r.batteryMv = telemetry::readBatteryMv();
  r.heapFree = (uint32_t)xPortGetFreeHeapSize();
  r.displayOk = false;
  Serial.printf("[RiftLink] Selftest: radio=%s ant=%s bat=%umV heap=%u\n", r.radioOk ? "OK" : "FAIL",
      r.antennaOk ? "OK" : "WARN", (unsigned)r.batteryMv, (unsigned)r.heapFree);
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
