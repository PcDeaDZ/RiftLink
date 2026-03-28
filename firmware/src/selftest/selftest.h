/**
 * RiftLink — самотестирование
 * Проверка радио (TX), дисплея, батареи, памяти
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace selftest {

struct Result {
  bool radioOk;
  bool displayOk;
  bool antennaOk;
  uint16_t batteryMv;
  uint32_t heapFree;
};

/** Запуск тестов. Результат в *out. Выводит в Serial. */
void run(Result* out);

/** Быстрый тест антенны/радио при загрузке. true = ok */
bool quickAntennaCheck();

struct ScanResult {
  uint8_t sf;
  float bw;
  int rssi;
};

/** Auto-scan: перебор SF/BW, слушает эфир. Возвращает количество найденных комбинаций (0 = ничего). */
int modemScan(ScanResult* results, int maxResults);

/**
 * Короткий скан (BW 125 kHz, несколько SF, ~2 с на комбо) — для BLE/nRF без минутной блокировки обработчика.
 * Полный перебор см. modemScan (UI на ESP).
 */
int modemScanQuick(ScanResult* results, int maxResults);

}  // namespace selftest
