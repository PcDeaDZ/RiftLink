/**
 * RiftLink Offline Queue — store-and-forward (RAM + NVS)
 * При HELLO от узла — доставка накопленных сообщений
 */

#include "offline_queue.h"
#include "neighbors/neighbors.h"
#include "node/node.h"
#include "radio/radio.h"
#include "async_tasks.h"
#include "log.h"
#include <Arduino.h>
#include <nvs.h>
#include <esp_err.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_OFFLINE "offline_q"
#define OFFLINE_FLAG_COMPRESSED 0x01
#define OFFLINE_FLAG_CRITICAL   0x02
#define OFFLINE_FLAG_COURIER    0x04

#pragma pack(push, 1)
struct StoredMsgNvs {
  uint8_t inUse;
  uint8_t to[protocol::NODE_ID_LEN];
  uint16_t payloadLen;
  uint8_t opcode;
  uint8_t flags;
  uint8_t payload[OFFLINE_MAX_LEN];
};
#pragma pack(pop)

struct StoredMsg {
  uint8_t to[protocol::NODE_ID_LEN];
  uint8_t payload[OFFLINE_MAX_LEN];
  size_t payloadLen;
  uint8_t opcode;
  uint8_t flags;
  uint32_t queuedAtMs;
  uint32_t seq;
  bool inUse;
};

static StoredMsg s_msgs[OFFLINE_MAX_MSGS];
// NVS shadow buffer: avoid large stack allocations in nvs task and boot path.
static StoredMsgNvs s_nvsBuf[OFFLINE_MAX_MSGS];
static bool s_inited = false;
static volatile bool s_dirty = false;  // отложенное сохранение в NVS — не блокировать handlePacket
static SemaphoreHandle_t s_mutex = nullptr;
static uint32_t s_seqCounter = 0;

#define NVS_SAVE_INTERVAL_MS 2000
#define MUTEX_TIMEOUT_MS 100
#define OFFLINE_EXPIRY_NORMAL_MS (6UL * 60UL * 60UL * 1000UL)
#define OFFLINE_EXPIRY_COURIER_MS (24UL * 60UL * 60UL * 1000UL)

/** Snapshot s_msgs → s_nvsBuf под mutex, затем запись в NVS без mutex. */
static void nvsTask(void* arg) {
  vTaskDelay(pdMS_TO_TICKS(2000));
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(NVS_SAVE_INTERVAL_MS));
    if (!s_dirty || !s_mutex) continue;
    bool doWrite = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (s_dirty) {
        for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
          s_nvsBuf[i].inUse = s_msgs[i].inUse ? 1 : 0;
          memcpy(s_nvsBuf[i].to, s_msgs[i].to, protocol::NODE_ID_LEN);
          s_nvsBuf[i].payloadLen = (uint16_t)s_msgs[i].payloadLen;
          s_nvsBuf[i].opcode = s_msgs[i].opcode;
          s_nvsBuf[i].flags = s_msgs[i].flags;
          memcpy(s_nvsBuf[i].payload, s_msgs[i].payload, OFFLINE_MAX_LEN);
        }
        s_dirty = false;
        doWrite = true;
      }
      xSemaphoreGive(s_mutex);
    }
    if (doWrite) {
      nvs_handle_t h;
      esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
      if (err != ESP_OK) {
        Serial.printf("[RiftLink] NVS save failed: %s (0x%x)\n", esp_err_to_name(err), err);
        continue;
      }
      err = nvs_set_blob(h, NVS_KEY_OFFLINE, s_nvsBuf, sizeof(s_nvsBuf));
      if (err != ESP_OK) {
        Serial.printf("[RiftLink] NVS set_blob failed: %s\n", esp_err_to_name(err));
      } else if (nvs_commit(h) != ESP_OK) {
        Serial.println("[RiftLink] NVS commit failed");
      }
      nvs_close(h);
    }
  }
}

