/**
 * Node — nRF52840 (KV вместо NVS)
 */
#include "node/node.h"
#include "kv.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

static uint8_t s_nodeId[protocol::NODE_ID_LEN];
static char s_nickname[33] = {0};
static bool s_inited = false;

#define KEY_NODEID "nodeid"
#define KEY_NICKNAME "nick"
#define NICKNAME_MAX 32

namespace node {

void init() {
  if (s_inited) return;

  size_t len = protocol::NODE_ID_LEN;
  if (riftlink_kv::getBlob(KEY_NODEID, s_nodeId, &len) && len == protocol::NODE_ID_LEN) {
    if (s_nodeId[0] == 0xFF && s_nodeId[1] == 0xFF) {
      Serial.println("[RiftLink] Node ID invalid (0xFF..), regenerating");
    } else {
      len = sizeof(s_nickname);
      if (!riftlink_kv::getBlob(KEY_NICKNAME, (uint8_t*)s_nickname, &len)) {
        s_nickname[0] = '\0';
      } else {
        s_nickname[sizeof(s_nickname) - 1] = '\0';
      }
      s_inited = true;
      return;
    }
  }

  randomSeed((uint32_t)micros() ^ (uint32_t)((uintptr_t)&s_nodeId));
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    s_nodeId[i] = (uint8_t)(random(256));
  }
  Serial.printf("[RiftLink] Node ID: %02X%02X%02X%02X%02X%02X%02X%02X (new)\n",
      s_nodeId[0], s_nodeId[1], s_nodeId[2], s_nodeId[3], s_nodeId[4], s_nodeId[5], s_nodeId[6], s_nodeId[7]);

  (void)riftlink_kv::setBlob(KEY_NODEID, s_nodeId, protocol::NODE_ID_LEN);
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

bool isSameShortId(const uint8_t* id) {
  if (!id) return false;
  return memcmp(id, s_nodeId, protocol::NODE_ID_LEN) == 0;
}

bool isInvalidNodeId(const uint8_t* id) {
  if (!id) return true;
  if (id[0] == 0xFF && id[1] == 0xFF) return true;
  if (id[0] == 0x00 && id[1] == 0x00) return true;
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

  (void)riftlink_kv::setBlob(KEY_NICKNAME, (const uint8_t*)s_nickname, strlen(s_nickname) + 1);
  return true;
}

}  // namespace node
