/**
 * Beacon-sync — слот по hash, избежание слотов соседей
 */

#include "beacon_sync.h"
#include "node/node.h"
#include <Arduino.h>
#include "neighbors/neighbors.h"
#if !defined(RIFTLINK_NRF52)
#include <esp_random.h>
#endif
#include <string.h>

#if defined(RIFTLINK_NRF52)
static uint32_t jitter200() {
  return (uint32_t)(random(200));
}
static uint32_t jitter300() {
  return (uint32_t)(random(300));
}
#endif

namespace beacon_sync {

constexpr int N_SLOTS = 16;
constexpr uint32_t SLOT_MS = 500;
static uint8_t s_neighborSlots[16];
static int s_neighborCount = 0;
static bool s_inited = false;

void init() {
  memset(s_neighborSlots, 0xFF, sizeof(s_neighborSlots));
  s_neighborCount = 0;
  s_inited = true;
}

uint8_t getSlotFor(const uint8_t* nodeId) {
  if (!nodeId) return 0;
  uint32_t h = 0;
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) h = h * 31 + nodeId[i];
  return (uint8_t)(h % N_SLOTS);
}

void onBeaconReceived(const uint8_t* from) {
  if (!s_inited || !from || s_neighborCount >= 16) return;
  uint8_t slot = getSlotFor(from);
  for (int i = 0; i < s_neighborCount; i++) {
    if (s_neighborSlots[i] == slot) return;
  }
  s_neighborSlots[s_neighborCount++] = slot;
  if (s_neighborCount >= 16) s_neighborCount = 0;
}

uint32_t getAvoidanceDelayMs() {
  if (!s_inited || s_neighborCount == 0) return 0;
  uint8_t mySlot = getSlotFor(node::getId());
  uint32_t now = (uint32_t)millis();
  int curSlot = (int)((now / SLOT_MS) % N_SLOTS);
  for (int i = 0; i < N_SLOTS; i++) {
    int s = (curSlot + i) % N_SLOTS;
    bool busy = false;
    for (int j = 0; j < s_neighborCount; j++) {
      if (s_neighborSlots[j] == (uint8_t)s) { busy = true; break; }
    }
    if (!busy && s != mySlot) {
#if defined(RIFTLINK_NRF52)
      return (uint32_t)i * SLOT_MS + jitter200();
#else
      return (uint32_t)i * SLOT_MS + (esp_random() % 200);
#endif
    }
  }
#if defined(RIFTLINK_NRF52)
  return jitter300();
#else
  return (uint32_t)(esp_random() % 300);
#endif
}

}  // namespace beacon_sync
