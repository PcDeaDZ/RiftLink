/**
 * RiftLink Offline Queue — store-and-forward (RAM + NVS)
 * При HELLO от узла — доставка накопленных сообщений
 */

#include "offline_queue.h"
#include "neighbors/neighbors.h"
#include "node/node.h"
#include "radio/radio.h"
#include <Arduino.h>
#include <nvs.h>
#include <esp_err.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_OFFLINE "offline_q"

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
  bool inUse;
};

static StoredMsg s_msgs[OFFLINE_MAX_MSGS];
static bool s_inited = false;
static volatile bool s_dirty = false;  // отложенное сохранение в NVS — не блокировать handlePacket
static SemaphoreHandle_t s_mutex = nullptr;
static StoredMsgNvs s_nvsBuf[OFFLINE_MAX_MSGS];  // статика — saveToNvs/loadFromNvs вызываются из handlePacket (stack overflow)

#define NVS_SAVE_INTERVAL_MS 2000
#define MUTEX_TIMEOUT_MS 100

/** Копирование под mutex (быстро). Вызывать с захваченным mutex. */
static void copyToNvsBuffer() {
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    s_nvsBuf[i].inUse = s_msgs[i].inUse ? 1 : 0;
    memcpy(s_nvsBuf[i].to, s_msgs[i].to, protocol::NODE_ID_LEN);
    s_nvsBuf[i].payloadLen = (uint16_t)s_msgs[i].payloadLen;
    s_nvsBuf[i].opcode = s_msgs[i].opcode;
    s_nvsBuf[i].flags = s_msgs[i].flags;
    memcpy(s_nvsBuf[i].payload, s_msgs[i].payload, OFFLINE_MAX_LEN);
  }
}

/** Запись в NVS (без mutex — пишем s_nvsBuf). */
static void writeNvsBuffer() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    Serial.printf("[RiftLink] NVS save failed: %s (0x%x)\n", esp_err_to_name(err), err);
    return;
  }
  err = nvs_set_blob(h, NVS_KEY_OFFLINE, s_nvsBuf, sizeof(s_nvsBuf));
  if (err != ESP_OK) {
    Serial.printf("[RiftLink] NVS set_blob failed: %s\n", esp_err_to_name(err));
  } else if (nvs_commit(h) != ESP_OK) {
    Serial.println("[RiftLink] NVS commit failed");
  }
  nvs_close(h);
}

static void nvsTask(void* arg) {
  vTaskDelay(pdMS_TO_TICKS(2000));  // дать init завершиться
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(NVS_SAVE_INTERVAL_MS));
    if (!s_dirty || !s_mutex) continue;
    bool doWrite = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (s_dirty) {
        copyToNvsBuffer();
        s_dirty = false;
        doWrite = true;
      }
      xSemaphoreGive(s_mutex);
    }
    if (doWrite) writeNvsBuffer();
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
  }
}

namespace offline_queue {

#define NVS_TASK_STACK 2048
#define NVS_TASK_PRIO 1

void init() {
  s_mutex = xSemaphoreCreateMutex();
  memset(s_msgs, 0, sizeof(s_msgs));
  loadFromNvs();
  s_inited = true;
  xTaskCreate(nvsTask, "nvs", NVS_TASK_STACK, nullptr, NVS_TASK_PRIO, nullptr);
}

static StoredMsg* findFree() {
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    if (!s_msgs[i].inUse) return &s_msgs[i];
  }
  return nullptr;
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
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    StoredMsg* m = &s_msgs[i];
    if (!m->inUse) continue;
    if (memcmp(m->to, nodeId, protocol::NODE_ID_LEN) != 0) continue;

    uint8_t* pkt = s_onlinePktBuf;
    bool compressed = (m->flags & 1) != 0;
    size_t len = protocol::buildPacket(pkt, sizeof(s_onlinePktBuf),
        node::getId(), m->to, 31, m->opcode,
        m->payload, m->payloadLen, true, true, compressed);
    if (len > 0) {
      radio::send(pkt, len, neighbors::rssiToSf(neighbors::getRssiFor(nodeId)), true);  // priority
      Serial.printf("[RiftLink] Offline delivery to %02X%02X\n", nodeId[0], nodeId[1]);
    }
    m->inUse = false;
    modified = true;
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

void update() {
  // Сохранение перенесено в nvsTask — loop не блокируется
}

}  // namespace offline_queue
