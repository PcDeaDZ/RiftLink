/**
 * FakeTech Storage — Flash-based key-value (упрощённый)
 */

#include "board.h"
#include "storage.h"
#include <Arduino.h>
#include <string.h>

// Простое хранилище в RAM (для первой версии — сброс при отключении)
// TODO: использовать LittleFS или nRF52 FDS для персистентности
#define STORAGE_MAX_KEYS 16
#define STORAGE_MAX_VAL  64

struct Entry {
  char key[16];
  uint8_t data[STORAGE_MAX_VAL];
  uint8_t len;
  bool valid;
};

static Entry s_entries[STORAGE_MAX_KEYS];
static bool s_inited = false;

namespace storage {

bool init() {
  if (s_inited) return true;
  memset(s_entries, 0, sizeof(s_entries));
  s_inited = true;
  return true;
}

static Entry* find(const char* key) {
  for (int i = 0; i < STORAGE_MAX_KEYS; i++) {
    if (s_entries[i].valid && strcmp(s_entries[i].key, key) == 0)
      return &s_entries[i];
  }
  return nullptr;
}

static Entry* alloc(const char* key) {
  Entry* e = find(key);
  if (e) return e;
  for (int i = 0; i < STORAGE_MAX_KEYS; i++) {
    if (!s_entries[i].valid) {
      strncpy(s_entries[i].key, key, 15);
      s_entries[i].key[15] = '\0';
      s_entries[i].valid = true;
      return &s_entries[i];
    }
  }
  return nullptr;
}

bool getBlob(const char* key, uint8_t* out, size_t* len) {
  if (!s_inited || !out || !len) return false;
  Entry* e = find(key);
  if (!e) return false;
  size_t copyLen = (*len < e->len) ? *len : e->len;
  memcpy(out, e->data, copyLen);
  *len = e->len;
  return true;
}

bool setBlob(const char* key, const uint8_t* data, size_t len) {
  if (!s_inited || !data || len > STORAGE_MAX_VAL) return false;
  Entry* e = alloc(key);
  if (!e) return false;
  memcpy(e->data, data, len);
  e->len = len;
  return true;
}

bool getStr(const char* key, char* out, size_t maxLen) {
  if (!out || maxLen == 0) return false;
  size_t len = maxLen;
  if (!getBlob(key, (uint8_t*)out, &len)) return false;
  out[len < maxLen ? len : maxLen - 1] = '\0';
  return true;
}

bool setStr(const char* key, const char* value) {
  size_t len = strlen(value) + 1;
  return setBlob(key, (const uint8_t*)value, len);
}

bool getU32(const char* key, uint32_t* out) {
  if (!out) return false;
  size_t len = 4;
  return getBlob(key, (uint8_t*)out, &len);
}

bool setU32(const char* key, uint32_t value) {
  return setBlob(key, (const uint8_t*)&value, 4);
}

bool getI8(const char* key, int8_t* out) {
  if (!out) return false;
  size_t len = 1;
  return getBlob(key, (uint8_t*)out, &len);
}

bool setI8(const char* key, int8_t value) {
  return setBlob(key, (const uint8_t*)&value, 1);
}

}  // namespace storage
