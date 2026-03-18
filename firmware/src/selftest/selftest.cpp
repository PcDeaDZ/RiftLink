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

void run(Result* out) {
  Result r = {false, false, 0, 0};

  // 1. Radio TX test — PING broadcast
  // Сброс duty cycle для теста (EU 1% мог заблокировать TX)
  duty_cycle::reset();

  uint8_t pkt[protocol::HEADER_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_PING, nullptr, 0);
  r.radioOk = (len > 0 && radio::send(pkt, len));
  Serial.printf("[RiftLink] Selftest Radio TX: %s (region=%s, freq=%.1f)\n",
      r.radioOk ? "OK" : "FAIL", region::getCode(), region::getFreq());

  // 2. Display test — clear, text, show
  displayClear();
  displayText(0, 0, "SELF TEST");
  char buf[32];
  snprintf(buf, sizeof(buf), "Radio: %s", r.radioOk ? "OK" : "FAIL");
  displayText(0, 12, buf);
  displayText(0, 24, "Display: OK");
  displayShow();
  r.displayOk = true;  // если дошли сюда — дисплей отвечает
  Serial.println("[RiftLink] Selftest Display: OK");

  // 3. Battery
  r.batteryMv = telemetry::readBatteryMv();
  Serial.printf("[RiftLink] Selftest Battery: %u mV\n", (unsigned)r.batteryMv);

  // 4. Heap
  r.heapFree = ESP.getFreeHeap();
  Serial.printf("[RiftLink] Selftest Heap: %u bytes\n", (unsigned)r.heapFree);

  Serial.println("[RiftLink] Selftest done");
  if (out) *out = r;
}

}  // namespace selftest
