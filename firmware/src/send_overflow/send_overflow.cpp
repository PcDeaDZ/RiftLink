/**
 * send_overflow — буфер при полной sendQueue.
 * Pull on-demand, приоритеты для ACK.
 */

#include "send_overflow.h"
#include <Arduino.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define PRIORITY_SLOTS 4   // ACK — обслуживаются первыми
#define NORMAL_SLOTS 8     // MSG, KEY_EXCHANGE и т.п.
#define MUTEX_TIMEOUT_MS 50

struct OverflowSlot {
  uint8_t buf[PACKET_BUF_SIZE];
  uint16_t len;
  uint8_t txSf;
  bool used;
};

static OverflowSlot s_priority[PRIORITY_SLOTS];
static OverflowSlot s_normal[NORMAL_SLOTS];
static uint8_t s_priHead = 0, s_priTail = 0, s_priCount = 0;
static uint8_t s_normHead = 0, s_normTail = 0, s_normCount = 0;
static SemaphoreHandle_t s_mutex = nullptr;
static bool s_inited = false;

namespace send_overflow {

void init() {
  if (s_inited) return;
  s_mutex = xSemaphoreCreateMutex();
  memset(s_priority, 0, sizeof(s_priority));
  memset(s_normal, 0, sizeof(s_normal));
  s_inited = true;
}

bool push(const uint8_t* buf, size_t len, uint8_t txSf, bool priority) {
  if (!s_inited || !buf || len > PACKET_BUF_SIZE) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;

  OverflowSlot* arr = priority ? s_priority : s_normal;
  int cap = priority ? PRIORITY_SLOTS : NORMAL_SLOTS;
  uint8_t* tail = priority ? &s_priTail : &s_normTail;
  uint8_t* count = priority ? &s_priCount : &s_normCount;

  if (*count >= cap) {
    xSemaphoreGive(s_mutex);
    return false;
  }

  OverflowSlot* slot = &arr[*tail];
  memcpy(slot->buf, buf, len);
  slot->len = (uint16_t)len;
  slot->txSf = (txSf >= 7 && txSf <= 12) ? txSf : 0;
  slot->used = true;
  *tail = (*tail + 1) % cap;
  (*count)++;
  xSemaphoreGive(s_mutex);
  return true;
}

bool pop(SendQueueItem* item) {
  if (!s_inited || !item) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;

  OverflowSlot* slot = nullptr;
  uint8_t* head = nullptr;
  int cap = 0;

  if (s_priCount > 0) {
    slot = &s_priority[s_priHead];
    head = &s_priHead;
    cap = PRIORITY_SLOTS;
    s_priCount--;
  } else if (s_normCount > 0) {
    slot = &s_normal[s_normHead];
    head = &s_normHead;
    cap = NORMAL_SLOTS;
    s_normCount--;
  }

  if (!slot) {
    xSemaphoreGive(s_mutex);
    return false;
  }

  memcpy(item->buf, slot->buf, slot->len);
  item->len = slot->len;
  item->txSf = slot->txSf;
  slot->used = false;
  *head = (*head + 1) % cap;
  xSemaphoreGive(s_mutex);
  return true;
}

bool getNextTxPacket(SendQueueItem* item) {
  if (!item) return false;
  if (sendQueue && xQueueReceive(sendQueue, item, 0) == pdTRUE) return true;
  return pop(item);
}

}  // namespace send_overflow
