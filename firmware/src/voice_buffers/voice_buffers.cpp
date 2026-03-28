#include "voice_buffers.h"
#include "../crypto/crypto.h"
#include "../voice_frag/voice_frag.h"
#include "port/rtos_include.h"
#include <Arduino.h>
#if defined(RIFTLINK_NRF52)
#include <stdlib.h>
#else
#include <esp_heap_caps.h>
#endif
#include <string.h>

static SemaphoreHandle_t s_mutex = nullptr;
static uint8_t* s_pool = nullptr;

static size_t pool_total_bytes() {
  return voice_buffers_plain_cap() + voice_buffers_cipher_cap();
}

bool voice_buffers_init() {
  if (s_mutex) return true;
  s_mutex = xSemaphoreCreateMutex();
  return s_mutex != nullptr;
}

void voice_buffers_deinit() {
  if (s_pool) {
#if defined(RIFTLINK_NRF52)
    free(s_pool);
#else
    heap_caps_free(s_pool);
#endif
    s_pool = nullptr;
  }
  if (s_mutex) {
    vSemaphoreDelete(s_mutex);
    s_mutex = nullptr;
  }
}

static bool ensure_pool() {
  if (s_pool) return true;
  size_t n = pool_total_bytes();
#if defined(RIFTLINK_NRF52)
  s_pool = (uint8_t*)malloc(n);
#else
  s_pool = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
  if (!s_pool) {
    Serial.printf("[RiftLink] voice_buffers: OOM need=%u\n", (unsigned)n);
    return false;
  }
  memset(s_pool, 0, n);
  return true;
}

bool voice_buffers_acquire() {
  if (!voice_buffers_init()) return false;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
  if (!ensure_pool()) {
    xSemaphoreGive(s_mutex);
    return false;
  }
  return true;
}

void voice_buffers_release() {
  if (s_mutex) xSemaphoreGive(s_mutex);
}

size_t voice_buffers_plain_cap() {
  return voice_frag::MAX_VOICE_PLAIN + 1024;
}

size_t voice_buffers_cipher_cap() {
  return voice_frag::MAX_VOICE_PLAIN + 1024 + crypto::OVERHEAD;
}

uint8_t* voice_buffers_plain() {
  return s_pool;
}

uint8_t* voice_buffers_cipher() {
  return s_pool + voice_buffers_plain_cap();
}
