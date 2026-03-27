/**
 * RiftLink — самотестирование
 * Радио: TX PING broadcast. Дисплей: clear + text + show. Батарея, heap.
 */

#include "selftest.h"
#include "radio/radio.h"
#include "protocol/packet.h"
#include "node/node.h"
#include "ui/display.h"
#include "telemetry/telemetry.h"
#include "duty_cycle/duty_cycle.h"
#include "region/region.h"
#include <Arduino.h>
#include <cstdio>

namespace selftest {

bool quickAntennaCheck() {
  duty_cycle::reset();
  uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_PING, nullptr, 0);
  if (len == 0) return false;
  // При загрузке async-очередь ещё не создана — radio::send() вернёт false.
  // Используем прямой SPI TX под мьютексом (radioSchedulerTask не запущен, конкуренции нет).
  if (!radio::takeMutex(pdMS_TO_TICKS(500))) return false;
  bool ok = radio::sendDirectInternal(pkt, len);
  radio::releaseMutex();
  Serial.printf("[RiftLink] Antenna check: %s\n", ok ? "OK" : "FAIL");
  return ok;
}

void run(Result* out) {
  Result r = {false, false, false, 0, 0};

  // 1. Radio TX
  duty_cycle::reset();
  uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_PING, nullptr, 0);
  r.radioOk = (len > 0 && radio::send(pkt, len));
  r.antennaOk = r.radioOk;

  // 2. Battery + Heap
  r.batteryMv = telemetry::readBatteryMv();
  r.heapFree = ESP.getFreeHeap();
  r.displayOk = true;

  // 3. Show all results (стиль как у подменю)
  displayShowSelftestSummary(r.radioOk, r.antennaOk, r.batteryMv, r.heapFree);

  Serial.printf("[RiftLink] Selftest: radio=%s ant=%s bat=%umV heap=%u\n",
      r.radioOk ? "OK" : "FAIL", r.antennaOk ? "OK" : "WARN",
      (unsigned)r.batteryMv, (unsigned)r.heapFree);

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
        Serial.printf("[Scan] FOUND: SF%u BW%.0f RSSI=%d\n",
            (unsigned)sfs[si], (double)bws[bi], results[found - 1].rssi);
      }
    }
  }

  radio::applyHardwareModem(origSf, origBw, origCr);
  radio::releaseMutex();

  return found;
}

}  // namespace selftest
