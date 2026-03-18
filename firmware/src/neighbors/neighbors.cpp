/**
 * RiftLink Neighbors — список узлов по HELLO
 */

#include "neighbors.h"
#include "node/node.h"
#include <Arduino.h>
#include <string.h>

struct Entry {
  uint8_t id[protocol::NODE_ID_LEN];
  uint32_t lastSeenMs;
  int8_t lastRssi;  // dBm, 0 = unknown
  bool used;
};

static Entry s_entries[NEIGHBORS_MAX];
static bool s_inited = false;

namespace neighbors {

void init() {
  if (s_inited) return;
  memset(s_entries, 0, sizeof(s_entries));
  s_inited = true;
}

static int findSlot(const uint8_t* nodeId) {
  for (int i = 0; i < NEIGHBORS_MAX; i++) {
    if (s_entries[i].used && memcmp(s_entries[i].id, nodeId, protocol::NODE_ID_LEN) == 0) {
      return i;
    }
  }
  return -1;
}

static int findFreeSlot() {
  for (int i = 0; i < NEIGHBORS_MAX; i++) {
    if (!s_entries[i].used) return i;
  }
  // Вытесняем самого старого
  int oldest = 0;
  uint32_t oldestMs = s_entries[0].lastSeenMs;
  for (int i = 1; i < NEIGHBORS_MAX; i++) {
    if (s_entries[i].lastSeenMs < oldestMs) {
      oldestMs = s_entries[i].lastSeenMs;
      oldest = i;
    }
  }
  return oldest;
}

bool onHello(const uint8_t* nodeId, int rssi) {
  if (!nodeId || node::isForMe(nodeId)) return false;

  int idx = findSlot(nodeId);
  bool wasNew = (idx < 0);
  if (idx < 0) idx = findFreeSlot();

  memcpy(s_entries[idx].id, nodeId, protocol::NODE_ID_LEN);
  s_entries[idx].lastSeenMs = millis();
  s_entries[idx].lastRssi = (rssi >= -128 && rssi <= 0) ? (int8_t)rssi : 0;
  s_entries[idx].used = true;
  return wasNew;
}

void updateRssi(const uint8_t* nodeId, int rssi) {
  if (!nodeId || (rssi < -128 || rssi > 0)) return;
  int idx = findSlot(nodeId);
  if (idx >= 0) s_entries[idx].lastRssi = (int8_t)rssi;
}

int getRssi(int i) {
  if (i < 0) return 0;
  uint32_t now = millis();
  int idx = 0;
  for (int j = 0; j < NEIGHBORS_MAX; j++) {
    if (!s_entries[j].used || (now - s_entries[j].lastSeenMs) >= NEIGHBOR_TIMEOUT_MS) continue;
    if (idx == i) return s_entries[j].lastRssi;
    idx++;
  }
  return 0;
}

int getAverageRssi() {
  uint32_t now = millis();
  int sum = 0;
  int n = 0;
  for (int i = 0; i < NEIGHBORS_MAX; i++) {
    if (!s_entries[i].used || (now - s_entries[i].lastSeenMs) >= NEIGHBOR_TIMEOUT_MS) continue;
    if (s_entries[i].lastRssi != 0) {
      sum += s_entries[i].lastRssi;
      n++;
    }
  }
  if (n == 0) return -90;  // нет данных — нейтральное значение
  return sum / n;
}

int getCount() {
  uint32_t now = millis();
  int n = 0;
  for (int i = 0; i < NEIGHBORS_MAX; i++) {
    if (s_entries[i].used && (now - s_entries[i].lastSeenMs) < NEIGHBOR_TIMEOUT_MS) {
      n++;
    }
  }
  return n;
}

bool getId(int i, uint8_t* out) {
  if (!out || i < 0) return false;
  uint32_t now = millis();
  int idx = 0;
  for (int j = 0; j < NEIGHBORS_MAX; j++) {
    if (!s_entries[j].used || (now - s_entries[j].lastSeenMs) >= NEIGHBOR_TIMEOUT_MS) continue;
    if (idx == i) {
      memcpy(out, s_entries[j].id, protocol::NODE_ID_LEN);
      return true;
    }
    idx++;
  }
  return false;
}

void getIdHex(int i, char* out) {
  if (!out) return;
  uint8_t id[protocol::NODE_ID_LEN];
  if (!getId(i, id)) {
    out[0] = '\0';
    return;
  }
  for (size_t j = 0; j < protocol::NODE_ID_LEN; j++) {
    snprintf(out + j * 2, 3, "%02X", id[j]);
  }
  out[protocol::NODE_ID_LEN * 2] = '\0';
}

}  // namespace neighbors
