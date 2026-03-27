/**
 * FakeTech Storage — InternalFS (flash) с откатом на RAM при ошибке FS.
 */

#include "storage.h"
#include "log.h"
#include <Arduino.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <string.h>

using Adafruit_LittleFS_Namespace::File;
using Adafruit_LittleFS_Namespace::FILE_O_READ;
using Adafruit_LittleFS_Namespace::FILE_O_WRITE;

#define STORAGE_MAX_KEYS 16
#define STORAGE_MAX_VAL  64
#define STORAGE_MAX_FS_BLOB 8192

struct Entry {
  char key[16];
  uint8_t data[STORAGE_MAX_VAL];
  uint8_t len;
  bool valid;
};

static bool s_fsOk = false;
static bool s_inited = false;
static bool s_mountCalled = false;
static Entry s_entries[STORAGE_MAX_KEYS];

static String pathForKey(const char* key) {
  return String("/rl/") + key;
}

static bool writeFile(const char* key, const uint8_t* data, size_t len) {
  if (!s_fsOk || len > STORAGE_MAX_VAL) return false;
  String p = pathForKey(key);
  /* InternalFS (NVMC) может десятки мс держать CPU; yield до/после — меньше конфликтов с SoftDevice/USB. */
  yield();
  InternalFS.remove(p.c_str());
  File f = InternalFS.open(p.c_str(), FILE_O_WRITE);
  if (!f) {
    yield();
    return false;
  }
  size_t w = f.write(data, len);
  f.close();
  yield();
  return w == len;
}

static bool readFile(const char* key, uint8_t* out, size_t* len) {
  if (!s_fsOk || !out || !len) return false;
  String p = pathForKey(key);
  File f = InternalFS.open(p.c_str(), FILE_O_READ);
  if (!f) return false;
  size_t maxRead = *len;
  size_t n = f.readBytes((char*)out, maxRead);
  f.close();
  if (n == 0) return false;
  *len = n;
  return true;
}

static Entry* findRam(const char* key) {
  for (int i = 0; i < STORAGE_MAX_KEYS; i++) {
    if (s_entries[i].valid && strcmp(s_entries[i].key, key) == 0)
      return &s_entries[i];
  }
  return nullptr;
}

static Entry* allocRam(const char* key) {
  Entry* e = findRam(key);
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

namespace storage {

bool init() {
  if (s_inited) return true;
  memset(s_entries, 0, sizeof(s_entries));
#if defined(RIFTLINK_SKIP_INTERNALFS)
  s_fsOk = false;
  RIFTLINK_DIAG("STORAGE", "event=INTERNALFS skip=1 reason=RIFTLINK_SKIP_INTERNALFS mode=ram_only");
#else
  /* begin() не вызываем здесь: при первом mount стек Adafruit стирает регион и может
   * долго блокировать CPU — сначала должен уйти в Serial текст из main (см. mountInternalFs). */
  s_fsOk = false;
#endif
  s_inited = true;
  return true;
}

void mountInternalFs() {
#if defined(RIFTLINK_SKIP_INTERNALFS)
  return;
#endif
  if (s_mountCalled) return;
  s_mountCalled = true;
  yield();
  s_fsOk = InternalFS.begin();
  if (!s_fsOk) {
    RIFTLINK_DIAG("STORAGE", "event=INTERNALFS_MOUNT ok=0 mode=ram_only");
  } else {
    RIFTLINK_DIAG("STORAGE", "event=INTERNALFS_MOUNT ok=1");
  }
  yield();
}

static bool getBlobRam(const char* key, uint8_t* out, size_t* len) {
  Entry* e = findRam(key);
  if (!e) return false;
  size_t copyLen = (*len < e->len) ? *len : e->len;
  memcpy(out, e->data, copyLen);
  *len = e->len;
  return true;
}

static bool setBlobRam(const char* key, const uint8_t* data, size_t len) {
  Entry* e = allocRam(key);
  if (!e || len > STORAGE_MAX_VAL) return false;
  memcpy(e->data, data, len);
  e->len = len;
  return true;
}

bool getBlob(const char* key, uint8_t* out, size_t* len) {
  if (!s_inited || !out || !len) return false;
  if (s_fsOk) {
    size_t want = *len;
    if (readFile(key, out, len)) return true;
    *len = want;
  }
  return getBlobRam(key, out, len);
}

static bool writeFileLarge(const char* key, const uint8_t* data, size_t len) {
  if (!s_fsOk || !data || len == 0 || len > STORAGE_MAX_FS_BLOB) return false;
  String p = pathForKey(key);
  yield();
  InternalFS.remove(p.c_str());
  File f = InternalFS.open(p.c_str(), FILE_O_WRITE);
  if (!f) {
    yield();
    return false;
  }
  size_t w = f.write(data, len);
  f.close();
  yield();
  return w == len;
}

static bool readFileLarge(const char* key, uint8_t* out, size_t* len) {
  if (!s_fsOk || !out || !len) return false;
  String p = pathForKey(key);
  File f = InternalFS.open(p.c_str(), FILE_O_READ);
  if (!f) return false;
  size_t maxRead = *len;
  size_t n = f.readBytes((char*)out, maxRead);
  f.close();
  if (n == 0) return false;
  *len = n;
  return true;
}

bool setBlob(const char* key, const uint8_t* data, size_t len) {
  if (!s_inited || !data || len > STORAGE_MAX_VAL) return false;
  if (s_fsOk && writeFile(key, data, len)) {
    Entry* e = findRam(key);
    if (e) e->valid = false;
    return true;
  }
  return setBlobRam(key, data, len);
}

bool setLargeBlob(const char* key, const uint8_t* data, size_t len) {
  if (!s_inited || !key || !data || len == 0 || len > STORAGE_MAX_FS_BLOB) return false;
  if (s_fsOk && writeFileLarge(key, data, len)) {
    Entry* e = findRam(key);
    if (e) e->valid = false;
    return true;
  }
  return false;
}

bool getLargeBlob(const char* key, uint8_t* out, size_t* len) {
  if (!s_inited || !out || !len) return false;
  if (s_fsOk && readFileLarge(key, out, len)) return true;
  return false;
}

bool getStr(const char* key, char* out, size_t maxLen) {
  if (!out || maxLen == 0) return false;
  size_t len = maxLen;
  if (!getBlob(key, (uint8_t*)out, &len)) return false;
  if (len >= maxLen) len = maxLen - 1;
  out[len] = '\0';
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
