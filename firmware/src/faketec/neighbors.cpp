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
  uint16_t batteryMv;
  uint16_t ackSent;
  uint16_t ackRecv;
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

void updateBattery(const uint8_t* nodeId, uint16_t batteryMv) {
  if (!nodeId || batteryMv == 0 || !s_inited) return;
  int i = find(nodeId);
  if (i < 0) return;
  s_entries[i].batteryMv = batteryMv;
  s_entries[i].lastSeen = millis();
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

int getRssiFor(const uint8_t* nodeId) {
  if (!nodeId) return -128;
  int i = find(nodeId);
  if (i < 0) return -128;
  if ((millis() - s_entries[i].lastSeen) >= NEIGHBOR_TIMEOUT_MS) return -128;
  return s_entries[i].rssi;
}

int getMinRssi() {
  uint32_t now = millis();
  int minR = 0;
  bool any = false;
  for (int i = 0; i < NEIGHBORS_MAX; i++) {
    if (!s_entries[i].valid || (now - s_entries[i].lastSeen) >= NEIGHBOR_TIMEOUT_MS) continue;
    if (s_entries[i].rssi != 0) {
      if (!any || s_entries[i].rssi < minR) {
        minR = s_entries[i].rssi;
        any = true;
      }
    }
  }
  return any ? minR : 0;
}

int getAverageRssi() {
  uint32_t now = millis();
  int sum = 0, n = 0;
  for (int i = 0; i < NEIGHBORS_MAX; i++) {
    if (!s_entries[i].valid || (now - s_entries[i].lastSeen) >= NEIGHBOR_TIMEOUT_MS) continue;
    if (s_entries[i].rssi != 0) {
      sum += s_entries[i].rssi;
      n++;
    }
  }
  return n == 0 ? -90 : (sum / n);
}

bool isOnline(const uint8_t* nodeId) {
  if (!nodeId) return false;
  int i = find(nodeId);
  if (i < 0) return false;
  return (millis() - s_entries[i].lastSeen) < NEIGHBOR_TIMEOUT_MS;
}

uint32_t getFreshnessMs(const uint8_t* nodeId) {
  if (!nodeId) return UINT32_MAX;
  int i = find(nodeId);
  if (i < 0) return UINT32_MAX;
  uint32_t now = millis();
  if ((now - s_entries[i].lastSeen) >= NEIGHBOR_TIMEOUT_MS) return UINT32_MAX;
  return now - s_entries[i].lastSeen;
}

int getBatteryMv(const uint8_t* nodeId) {
  if (!nodeId) return 0;
  int i = find(nodeId);
  if (i < 0) return 0;
  return s_entries[i].batteryMv;
}

void recordAckSent(const uint8_t* nodeId) {
  int i = find(nodeId);
  if (i < 0) return;
  if (s_entries[i].ackSent < 65535) s_entries[i].ackSent++;
}

void recordAckReceived(const uint8_t* nodeId) {
  int i = find(nodeId);
  if (i < 0) return;
  if (s_entries[i].ackRecv < 65535) s_entries[i].ackRecv++;
}

int getAckRatePermille(const uint8_t* nodeId) {
  int i = find(nodeId);
  if (i < 0) return -1;
  uint32_t sent = s_entries[i].ackSent;
  uint32_t recv = s_entries[i].ackRecv;
  if (sent == 0 && recv == 0) return -1;
  if (sent == 0) return 1000;
  if (recv > sent) recv = sent;
  return (int)((recv * 1000) / sent);
}

uint8_t rssiToSfOrthogonal(const uint8_t* nodeId) {
  if (!nodeId) return 0;
  int r = getRssiFor(nodeId);
  uint8_t sfRssi = rssiToSf(r);
  if (getCount() < 2) return sfRssi ? sfRssi : 12;
  uint32_t h = 0;
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) h = h * 31u + nodeId[i];
  uint8_t sfOrtho = (uint8_t)(7 + (h % 6u));
  return (sfRssi > 0 && sfRssi > sfOrtho) ? sfRssi : sfOrtho;
}

}  // namespace neighbors
