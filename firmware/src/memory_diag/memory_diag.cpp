/**
 * Реализация memory_diag — internal DRAM, сравнение с общим free heap, PSRAM, задачи, CPU MHz.
 */

#include "memory_diag.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

void memoryDiagLog(const char* tag) {
  const char* t = tag ? tag : "?";

  size_t totInt = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  if (totInt == 0) {
    totInt = ESP.getHeapSize();
  }
  size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  size_t usedInt = (totInt >= freeInt) ? (totInt - freeInt) : 0;

  uint32_t minEver = ESP.getMinFreeHeap();
  uint32_t freeAll = ESP.getFreeHeap();
  uint32_t cpuMhz = ESP.getCpuFreqMHz();
  UBaseType_t nTasks = uxTaskGetNumberOfTasks();

  Serial.printf(
      "[RiftLink] MemDiag[%s] intSRAM: heap_tot=%u free=%u used=%u largest=%u minFreeEver=%u | "
      "all_heap_free=%u | tasks=%u | CPU=%uMHz\n",
      t, (unsigned)totInt, (unsigned)freeInt, (unsigned)usedInt, (unsigned)largest, (unsigned)minEver,
      (unsigned)freeAll, (unsigned)nTasks, (unsigned)cpuMhz);

  size_t psramTot = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (psramTot > 0) {
    size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psramUsed = (psramTot >= psramFree) ? (psramTot - psramFree) : 0;
    Serial.printf("[RiftLink] MemDiag[%s] PSRAM: free=%u used=%u total=%u\n", t, (unsigned)psramFree,
        (unsigned)psramUsed, (unsigned)psramTot);
  }
}
