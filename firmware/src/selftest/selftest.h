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
  uint16_t batteryMv;
  uint32_t heapFree;
};

/** Запуск тестов. Результат в *out. Выводит в Serial. */
void run(Result* out);

}  // namespace selftest
