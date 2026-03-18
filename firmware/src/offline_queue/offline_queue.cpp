/**
 * RiftLink Offline Queue — store-and-forward (RAM + NVS)
 * При HELLO от узла — доставка накопленных сообщений
 */

#include "offline_queue.h"
#include "node/node.h"
#include "radio/radio.h"
#include <Arduino.h>
#include <nvs.h>
#include <esp_err.h>
#include <string.h>

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

static void saveToNvs() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    Serial.printf("[RiftLink] NVS save failed: %s (0x%x)\n", esp_err_to_name(err), err);
    return;
  }
  StoredMsgNvs buf[OFFLINE_MAX_MSGS];
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    buf[i].inUse = s_msgs[i].inUse ? 1 : 0;
    memcpy(buf[i].to, s_msgs[i].to, protocol::NODE_ID_LEN);
    buf[i].payloadLen = (uint16_t)s_msgs[i].payloadLen;
    buf[i].opcode = s_msgs[i].opcode;
    buf[i].flags = s_msgs[i].flags;
    memcpy(buf[i].payload, s_msgs[i].payload, OFFLINE_MAX_LEN);
  }
  err = nvs_set_blob(h, NVS_KEY_OFFLINE, buf, sizeof(buf));
  if (err != ESP_OK) {
    Serial.printf("[RiftLink] NVS set_blob failed: %s\n", esp_err_to_name(err));
  } else if (nvs_commit(h) != ESP_OK) {
    Serial.println("[RiftLink] NVS commit failed");
  }
  nvs_close(h);
}

static void loadFromNvs() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
  if (err != ESP_OK) {
    if (err != ESP_ERR_NVS_NOT_FOUND)
      Serial.printf("[RiftLink] NVS load failed: %s (0x%x)\n", esp_err_to_name(err), err);
    return;
  }
  StoredMsgNvs buf[OFFLINE_MAX_MSGS];
  size_t len = sizeof(buf);
  if (nvs_get_blob(h, NVS_KEY_OFFLINE, buf, &len) != ESP_OK) {
    nvs_close(h);
    return;
  }
  nvs_close(h);
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    s_msgs[i].inUse = (buf[i].inUse != 0);
    memcpy(s_msgs[i].to, buf[i].to, protocol::NODE_ID_LEN);
    s_msgs[i].payloadLen = buf[i].payloadLen;
    s_msgs[i].opcode = buf[i].opcode;
    s_msgs[i].flags = buf[i].flags;
    memcpy(s_msgs[i].payload, buf[i].payload, OFFLINE_MAX_LEN);
  }
}

namespace offline_queue {

void init() {
  memset(s_msgs, 0, sizeof(s_msgs));
  loadFromNvs();
  s_inited = true;
}

static StoredMsg* findFree() {
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    if (!s_msgs[i].inUse) return &s_msgs[i];
  }
  return nullptr;
}

bool enqueue(const uint8_t* to, const uint8_t* encPayload, size_t encLen, uint8_t opcode, uint8_t flags) {
  if (!s_inited || !to || encLen > OFFLINE_MAX_LEN) return false;

  StoredMsg* slot = findFree();
  if (!slot) return false;

  memcpy(slot->to, to, protocol::NODE_ID_LEN);
  memcpy(slot->payload, encPayload, encLen);
  slot->payloadLen = encLen;
  slot->opcode = opcode;
  slot->flags = flags;
  slot->inUse = true;
  saveToNvs();
  return true;
}

void onNodeOnline(const uint8_t* nodeId) {
  if (!s_inited || !nodeId) return;

  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    StoredMsg* m = &s_msgs[i];
    if (!m->inUse) continue;
    if (memcmp(m->to, nodeId, protocol::NODE_ID_LEN) != 0) continue;

    uint8_t pkt[protocol::HEADER_LEN + OFFLINE_MAX_LEN];
    bool compressed = (m->flags & 1) != 0;
    size_t len = protocol::buildPacket(pkt, sizeof(pkt),
        node::getId(), m->to, 31, m->opcode,
        m->payload, m->payloadLen, true, true, compressed);
    if (len > 0) {
      radio::send(pkt, len);
      Serial.printf("[RiftLink] Offline delivery to %02X%02X\n", nodeId[0], nodeId[1]);
    }
    m->inUse = false;
  }
  saveToNvs();
}

int getPendingCount() {
  int n = 0;
  for (int i = 0; i < OFFLINE_MAX_MSGS; i++) {
    if (s_msgs[i].inUse) n++;
  }
  return n;
}

void update() {
}

}  // namespace offline_queue
