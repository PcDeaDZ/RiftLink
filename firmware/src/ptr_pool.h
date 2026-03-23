/**
 * ptr_pool.h — Pre-allocated fixed-size pool for pointer-based FreeRTOS queues.
 *
 * Instead of copying large structs (228+ bytes) through xQueueSend critical
 * sections, allocate from a pool and pass 4-byte pointers through the queue.
 * Critical section drops from ~228 bytes to 4 bytes (~57x reduction).
 *
 * SPSC/MPSC safe: alloc/free use a FreeRTOS queue internally as a free-list
 * (xQueueSend/Receive of pointers — 4 bytes, negligible critical section).
 */

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include <esp_heap_caps.h>

template <typename T, size_t Capacity>
class PtrPool {
 public:
  PtrPool() = default;

  bool init() {
    if (freeList_) return true;
    storage_ = static_cast<T*>(heap_caps_malloc(sizeof(T) * Capacity, MALLOC_CAP_INTERNAL));
    if (!storage_) return false;
    memset(storage_, 0, sizeof(T) * Capacity);
    freeList_ = xQueueCreate(Capacity, sizeof(T*));
    if (!freeList_) {
      heap_caps_free(storage_);
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

 private:
  T* storage_ = nullptr;
  QueueHandle_t freeList_ = nullptr;
};
