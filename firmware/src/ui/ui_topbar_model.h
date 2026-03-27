/**
 * Данные для полосы статуса (топ-бар): единая модель, отрисовка — в backend display*.cpp.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace ui_topbar {

/** Сигнал 0..4 по RSSI соседей (как на дисплеях). */
inline int rssiToBars(int rssi) {
  if (rssi >= -75) return 4;
  if (rssi >= -85) return 3;
  if (rssi >= -95) return 2;
  if (rssi >= -105) return 1;
  return 0;
}

struct Model {
  int signalBars;  // 0..4
  bool linkIsBle;
  bool linkConnected;
  char regionModem[28];
  bool hasTime;
  uint8_t hour;
  uint8_t minute;
  int batteryPercent;  // -1 если неизвестно
  bool charging;
};

}  // namespace ui_topbar
