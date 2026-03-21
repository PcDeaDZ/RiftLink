/**
 * RiftLink Node — Node ID, никнейм
 */

#include "node.h"
#include <Arduino.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <nvs.h>
#include <esp_random.h>
#include <string.h>

static uint8_t s_nodeId[protocol::NODE_ID_LEN];
static char s_nickname[33] = {0};
static bool s_inited = false;

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_NODEID "nodeid"
#define NVS_KEY_NICKNAME "nick"
#define NICKNAME_MAX 32

namespace node {

void init() {
  if (s_inited) return;

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    size_t len = protocol::NODE_ID_LEN;
    if (nvs_get_blob(h, NVS_KEY_NODEID, s_nodeId, &len) == ESP_OK) {
      // Отвергаем некорректный ID (0xFF 0xFF в начале = broadcast/сброс NVS)
      if (s_nodeId[0] == 0xFF && s_nodeId[1] == 0xFF) {
        Serial.println("[RiftLink] Node ID invalid (0xFF..), regenerating");
        nvs_close(h);
        // Генерируем новый — см. ниже
      } else {
        len = sizeof(s_nickname);
        if (nvs_get_str(h, NVS_KEY_NICKNAME, s_nickname, &len) != ESP_OK) {
          s_nickname[0] = '\0';
        }
        s_inited = true;
        nvs_close(h);
        return;
      }
    }
    nvs_close(h);
  }

  // Генерация нового ID
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    s_nodeId[i] = (uint8_t)(esp_random() & 0xFF);
  }
  Serial.printf("[RiftLink] Node ID: %02X%02X%02X%02X... (new)\n",
      s_nodeId[0], s_nodeId[1], s_nodeId[2], s_nodeId[3]);

  nvs_handle_t hw;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &hw) == ESP_OK) {
    nvs_set_blob(hw, NVS_KEY_NODEID, s_nodeId, protocol::NODE_ID_LEN);
    nvs_commit(hw);
    nvs_close(hw);
  }
  s_inited = true;
}

const uint8_t* getId() {
  return s_nodeId;
}

void getIdCopy(uint8_t* out) {
  memcpy(out, s_nodeId, protocol::NODE_ID_LEN);
}

bool isForMe(const uint8_t* to) {
  return memcmp(to, s_nodeId, protocol::NODE_ID_LEN) == 0;
}

bool isBroadcast(const uint8_t* to) {
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    if (to[i] != 0xFF) return false;
  }
  return true;
}

// 4 байта: при коррупции 1 байта в from (A653..5801→A653..5802) isForMe не сработает,
// но short ID совпадёт — отсекаем ghost «себя». LoRa CRC (2B) уже проверяет целостность.
bool isSameShortId(const uint8_t* id) {
  if (!id) return false;
  return memcmp(id, s_nodeId, 4) == 0;
}

bool isInvalidNodeId(const uint8_t* id) {
  if (!id) return true;
  if (id[0] == 0xFF && id[1] == 0xFF) return true;  // broadcast-like
  if (id[0] == 0x00 && id[1] == 0x00) return true;  // ghost от шума/коррупции
  // Сдвиг: opcode/TTL попали в from (03=HELLO, 06=KEY_EXCHANGE, 1F=TTL)
  if (id[0] == 0x03 && id[1] == 0x00) return true;
  if (id[0] == 0x06 && id[1] == 0x00) return true;
  if (id[0] == 0x1F && id[1] == 0x03) return true;
  if (id[0] == 0x1F && id[1] == 0x06) return true;
  return false;
}

void getNickname(char* out, size_t maxLen) {
  if (!out || maxLen == 0) return;
  strncpy(out, s_nickname, maxLen - 1);
  out[maxLen - 1] = '\0';
}

bool setNickname(const char* name) {
  if (!name) return false;
  size_t len = strnlen(name, NICKNAME_MAX + 1);
  if (len > NICKNAME_MAX) return false;

  strncpy(s_nickname, name, NICKNAME_MAX);
  s_nickname[NICKNAME_MAX] = '\0';

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_str(h, NVS_KEY_NICKNAME, s_nickname);
    nvs_commit(h);
    nvs_close(h);
  }
  return true;
}

}  // namespace node
