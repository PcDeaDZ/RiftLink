/**
 * Диагностика internal heap, сводки по RAM-пулам, задачам и частоте CPU.
 * Фаза 0: метрики для анализа фрагментации и OOM (BLE/Wi‑Fi/async).
 */

#pragma once

#include <Arduino.h>

/**
 * Лог в Serial:
 * - intSRAM: heap_tot / free / used / largest / minFreeEver (internal DRAM под malloc);
 * - all_free: ESP.getFreeHeap() (все регионы, при PSRAM может быть больше int free);
 * - tasks: число задач FreeRTOS;
 * - CPU: частота ядра (МГц), не загрузка %;
 * при наличии PSRAM — вторая строка free/total SPIRAM.
 */
void memoryDiagLog(const char* tag);
