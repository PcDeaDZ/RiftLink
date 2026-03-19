/**
 * FakeTech Node — Node ID, никнейм
 */

#include "node.h"
#include "storage.h"
#include <Arduino.h>
#include <string.h>

static uint8_t s_nodeId[protocol::NODE_ID_LEN];
static char s_nickname[17] = {0};
static bool s_inited = false;

#define STORAGE_KEY_NODEID "nodeid"
#define STORAGE_KEY_NICKNAME "nick"

namespace node {

void init() {
  if (s_inited) return;

  size_t len = protocol::NODE_ID_LEN;
  if (storage::getBlob(STORAGE_KEY_NODEID, s_nodeId, &len)) {
    if (s_nodeId[0] == 0xFF && s_nodeId[1] == 0xFF) {
      // Invalid, regenerate
    } else {
      storage::getStr(STORAGE_KEY_NICKNAME, s_nickname, sizeof(s_nickname));
      s_inited = true;
      return;
    }
  }

  // Генерация нового ID (random из nRF52)
  randomSeed(analogRead(0) + millis());
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    s_nodeId[i] = (uint8_t)(random(256));
  }
  Serial.print("[RiftLink] Node ID: ");
  Serial.print(s_nodeId[0], HEX);
  Serial.print(s_nodeId[1], HEX);
  Serial.println("... (new)");

  storage::setBlob(STORAGE_KEY_NODEID, s_nodeId, protocol::NODE_ID_LEN);
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

void setNickname(const char* nick) {
  if (!nick) return;
  strncpy(s_nickname, nick, 16);
  s_nickname[16] = '\0';
  storage::setStr(STORAGE_KEY_NICKNAME, s_nickname);
}

const char* getNickname() {
  return s_nickname;
}

}  // namespace node
