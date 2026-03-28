/**
 * KV на Internal LittleFS (Adafruit nRF52).
 */
#include "kv.h"
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <stdio.h>
#include <string.h>

using Adafruit_LittleFS_Namespace::File;

static bool s_ok = false;

namespace riftlink_kv {

bool begin() {
  if (s_ok) return true;
  if (!InternalFS.begin()) return false;
  s_ok = true;
  return true;
}

static void pathFor(char* out, size_t outLen, const char* key) {
  snprintf(out, outLen, "/rl_%s", key);
}

bool getBlob(const char* key, uint8_t* buf, size_t* len) {
  if (!s_ok || !key || !len) return false;
  char path[48];
  pathFor(path, sizeof(path), key);
  File f = InternalFS.open(path, Adafruit_LittleFS_Namespace::FILE_O_READ);
  if (!f) return false;
  size_t n = f.readBytes((char*)buf, *len);
  f.close();
  *len = n;
  return n > 0;
}

bool setBlob(const char* key, const uint8_t* buf, size_t len) {
  if (!s_ok || !key) return false;
  char path[48];
  pathFor(path, sizeof(path), key);
  File f = InternalFS.open(path, Adafruit_LittleFS_Namespace::FILE_O_WRITE);
  if (!f) return false;
  size_t w = f.write(buf, len);
  f.close();
  return w == len;
}

bool getU32(const char* key, uint32_t* out) {
  uint8_t b[4];
  size_t n = 4;
  if (!getBlob(key, b, &n) || n != 4) return false;
  memcpy(out, b, 4);
  return true;
}

bool setU32(const char* key, uint32_t v) {
  uint8_t b[4];
  memcpy(b, &v, 4);
  return setBlob(key, b, 4);
}

bool getI8(const char* key, int8_t* out) {
  uint8_t b[1];
  size_t n = 1;
  if (!getBlob(key, b, &n) || n != 1) return false;
  *out = (int8_t)b[0];
  return true;
}

bool setI8(const char* key, int8_t v) {
  uint8_t b = (uint8_t)v;
  return setBlob(key, &b, 1);
}

}  // namespace riftlink_kv
