/**
 * BQ27220 — fuel gauge по I2C (LilyGO T-Lora Pager, общая шина SDA=3/SCL=2).
 * Адрес 7-bit 0x55 (ADDR к GND). Команды — см. TI SLUUDG0B (Standard Commands).
 */

#pragma once

#include <Arduino.h>
#include <cstdint>

namespace bq27220_tpager {

/** Проба устройства на шине (после Wire.begin). */
bool probe();

/** Напряжение на батарее, мВ; 0 при ошибке. */
uint16_t readVoltageMv();

/** Относительный заряд 0–100%; -1 при ошибке/нет данных. */
int readRelativeSocPercent();

/** Средний ток, мА (+ заряд, − разряд). 0 при ошибке. */
int16_t readAverageCurrentMa();

/** Зарядка: положительный средний ток или флаги (если доступны). */
bool isCharging();

}  // namespace bq27220_tpager
