#include "ui_topbar_fill.h"
#include "region_modem_fmt.h"
#include "ble/ble.h"
#include "gps/gps.h"
#include "neighbors/neighbors.h"
#include "radio_mode/radio_mode.h"
#include "telemetry/telemetry.h"
#include "wifi/wifi.h"
#include <cstring>

namespace ui_topbar {

void fill(Model& m) {
  int n = neighbors::getCount();
  int avgRssi = neighbors::getAverageRssi();
  m.signalBars = rssiToBars(n > 0 ? avgRssi : -120);
  m.linkIsBle = (radio_mode::current() == radio_mode::BLE);
  m.linkConnected = m.linkIsBle ? ble::isConnected() : wifi::isConnected();
  m.regionModem[0] = '\0';
  ui_fmt::regionModemShort(m.regionModem, sizeof(m.regionModem));
  m.hasTime = gps::hasTime();
  if (m.hasTime) {
    m.hour = (uint8_t)gps::getLocalHour();
    m.minute = (uint8_t)gps::getLocalMinute();
  } else {
    m.hour = 0;
    m.minute = 0;
  }
  m.batteryPercent = telemetry::batteryPercent();
  m.charging = telemetry::isCharging();
}

}  // namespace ui_topbar