static void loadFromNvs() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
  if (err != ESP_OK) {
    if (err != ESP_ERR_NVS_NOT_FOUND)
      Serial.printf("[RiftLink] NVS load failed: %s (0x%x)\n", esp_err_to_name(err), err);
    return;
  }
  size_t len = sizeof(s_nvsBuf);
  if (nvs_get_blob(h, NVS_KEY_OFFLINE, s_nvsBuf, &len) != ESP_OK) {
    nvs_close(h);
    return;
  }
  nvs_close(h);
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    s_msgs[i].inUse = (s_nvsBuf[i].inUse != 0);
    memcpy(s_msgs[i].to, s_nvsBuf[i].to, protocol::NODE_ID_LEN);
    s_msgs[i].payloadLen = s_nvsBuf[i].payloadLen;
    s_msgs[i].opcode = s_nvsBuf[i].opcode;
    s_msgs[i].flags = s_nvsBuf[i].flags;
    memcpy(s_msgs[i].payload, s_nvsBuf[i].payload, OFFLINE_MAX_LEN);
    s_msgs[i].queuedAtMs = millis();
    s_msgs[i].seq = ++s_seqCounter;
  }
}

namespace offline_queue {

#define NVS_TASK_STACK 8192  // запас под NVS internals + логи; большой буфер вынесен из стека
#define NVS_TASK_PRIO 1

void init() {
  s_mutex = xSemaphoreCreateMutex();
  memset(s_msgs, 0, sizeof(s_msgs));
  loadFromNvs();
  s_inited = true;
  // IMPORTANT: nvsTask calls nvs_commit (flash ops with cache-disabled sections).
  // Keep this stack in internal RAM; PSRAM stacks can trigger
  // esp_task_stack_is_sane_cache_disabled() assert on ESP32-S3.
  xTaskCreate(nvsTask, "nvs", NVS_TASK_STACK, nullptr, NVS_TASK_PRIO, nullptr);
}

static StoredMsg* findFree() {
  int oldestIdx = -1;
  uint32_t oldestSeq = 0xFFFFFFFFUL;
  int oldestCourierIdx = -1;
  uint32_t oldestCourierSeq = 0xFFFFFFFFUL;
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    if (!s_msgs[i].inUse) return &s_msgs[i];
    if ((s_msgs[i].flags & OFFLINE_FLAG_COURIER) != 0) {
      if (s_msgs[i].seq < oldestCourierSeq) {
        oldestCourierSeq = s_msgs[i].seq;
        oldestCourierIdx = i;
      }
    } else if (s_msgs[i].seq < oldestSeq) {
      oldestSeq = s_msgs[i].seq;
      oldestIdx = i;
    }
  }
  // Eviction policy: сначала вытесняем самый старый обычный пакет, иначе courier.
  int evictIdx = (oldestIdx >= 0) ? oldestIdx : oldestCourierIdx;
  if (evictIdx >= 0) return &s_msgs[evictIdx];
  return nullptr;
}

static bool isExpired(const StoredMsg* m, uint32_t nowMs) {
  if (!m || !m->inUse) return false;
  uint32_t ttlMs = (m->flags & OFFLINE_FLAG_COURIER) ? OFFLINE_EXPIRY_COURIER_MS : OFFLINE_EXPIRY_NORMAL_MS;
  return (nowMs - m->queuedAtMs) > ttlMs;
}

bool enqueue(const uint8_t* to, const uint8_t* encPayload, size_t encLen, uint8_t opcode, uint8_t flags) {
  if (!s_inited || !to || encLen > OFFLINE_MAX_LEN) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;

  StoredMsg* slot = findFree();
  if (!slot) {
    xSemaphoreGive(s_mutex);
    return false;
  }

  memcpy(slot->to, to, protocol::NODE_ID_LEN);
  memcpy(slot->payload, encPayload, encLen);
  slot->payloadLen = encLen;
  slot->opcode = opcode;
  slot->flags = flags;
  slot->queuedAtMs = millis();
  slot->seq = ++s_seqCounter;
  slot->inUse = true;
  s_dirty = true;
  xSemaphoreGive(s_mutex);
  return true;
}

bool enqueueCourier(const uint8_t* pkt, size_t len) {
  if (!s_inited || !pkt || len == 0 || len > OFFLINE_MAX_LEN) return false;
  protocol::PacketHeader hdr;
  const uint8_t* pl = nullptr;
  size_t plLen = 0;
  if (!protocol::parsePacket(pkt, len, &hdr, &pl, &plLen)) return false;
  if (node::isBroadcast(hdr.to)) return false;
  if (hdr.opcode != protocol::OP_MSG && hdr.opcode != protocol::OP_SOS) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;
  StoredMsg* slot = findFree();
  if (!slot) {
    xSemaphoreGive(s_mutex);
    return false;
  }
  memcpy(slot->to, hdr.to, protocol::NODE_ID_LEN);
  memcpy(slot->payload, pkt, len);
  slot->payloadLen = len;
  slot->opcode = hdr.opcode;
  slot->flags = OFFLINE_FLAG_COURIER | ((hdr.channel == protocol::CHANNEL_CRITICAL) ? OFFLINE_FLAG_CRITICAL : 0);
  slot->queuedAtMs = millis();
  slot->seq = ++s_seqCounter;
  slot->inUse = true;
  s_dirty = true;
  xSemaphoreGive(s_mutex);
  return true;
}

