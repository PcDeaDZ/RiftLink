/**
 * RiftLink — телеметрия (батарея, память)
 * OP_TELEMETRY, broadcast
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace telemetry {

// Payload: battery_mV (uint16), heapKb (uint16) = 4 bytes
constexpr size_t TELEM_PAYLOAD_LEN = 4;

void init();
void send();  // Отправка broadcast TELEMETRY

// Чтение батареи (mV), 0 если недоступно
uint16_t readBatteryMv();

/**
 * Зарядка: на ESP32 с USB Serial JTAG — только usb_serial_jtag (U>4.2 В давало ложный CHG на полной АКБ).
 * Без JTAG-драйвера — эвристика U>4.2 V. T‑Pager: BQ; T‑Beam: PMU. T114: U>4.2 V.
 */
bool isCharging();

/** Нижняя граница шкалы 0% (мВ): как дефолтный минимум OCV в Meshtastic power.h */
constexpr uint16_t kBatteryPctMvMin = 3100;
/** Верхняя граница 100% (мВ), типичный потолок 1S Li-ion */
constexpr uint16_t kBatteryPctMvMax = 4200;
constexpr int kBatteryPctMvSpan = static_cast<int>(kBatteryPctMvMax) - static_cast<int>(kBatteryPctMvMin);

/**
 * Процент заряда Li-ion по напряжению: kBatteryPctMvMin (0%) … kBatteryPctMvMax (100%), линейно с округлением.
 * Ниже 2.5 В — -1 (батарея не определена / не подключена).
 */
inline int batteryPercentFromMv(uint16_t mv) {
  if (mv < 2500) return -1;
  if (mv >= kBatteryPctMvMax) return 100;
  if (mv <= kBatteryPctMvMin) return 0;
  const int numer =
      (static_cast<int>(mv) - static_cast<int>(kBatteryPctMvMin)) * 100 + (kBatteryPctMvSpan / 2);
  int p = numer / kBatteryPctMvSpan;
  if (p > 100) p = 100;
  return p;
}

/**
 * Процент (0–100), -1 если батарея не определена.
 * На ESP32-S3 с USB-JTAG: при подключённом USB или U>4.2 В возвращается последний
 * процент «только от батареи», чтобы не завышать SOC из-за зарядника на том же делителе.
 */
int batteryPercent();

}  // namespace telemetry
