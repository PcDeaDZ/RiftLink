/**
 * Оценка свободной кучи на nRF52 (Arduino Adafruit): newlib malloc + область __HeapBase..__HeapLimit.
 * Не ESP.getFreeHeap — но те же поля heapFree / heapTotal / heapMin для evt:node и логов.
 */

#pragma once

#include <cstdint>

uint32_t heap_metrics_free_bytes();
uint32_t heap_metrics_total_bytes();
/** Минимум свободной кучи с момента первого вызова heap_metrics_free_bytes (аналог ESP.getMinFreeHeap). */
uint32_t heap_metrics_min_free_ever_bytes();
