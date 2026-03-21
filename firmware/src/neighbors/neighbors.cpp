/**
 * RiftLink Neighbors — список узлов по HELLO
 */

#include "neighbors.h"
#include "node/node.h"
#include "log.h"
#include <Arduino.h>
#include <string.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define MUTEX_TIMEOUT_MS 100

static SemaphoreHandle_t s_mutex = nullptr;

struct Entry {
  uint8_t id[protocol::NODE_ID_LEN];
  uint32_t lastSeenMs;
  uint32_t lastDiagMs;
  int8_t lastRssi;  // dBm, 0 = unknown
  uint16_t batteryMv;
  uint16_t ackSent;
  uint16_t ackReceived;
  bool online;
  bool used;
};

static Entry s_entries[NEIGHBORS_MAX];
struct EntrySnapshot {
  uint8_t id[protocol::NODE_ID_LEN];
  uint32_t lastSeenMs;
  int8_t lastRssi;
  uint16_t batteryMv;
  uint16_t ackSent;
  uint16_t ackReceived;
  bool used;
};
static EntrySnapshot s_snapshot[NEIGHBORS_MAX];
static std::atomic<uint32_t> s_snapshotSeq{0};  // even=stable, odd=writer
static bool s_inited = false;

namespace neighbors {

static void publishSnapshotLocked() {
  s_snapshotSeq.fetch_add(1, std::memory_order_release);  // writer begin
  for (int i = 0; i < NEIGHBORS_MAX; i++) {
    s_snapshot[i].used = s_entries[i].used;
    if (!s_entries[i].used) continue;
    memcpy(s_snapshot[i].id, s_entries[i].id, protocol::NODE_ID_LEN);
    s_snapshot[i].lastSeenMs = s_entries[i].lastSeenMs;
    s_snapshot[i].lastRssi = s_entries[i].lastRssi;
    s_snapshot[i].batteryMv = s_entries[i].batteryMv;
    s_snapshot[i].ackSent = s_entries[i].ackSent;
    s_snapshot[i].ackReceived = s_entries[i].ackReceived;
  }
  s_snapshotSeq.fetch_add(1, std::memory_order_release);  // writer end
}

void init() {
  if (s_inited) return;
  s_mutex = xSemaphoreCreateMutex();
  memset(s_entries, 0, sizeof(s_entries));
  memset(s_snapshot, 0, sizeof(s_snapshot));
  s_snapshotSeq.store(0, std::memory_order_relaxed);
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
  if (!nodeId || node::isForMe(nodeId) || node::isSameShortId(nodeId) || node::isInvalidNodeId(nodeId)) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;

  int idx = findSlot(nodeId);
  bool wasNew = (idx < 0);
  if (idx < 0) {
    idx = findFreeSlot();
  }

  memcpy(s_entries[idx].id, nodeId, protocol::NODE_ID_LEN);
  s_entries[idx].lastSeenMs = millis();
  s_entries[idx].lastRssi = (rssi >= -128 && rssi <= 0) ? (int8_t)rssi : 0;
  bool wasOnline = s_entries[idx].online;
  s_entries[idx].online = true;
  s_entries[idx].used = true;
  if (wasNew) {
    RIFTLINK_DIAG("NEIGH", "event=NEIGHBOR_ONLINE peer=%02X%02X rssi=%d",
        nodeId[0], nodeId[1], rssi);
    s_entries[idx].lastDiagMs = s_entries[idx].lastSeenMs;
  } else if (!wasOnline) {
    RIFTLINK_DIAG("NEIGH", "event=NEIGHBOR_BACK_ONLINE peer=%02X%02X rssi=%d",
        nodeId[0], nodeId[1], rssi);
    s_entries[idx].lastDiagMs = s_entries[idx].lastSeenMs;
  } else if ((s_entries[idx].lastSeenMs - s_entries[idx].lastDiagMs) >= 15000) {
    RIFTLINK_DIAG("NEIGH", "event=NEIGHBOR_REFRESH peer=%02X%02X rssi=%d age_ms=0",
        nodeId[0], nodeId[1], rssi);
    s_entries[idx].lastDiagMs = s_entries[idx].lastSeenMs;
  }
  publishSnapshotLocked();
  xSemaphoreGive(s_mutex);
  return wasNew;
}

void updateRssi(const uint8_t* nodeId, int rssi) {
  if (!nodeId || (rssi < -128 || rssi > 0)) return;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return;
  int idx = findSlot(nodeId);
  if (idx >= 0) {
    s_entries[idx].lastRssi = (int8_t)rssi;
    s_entries[idx].lastSeenMs = millis();  // любой пакет — продлеваем «онлайн»
    publishSnapshotLocked();
  }
  xSemaphoreGive(s_mutex);
}

void updateBattery(const uint8_t* nodeId, uint16_t batteryMv) {
  if (!nodeId || batteryMv == 0) return;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return;
  int idx = findSlot(nodeId);
  if (idx >= 0) {
    s_entries[idx].batteryMv = batteryMv;
    s_entries[idx].lastSeenMs = millis();
    publishSnapshotLocked();
  }
  xSemaphoreGive(s_mutex);
}

int getBatteryMv(const uint8_t* nodeId) {
  if (!nodeId) return 0;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return 0;
  uint32_t now = millis();
  int mv = 0;
  for (int i = 0; i < NEIGHBORS_MAX; i++) {
    if (!s_entries[i].used || (now - s_entries[i].lastSeenMs) >= NEIGHBOR_TIMEOUT_MS) continue;
    if (memcmp(s_entries[i].id, nodeId, protocol::NODE_ID_LEN) == 0) {
      mv = s_entries[i].batteryMv;
      break;
    }
  }
  xSemaphoreGive(s_mutex);
  return mv;
}

void recordAckSent(const uint8_t* nodeId) {
  if (!nodeId) return;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return;
  int idx = findSlot(nodeId);
  if (idx >= 0 && s_entries[idx].ackSent < 65535) {
    s_entries[idx].ackSent++;
    publishSnapshotLocked();
  }
  xSemaphoreGive(s_mutex);
}

void recordAckReceived(const uint8_t* nodeId) {
  if (!nodeId) return;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return;
  int idx = findSlot(nodeId);
  if (idx >= 0 && s_entries[idx].ackReceived < 65535) {
    s_entries[idx].ackReceived++;
    publishSnapshotLocked();
  }
  xSemaphoreGive(s_mutex);
}

int getAckRatePermille(const uint8_t* nodeId) {
  if (!nodeId) return -1;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return -1;
  uint32_t now = millis();
  int rate = -1;
  for (int i = 0; i < NEIGHBORS_MAX; i++) {
    if (!s_entries[i].used || (now - s_entries[i].lastSeenMs) >= NEIGHBOR_TIMEOUT_MS) continue;
    if (memcmp(s_entries[i].id, nodeId, protocol::NODE_ID_LEN) == 0) {
      if (s_entries[i].ackSent > 0) {
        rate = (int)((uint32_t)s_entries[i].ackReceived * 1000U / (uint32_t)s_entries[i].ackSent);
      }
      break;
    }
  }
  xSemaphoreGive(s_mutex);
  return rate;
}

int getRssi(int i) {
  if (i < 0) return 0;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return 0;
  uint32_t now = millis();
  int idx = 0;
  for (int j = 0; j < NEIGHBORS_MAX; j++) {
    if (!s_entries[j].used || (now - s_entries[j].lastSeenMs) >= NEIGHBOR_TIMEOUT_MS) continue;
    if (idx == i) {
      int r = s_entries[j].lastRssi;
      xSemaphoreGive(s_mutex);
      return r;
    }
    idx++;
  }
  xSemaphoreGive(s_mutex);
  return 0;
}

uint8_t rssiToSfOrthogonal(const uint8_t* nodeId) {
  if (!nodeId) return 0;
  int r = getRssiFor(nodeId);
  uint8_t sfRssi = rssiToSf(r);
  if (getCount() < 2) return sfRssi ? sfRssi : 12;
  uint32_t h = 0;
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) h = h * 31 + nodeId[i];
  uint8_t sfOrtho = 7 + (uint8_t)(h % 6);
  return (sfRssi > 0 && sfRssi > sfOrtho) ? sfRssi : sfOrtho;
}

