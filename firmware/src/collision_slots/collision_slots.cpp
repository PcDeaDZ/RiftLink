/**
 * Predictive Slot Avoidance — argmin collision_times
 */

#include "collision_slots.h"
#include <Arduino.h>
#include <string.h>

#if defined(RIFTLINK_NRF52)
static uint32_t collisionRandU32() {
  return (uint32_t)random();
}
#else
#include <esp_random.h>
static uint32_t collisionRandU32() {
  return esp_random();
}
#endif

namespace collision_slots {

static uint16_t s_collisionTimes[N_SLOTS];
static bool s_inited = false;

void init() {
  memset(s_collisionTimes, 0, sizeof(s_collisionTimes));
  s_inited = true;
}

void recordCollision() {
  if (!s_inited) return;
  uint32_t now = (uint32_t)millis();
  int slot = (int)((now / SLOT_MS) % N_SLOTS);
  if (s_collisionTimes[slot] < 65535) s_collisionTimes[slot]++;
}

uint32_t getAvoidanceDelayMs() {
  if (!s_inited) return 0;
  uint16_t minVal = 65535;
  int minIdx = 0;
  for (int i = 0; i < N_SLOTS; i++) {
    if (s_collisionTimes[i] < minVal) {
      minVal = s_collisionTimes[i];
      minIdx = i;
    }
  }
  if (minVal == 65535) return 0;  // нет коллизий — не смещать
  uint32_t now = (uint32_t)millis();
  int curSlot = (int)((now / SLOT_MS) % N_SLOTS);
  int delta = (minIdx - curSlot + N_SLOTS) % N_SLOTS;
  if (delta == 0) return (uint32_t)(collisionRandU32() % 300);  // небольшой jitter
  uint32_t delay = (uint32_t)delta * SLOT_MS + (collisionRandU32() % 500);
  return delay > 15000 ? 15000 : delay;  // cap 15 s
}

}  // namespace collision_slots
