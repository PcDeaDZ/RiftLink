/**
 * ptr_pool.h — Pre-allocated fixed-size pool for pointer-based FreeRTOS queues.
 *
 * Instead of copying large structs (228+ bytes) through xQueueSend critical
 * sections, allocate from a pool and pass 4-byte pointers through the queue.
 * Critical section drops from ~228 bytes to 4 bytes (~57x reduction).
 *
 * SPSC/MPSC safe: alloc/free use a FreeRTOS queue internally as a free-list
 * (xQueueSend/Receive of pointers — 4 bytes, negligible critical section).
 *
 * Хранилище элементов: при наличии PSRAM (Heltec V4, LilyGO T-Beam и др.) —
 * SPIRAM, иначе internal. Паритет с heap_caps_malloc_extmem_enable в main:
 * крупные буферы не забирают внутренний heap под NimBLE/Wi‑Fi. Указатели в
 * free-list остаются во internal (очередь FreeRTOS).
 */

#pragma once

#include "port/rtos_include.h"
#include <string.h>
#if defined(RIFTLINK_NRF52)
#include <stdlib.h>
#else
#include <esp_heap_caps.h>
#endif

template <typename T, size_t Capacity>
class PtrPool {
 public:
  PtrPool() = default;

  bool init() {
    if (freeList_) return true;
    const size_t nbytes = sizeof(T) * Capacity;
    storage_ = nullptr;
#if defined(RIFTLINK_NRF52)
    storage_ = static_cast<T*>(malloc(nbytes));
    storage_spiram_ = false;
#else
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
      storage_ = static_cast<T*>(heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!storage_) {
      storage_ = static_cast<T*>(heap_caps_malloc(nbytes, MALLOC_CAP_INTERNAL));
      storage_spiram_ = false;
    } else {
      storage_spiram_ = true;
    }
#endif
    if (!storage_) return false;
    memset(storage_, 0, sizeof(T) * Capacity);
    freeList_ = xQueueCreate(Capacity, sizeof(T*));
    if (!freeList_) {
#if defined(RIFTLINK_NRF52)
      free(storage_);
#else
      heap_caps_free(storage_);
#endif
      storage_ = nullptr;
      return false;
    }
    for (size_t i = 0; i < Capacity; i++) {
      T* p = &storage_[i];
      xQueueSend(freeList_, &p, 0);
    }
    return true;
  }

  T* alloc(TickType_t wait = 0) {
    T* p = nullptr;
    if (freeList_ && xQueueReceive(freeList_, &p, wait) == pdTRUE) return p;
    return nullptr;
  }

  void free(T* p) {
    if (!freeList_ || !p) return;
    xQueueSend(freeList_, &p, 0);
  }

  size_t available() const {
    return freeList_ ? (size_t)uxQueueMessagesWaiting(freeList_) : 0;
  }

  bool ready() const { return freeList_ != nullptr; }

  /** true если массив элементов выделен в SPIRAM (PSRAM), иначе internal DRAM */
  bool storageInSpiram() const { return storage_spiram_; }

 private:
  T* storage_ = nullptr;
  bool storage_spiram_ = false;
  QueueHandle_t freeList_ = nullptr;
};
