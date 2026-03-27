/**
 * FakeTech Node — Node ID, никнейм
 */

#include "node.h"
#include "storage.h"
#include "log.h"
#include <Arduino.h>
#include <stdio.h>
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
      char idHex[17] = {0};
      for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
        sprintf(idHex + i * 2, "%02X", s_nodeId[i]);
      }
      RIFTLINK_DIAG("NODE", "event=NODE_ID id=%s new=0 source=storage", idHex);
      return;
    }
  }

  // Генерация нового ID (random из nRF52)
  randomSeed(analogRead(0) + millis());
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    s_nodeId[i] = (uint8_t)(random(256));
  }
  char idHex[17] = {0};
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    sprintf(idHex + i * 2, "%02X", s_nodeId[i]);
  }
  RIFTLINK_DIAG("NODE", "event=NODE_ID id=%s new=1 source=random", idHex);

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
