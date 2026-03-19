/**
 * FakeTech Crypto — pass-through (без шифрования для первой версии)
 */

#include "crypto.h"
#include "storage.h"
#include <string.h>

#define KEY_LEN 32
#define STORAGE_KEY_CHANNEL "chkey"

static uint8_t s_key[KEY_LEN];
static bool s_inited = false;

static const uint8_t DEFAULT_KEY[KEY_LEN] = {
  0x52, 0x69, 0x66, 0x74, 0x4c, 0x69, 0x6e, 0x6b,
  0x2d, 0x52, 0x4c, 0x2d, 0x63, 0x68, 0x61, 0x6e,
  0x6e, 0x65, 0x6c, 0x2d, 0x6b, 0x65, 0x79, 0x2d,
  0x32, 0x30, 0x32, 0x35, 0x21, 0x21, 0x21, 0x21
};

namespace crypto {

bool init() {
  if (s_inited) return true;

  size_t len = KEY_LEN;
  if (storage::getBlob(STORAGE_KEY_CHANNEL, s_key, &len)) {
    s_inited = true;
    return true;
  }

  memcpy(s_key, DEFAULT_KEY, KEY_LEN);
  storage::setBlob(STORAGE_KEY_CHANNEL, s_key, KEY_LEN);
  s_inited = true;
  return true;
}

bool encrypt(const uint8_t* plain, size_t len, uint8_t* out, size_t* outLen) {
  (void)s_key;
  if (!s_inited || !out || !outLen) return false;
  if (*outLen < len) return false;
  memcpy(out, plain, len);
  *outLen = len;
  return true;
}

bool decrypt(const uint8_t* cipher, size_t len, uint8_t* out, size_t* outLen) {
  if (!s_inited || !out || !outLen) return false;
  memcpy(out, cipher, len);
  *outLen = len;
  return true;
}

bool encryptFor(const uint8_t* peerId, const uint8_t* plain, size_t len, uint8_t* out, size_t* outLen) {
  (void)peerId;
  return encrypt(plain, len, out, outLen);
}

bool decryptFrom(const uint8_t* senderId, const uint8_t* cipher, size_t len, uint8_t* out, size_t* outLen) {
  (void)senderId;
  return decrypt(cipher, len, out, outLen);
}

bool setChannelKey(const uint8_t* key) {
  if (!key) return false;
  memcpy(s_key, key, KEY_LEN);
  storage::setBlob(STORAGE_KEY_CHANNEL, s_key, KEY_LEN);
  return true;
}

bool getChannelKey(uint8_t* out) {
  if (!s_inited || !out) return false;
  memcpy(out, s_key, KEY_LEN);
  return true;
}

}  // namespace crypto
