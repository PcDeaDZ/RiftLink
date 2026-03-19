/**
 * FakeTech Neighbors
 */

#include "neighbors.h"
#include <Arduino.h>
#include <string.h>

struct Entry {
  uint8_t id[protocol::NODE_ID_LEN];
  int rssi;
  uint32_t lastSeen;
  bool valid;
};

static Entry s_entries[NEIGHBORS_MAX];
static bool s_inited = false;

namespace neighbors {

void init() {
  if (s_inited) return;
  memset(s_entries, 0, sizeof(s_entries));
  s_inited = true;
}

static int find(const uint8_t* nodeId) {
  for (int i = 0; i < NEIGHBORS_MAX; i++) {
    if (s_entries[i].valid && memcmp(s_entries[i].id, nodeId, protocol::NODE_ID_LEN) == 0)
      return i;
  }
  return -1;
}

static int alloc() {
  uint32_t now = millis();
  for (int i = 0; i < NEIGHBORS_MAX; i++) {
    if (!s_entries[i].valid) return i;
    if (now - s_entries[i].lastSeen > NEIGHBOR_TIMEOUT_MS) return i;
  }
  return -1;
}

bool onHello(const uint8_t* nodeId, int rssi) {
  if (!s_inited) return false;
  int i = find(nodeId);
  if (i >= 0) {
    s_entries[i].rssi = rssi;
    s_entries[i].lastSeen = millis();
    return false;
  }
  i = alloc();
  if (i < 0) return false;
  memcpy(s_entries[i].id, nodeId, protocol::NODE_ID_LEN);
  s_entries[i].rssi = rssi;
  s_entries[i].lastSeen = millis();
  s_entries[i].valid = true;
  return true;
}

void updateRssi(const uint8_t* nodeId, int rssi) {
  int i = find(nodeId);
  if (i >= 0) s_entries[i].rssi = rssi;
}

int getCount() {
  if (!s_inited) return 0;
  uint32_t now = millis();
  int n = 0;
  for (int i = 0; i < NEIGHBORS_MAX; i++) {
    if (s_entries[i].valid && (now - s_entries[i].lastSeen) < NEIGHBOR_TIMEOUT_MS)
      n++;
  }
  return n;
}

int getRssi(int i) {
  if (i < 0 || i >= NEIGHBORS_MAX) return 0;
  if (!s_entries[i].valid) return 0;
  if ((millis() - s_entries[i].lastSeen) >= NEIGHBOR_TIMEOUT_MS) return 0;
  return s_entries[i].rssi;
}

bool getId(int i, uint8_t* out) {
  if (!out || i < 0 || i >= NEIGHBORS_MAX) return false;
  if (!s_entries[i].valid) return false;
  if ((millis() - s_entries[i].lastSeen) >= NEIGHBOR_TIMEOUT_MS) return false;
  memcpy(out, s_entries[i].id, protocol::NODE_ID_LEN);
  return true;
}

}  // namespace neighbors
