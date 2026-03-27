/**
 * Порты подсистем V3 для nRF (без ESP-only API): beacon_sync, collision_slots, clock_drift, mab, voice_frag.
 */

#include "mab/mab.h"
#include "collision_slots/collision_slots.h"
#include "beacon_sync/beacon_sync.h"
#include "clock_drift/clock_drift.h"
#include "voice_frag/voice_frag.h"
#include "node.h"
#include "protocol/packet.h"
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

namespace mab {

static float s_sum[NUM_ARMS];
static uint32_t s_count[NUM_ARMS];
static uint32_t s_totalPulls = 0;
static constexpr float EPSILON = 0.2f;
static const uint32_t s_delayMin[NUM_ARMS] = {50, 200, 400, 700};
static const uint32_t s_delayMax[NUM_ARMS] = {150, 350, 600, 1000};

void init() {
  memset(s_sum, 0, sizeof(s_sum));
  memset(s_count, 0, sizeof(s_count));
  s_totalPulls = 0;
}

int selectAction() {
  for (int i = 0; i < NUM_ARMS; i++) {
    if (s_count[i] == 0) return i;
  }
  if ((random() % 100) < (int)(EPSILON * 100)) {
    return random() % NUM_ARMS;
  }
  float bestMean = -1e9f;
  int best = 0;
  for (int i = 0; i < NUM_ARMS; i++) {
    float mean = s_sum[i] / (float)s_count[i];
    if (mean > bestMean) {
      bestMean = mean;
      best = i;
    }
  }
  return best;
}

uint32_t getDelayMs(int action) {
  if (action < 0 || action >= NUM_ARMS) action = 1;
  uint32_t range = s_delayMax[action] - s_delayMin[action];
  return s_delayMin[action] + (uint32_t)(random() % (int)(range + 1));
}

void reward(int action, int rewardVal) {
  if (action < 0 || action >= NUM_ARMS) return;
  s_sum[action] += (float)rewardVal;
  s_count[action]++;
  s_totalPulls++;
}

}  // namespace mab

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
  if (minVal == 65535) return 0;
  uint32_t now = (uint32_t)millis();
  int curSlot = (int)((now / SLOT_MS) % N_SLOTS);
  int delta = (minIdx - curSlot + N_SLOTS) % N_SLOTS;
  if (delta == 0) return (uint32_t)(random() % 300);
  uint32_t delay = (uint32_t)delta * SLOT_MS + (uint32_t)(random() % 500);
  return delay > 15000 ? 15000 : delay;
}

}  // namespace collision_slots

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
      if (s_neighborSlots[j] == (uint8_t)s) {
        busy = true;
        break;
      }
    }
    if (!busy && s != (int)mySlot) return (uint32_t)i * SLOT_MS + (uint32_t)(random() % 200);
  }
  return (uint32_t)(random() % 300);
}

}  // namespace beacon_sync

namespace clock_drift {

struct Entry {
  uint8_t id[protocol::NODE_ID_LEN];
  uint32_t lastSeenMillis;
  bool used;
};

static Entry s_entries[MAX_NEIGHBORS];
static bool s_inited = false;

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
  memset(s_entries, 0, sizeof(s_entries));
  s_inited = true;
}

void onHelloReceived(const uint8_t* from) {
  if (!s_inited || !from) return;
  int idx = findSlot(from);
  if (idx < 0) idx = findFreeSlot();
  memcpy(s_entries[idx].id, from, protocol::NODE_ID_LEN);
  s_entries[idx].lastSeenMillis = millis();
  s_entries[idx].used = true;
}

uint32_t getQuietWindowMs(const uint8_t* neighborId) {
  if (!s_inited || !neighborId) return 0;
  int idx = findSlot(neighborId);
  if (idx < 0) return 0;
  uint32_t now = millis();
  uint32_t lastSeen = s_entries[idx].lastSeenMillis;
  if (now - lastSeen > HELLO_PERIOD_MS * 3) return 0;
  uint32_t elapsed = now - lastSeen;
  uint32_t phase = elapsed % HELLO_PERIOD_MS;
  constexpr uint32_t QUIET_HALF_MS = HELLO_PERIOD_MS / 2;
  if (phase < QUIET_HALF_MS) return 0;
  return HELLO_PERIOD_MS - phase;
}

}  // namespace clock_drift

namespace voice_frag {

void init() {}
void deinit() {}

bool send(const uint8_t*, const uint8_t*, size_t, uint32_t*) { return false; }

bool onFragment(const uint8_t*, const uint8_t*, const uint8_t*, size_t, uint8_t*, size_t, size_t*, uint32_t*) {
  return false;
}

bool matchAck(const uint8_t*, uint32_t) { return false; }

}  // namespace voice_frag