int getRssiFor(const uint8_t* nodeId) {
  if (!nodeId) return -128;
  uint32_t now = millis();
  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t seqStart = s_snapshotSeq.load(std::memory_order_acquire);
    if (seqStart & 1u) continue;
    int r = -128;
    for (int i = 0; i < NEIGHBORS_MAX; i++) {
      if (!s_snapshot[i].used || (now - s_snapshot[i].lastSeenMs) >= NEIGHBOR_TIMEOUT_MS) continue;
      if (memcmp(s_snapshot[i].id, nodeId, protocol::NODE_ID_LEN) == 0) {
        r = s_snapshot[i].lastRssi;
        break;
      }
    }
    uint32_t seqEnd = s_snapshotSeq.load(std::memory_order_acquire);
    if (seqStart == seqEnd && !(seqEnd & 1u)) return r;
  }
  return -128;
}

int getMinRssi() {
  uint32_t now = millis();
  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t seqStart = s_snapshotSeq.load(std::memory_order_acquire);
    if (seqStart & 1u) continue;
    int minRssi = 0;
    bool hasAny = false;
    for (int i = 0; i < NEIGHBORS_MAX; i++) {
      if (!s_snapshot[i].used || (now - s_snapshot[i].lastSeenMs) >= NEIGHBOR_TIMEOUT_MS) continue;
      if (s_snapshot[i].lastRssi != 0) {
        if (!hasAny || s_snapshot[i].lastRssi < minRssi) {
          minRssi = s_snapshot[i].lastRssi;
          hasAny = true;
        }
      }
    }
    uint32_t seqEnd = s_snapshotSeq.load(std::memory_order_acquire);
    if (seqStart == seqEnd && !(seqEnd & 1u)) return hasAny ? minRssi : 0;
  }
  return 0;
}

