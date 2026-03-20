/**
 * Power Save — light sleep с пробуждением по DIO1 (LoRa) и таймеру
 * Включается при отключённом BLE для экономии батареи
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace powersave {

/** Инициализация (вызов после radio::init) */
void init();

/** Включён ли power save (light sleep при BLE отключён). По умолчанию true. */
bool isEnabled();
void setEnabled(bool on);

/** Можно ли использовать light sleep (включён, BLE отключён, OTA не активен) */
bool canSleep();

/**
 * Только light sleep (без SPI). Перед вызовом: startReceive уже выполнен, mutex радио отпущен.
 */
void lightSleepWake();

/** Макс. время сна в мкс (1 сек) */
constexpr uint64_t SLEEP_TIMEOUT_US = 1000000;

/** DIO1 = GPIO 14 на Heltec V3/V4 */
constexpr int DIO1_GPIO = 14;

}  // namespace powersave
