/**
 * Differential Clock Drift — предсказание «тихого» момента цикла соседа
 *
 * Модель: сосед передаёт HELLO в начале своего слота. Слот = hash(nodeId) % 16.
 * Цикл = 8с. «Тихий» момент = середина между передачами (0–4с после его TX).
 * phase = (now - lastSeen) % 8000. phase < 4000 → тихо, delay=0. phase >= 4000 → delay = 8000 - phase.
 */

#include "clock_drift.h"
#include "port/rtos_include.h"
#include <Arduino.h>
#include <string.h>

#define MUTEX_TIMEOUT_MS 20

namespace clock_drift {

struct Entry {
  uint8_t id[protocol::NODE_ID_LEN];
  uint32_t lastSeenMillis;
  bool used;
};

static Entry s_entries[MAX_NEIGHBORS];
static bool s_inited = false;
static SemaphoreHandle_t s_mutex = nullptr;

static int findSlot(const uint8_t* nodeId) {
  for (int i = 0; i < MAX_NEIGHBORS; i++) {
    if (s_entries[i].used && memcmp(s_entries[i].id, nodeId, protocol::NODE_ID_LEN) == 0) {
      return i;
    }
  }
  return -1;
}

static int findFreeSlot() {
  for (int i = 0; i < MAX_NEIGHBORS; i++) {
    if (!s_entries[i].used) return i;
  }
  int oldest = 0;
  uint32_t oldestMs = s_entries[0].lastSeenMillis;
  for (int i = 1; i < MAX_NEIGHBORS; i++) {
    if (s_entries[i].lastSeenMillis < oldestMs) {
      oldestMs = s_entries[i].lastSeenMillis;
      oldest = i;
    }
  }
  return oldest;
}

void init() {
  s_mutex = xSemaphoreCreateMutex();
  memset(s_entries, 0, sizeof(s_entries));
  s_inited = true;
}

void onHelloReceived(const uint8_t* from) {
  if (!s_inited || !from || !s_mutex) return;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return;

  int idx = findSlot(from);
  if (idx < 0) idx = findFreeSlot();

  memcpy(s_entries[idx].id, from, protocol::NODE_ID_LEN);
  s_entries[idx].lastSeenMillis = millis();
  s_entries[idx].used = true;
  xSemaphoreGive(s_mutex);
}

uint32_t getQuietWindowMs(const uint8_t* neighborId) {
  if (!s_inited || !neighborId || !s_mutex) return 0;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return 0;

  int idx = findSlot(neighborId);
  if (idx < 0) { xSemaphoreGive(s_mutex); return 0; }

  uint32_t now = millis();
  uint32_t lastSeen = s_entries[idx].lastSeenMillis;
  xSemaphoreGive(s_mutex);

  // Слишком старые данные — не доверяем
  if (now - lastSeen > HELLO_PERIOD_MS * 3) return 0;

  // phase = сколько ms прошло с последнего HELLO в текущем цикле
  uint32_t elapsed = now - lastSeen;
  uint32_t phase = elapsed % HELLO_PERIOD_MS;

  // «Тихий» = 0–4с после его TX (середина между передачами)
  constexpr uint32_t QUIET_HALF_MS = HELLO_PERIOD_MS / 2;  // 4000
  if (phase < QUIET_HALF_MS) {
    return 0;  // уже тихо
  }
  // phase >= 4000: до следующего тихого окна осталось (8000 - phase) ms
  return HELLO_PERIOD_MS - phase;
}

}  // namespace clock_drift
