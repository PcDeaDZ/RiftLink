/**
 * Свободная RAM кучи: mallinfo().fordblks + «хвост» от sbrk(0) до __HeapLimit (см. cores/nRF5/new.cpp).
 */

#include "heap_metrics.h"
#include <malloc.h>
#include <stdint.h>
#include <unistd.h>

extern "C" {
extern unsigned char __HeapBase[];
extern unsigned char __HeapLimit[];
}

static uint32_t s_minFreeEver = 0xFFFFFFFFu;

static uint32_t heap_tail_bytes() {
  void* brk = sbrk(0);
  if (brk == nullptr) return 0;
  const uintptr_t lim = (uintptr_t)__HeapLimit;
  const uintptr_t top = (uintptr_t)brk;
  if (top >= lim) return 0;
  return (uint32_t)(lim - top);
}

uint32_t heap_metrics_total_bytes() {
  return (uint32_t)((uintptr_t)__HeapLimit - (uintptr_t)__HeapBase);
}

uint32_t heap_metrics_free_bytes() {
  struct mallinfo mi = mallinfo();
  uint32_t ford = 0;
  if (mi.arena > 0) ford = (uint32_t)mi.fordblks;
  const uint32_t tail = heap_tail_bytes();
  const uint32_t free = ford + tail;
  if (s_minFreeEver == 0xFFFFFFFFu || free < s_minFreeEver) s_minFreeEver = free;
  return free;
}

uint32_t heap_metrics_min_free_ever_bytes() {
  if (s_minFreeEver == 0xFFFFFFFFu) (void)heap_metrics_free_bytes();
  return s_minFreeEver;
}