static uint8_t s_onlinePktBuf[protocol::PAYLOAD_OFFSET + OFFLINE_MAX_LEN];  // статика — onNodeOnline вызывается из handlePacket

void onNodeOnline(const uint8_t* nodeId) {
  if (!s_inited || !nodeId) return;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return;

  bool modified = false;
  uint32_t now = millis();
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    StoredMsg* m = &s_msgs[i];
    if (!m->inUse) continue;
    if (isExpired(m, now)) {
      m->inUse = false;
      modified = true;
      continue;
    }
    if (memcmp(m->to, nodeId, protocol::NODE_ID_LEN) != 0) continue;

    uint8_t* pkt = s_onlinePktBuf;
    size_t len = 0;
    bool isCourier = (m->flags & OFFLINE_FLAG_COURIER) != 0;
    bool isCritical = (m->flags & OFFLINE_FLAG_CRITICAL) != 0;
    if (isCourier) {
      if (m->payloadLen <= sizeof(s_onlinePktBuf)) {
        memcpy(pkt, m->payload, m->payloadLen);
        len = m->payloadLen;
      }
    } else {
      bool compressed = (m->flags & OFFLINE_FLAG_COMPRESSED) != 0;
      uint8_t channel = isCritical ? protocol::CHANNEL_CRITICAL : protocol::CHANNEL_DEFAULT;
      len = protocol::buildPacket(pkt, sizeof(s_onlinePktBuf),
          node::getId(), m->to, 31, m->opcode,
          m->payload, m->payloadLen, true, true, compressed, channel);
    }
    if (len > 0) {
      uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(nodeId));
      char reasonBuf[40];
      // Всегда priority: иначе data-пакет застревает за ACK-reserve / overflow_norm (офлайн так и «не уходит»).
      bool queued = queueTxPacket(pkt, len, txSf, true,
          isCritical ? TxRequestClass::critical : TxRequestClass::data,
          reasonBuf, sizeof(reasonBuf));
      if (queued) {
        Serial.printf("[RiftLink] Offline delivery to %02X%02X\n", nodeId[0], nodeId[1]);
        m->inUse = false;
        modified = true;
      } else {
        // Best-effort deferred fallback. Keep entry in queue as safety net in case deferred slots are full.
        queueDeferredSend(pkt, len, txSf, 90, true);
        RIFTLINK_DIAG("OFFLINE", "event=OFFLINE_TX_DEFER to=%02X%02X cause=%s",
            nodeId[0], nodeId[1], reasonBuf[0] ? reasonBuf : "?");
      }
    } else {
      if (!isCourier) {
        RIFTLINK_DIAG("OFFLINE", "event=OFFLINE_BUILD_FAIL to=%02X%02X op=0x%02X pl=%u max_payload=%u",
            m->to[0], m->to[1], (unsigned)m->opcode, (unsigned)m->payloadLen, (unsigned)protocol::MAX_PAYLOAD);
      }
      m->inUse = false;
      modified = true;
    }
  }
  if (modified) s_dirty = true;
  xSemaphoreGive(s_mutex);
}

int getPendingCount() {
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return 0;
  int n = 0;
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    if (s_msgs[i].inUse) n++;
  }
  xSemaphoreGive(s_mutex);
  return n;
}

int getCourierPendingCount() {
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return 0;
  int n = 0;
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    if (s_msgs[i].inUse && (s_msgs[i].flags & OFFLINE_FLAG_COURIER) != 0) n++;
  }
  xSemaphoreGive(s_mutex);
  return n;
}

int getDirectPendingCount() {
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return 0;
  int n = 0;
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    if (s_msgs[i].inUse && (s_msgs[i].flags & OFFLINE_FLAG_COURIER) == 0) n++;
  }
  xSemaphoreGive(s_mutex);
  return n;
}

void update() {
  // Сохранение перенесено в nvsTask — loop не блокируется.
  // Flush по-прежнему по HELLO (onNodeOnline); редкий sweep опасен: при OFFLINE_TX_DEFER даст дубликаты в deferred.
}

}  // namespace offline_queue
