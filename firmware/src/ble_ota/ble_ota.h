/**
 * BLE OTA — обновление прошивки через BLE GATT.
 * Протокол: JSON-команды + raw binary chunks через NUS TX characteristic.
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace ble_ota {

/** Активен ли BLE OTA приём? */
bool isActive();

/** Начать приём прошивки. size — размер бинарника, md5 — hex-строка MD5. */
bool begin(uint32_t size, const char* md5Hex);

/** Принять чанк данных. Возвращает true если записан успешно. */
bool writeChunk(const uint8_t* data, size_t len);

/** Завершить приём, проверить MD5, переключить boot partition. */
bool end();

/** Отменить OTA. */
void abort();

/** Прогресс: сколько байт записано. */
uint32_t bytesWritten();

}  // namespace ble_ota