int getAverageRssi() {
  uint32_t now = millis();
  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t seqStart = s_snapshotSeq.load(std::memory_order_acquire);
    if (seqStart & 1u) continue;
    int sum = 0;
    int n = 0;
    for (int i = 0; i < NEIGHBORS_MAX; i++) {
      if (!s_snapshot[i].used || (now - s_snapshot[i].lastSeenMs) >= NEIGHBOR_TIMEOUT_MS) continue;
      if (s_snapshot[i].lastRssi != 0) {
        sum += s_snapshot[i].lastRssi;
        n++;
      }
    }
    uint32_t seqEnd = s_snapshotSeq.load(std::memory_order_acquire);
    if (seqStart == seqEnd && !(seqEnd & 1u)) return (n == 0) ? -90 : (sum / n);
  }
  return -90;
}

int getCount() {
  uint32_t now = millis();
  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t seqStart = s_snapshotSeq.load(std::memory_order_acquire);
    if (seqStart & 1u) continue;
    int n = 0;
    for (int i = 0; i < NEIGHBORS_MAX; i++) {
      if (!s_snapshot[i].used) continue;
      uint32_t age = now - s_snapshot[i].lastSeenMs;
      if (age < NEIGHBOR_TIMEOUT_MS) n++;
    }
    uint32_t seqEnd = s_snapshotSeq.load(std::memory_order_acquire);
    if (seqStart == seqEnd && !(seqEnd & 1u)) return n;
  }
  return 0;
}

bool isOnline(const uint8_t* nodeId) {
  if (!nodeId) return false;
  uint32_t now = millis();
  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t seqStart = s_snapshotSeq.load(std::memory_order_acquire);
    if (seqStart & 1u) continue;
    bool ok = false;
    for (int i = 0; i < NEIGHBORS_MAX; i++) {
      if (!s_snapshot[i].used || (now - s_snapshot[i].lastSeenMs) >= NEIGHBOR_TIMEOUT_MS) continue;
      if (memcmp(s_snapshot[i].id, nodeId, protocol::NODE_ID_LEN) == 0) {
        ok = true;
        break;
      }
    }
    uint32_t seqEnd = s_snapshotSeq.load(std::memory_order_acquire);
    if (seqStart == seqEnd && !(seqEnd & 1u)) return ok;
  }
  return false;
}

uint32_t getFreshnessMs(const uint8_t* nodeId) {
  if (!nodeId) return UINT32_MAX;
  uint32_t now = millis();
  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t seqStart = s_snapshotSeq.load(std::memory_order_acquire);
    if (seqStart & 1u) continue;
    uint32_t out = UINT32_MAX;
    for (int i = 0; i < NEIGHBORS_MAX; i++) {
      if (!s_snapshot[i].used) continue;
      if (memcmp(s_snapshot[i].id, nodeId, protocol::NODE_ID_LEN) == 0) {
        uint32_t age = now - s_snapshot[i].lastSeenMs;
        if (age < NEIGHBOR_TIMEOUT_MS) out = age;
        break;
      }
    }
    uint32_t seqEnd = s_snapshotSeq.load(std::memory_order_acquire);
    if (seqStart == seqEnd && !(seqEnd & 1u)) return out;
  }
  return UINT32_MAX;
}

bool getId(int i, uint8_t* out) {
  if (!out || i < 0) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;
  uint32_t now = millis();
  int idx = 0;
  for (int j = 0; j < NEIGHBORS_MAX; j++) {
    if (!s_entries[j].used || (now - s_entries[j].lastSeenMs) >= NEIGHBOR_TIMEOUT_MS) continue;
    if (idx == i) {
      memcpy(out, s_entries[j].id, protocol::NODE_ID_LEN);
      xSemaphoreGive(s_mutex);
      return true;
    }
    idx++;
  }
  xSemaphoreGive(s_mutex);
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
