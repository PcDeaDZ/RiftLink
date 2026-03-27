/**
 * RiftLink Crypto — ChaCha20-Poly1305 (libsodium), паритет с firmware/src/crypto/crypto.cpp (ESP).
 */

#include "crypto.h"
#include "group_owner_sign.h"
#include "rl_randombytes.h"
#include "storage.h"
#include "x25519_keys.h"

extern "C" {
#include "sodium/core.h"
#include "sodium/crypto_aead_chacha20poly1305.h"
#include "sodium/randombytes.h"
#include "sodium/utils.h"
}

#include <string.h>

#define STORAGE_KEY_CHANNEL "chkey"
#define STORAGE_NONCE_CTR "rl_nonce_ctr"
#define KEY_LEN crypto_aead_chacha20poly1305_ietf_KEYBYTES
#define NONCE_LEN crypto_aead_chacha20poly1305_ietf_NPUBBYTES
#define TAG_LEN crypto_aead_chacha20poly1305_ietf_ABYTES

static uint8_t s_key[KEY_LEN];
static uint32_t s_nonceCounter = 0;
static bool s_inited = false;

namespace crypto {

bool init() {
  rl_randombytes_install();
  if (sodium_init() < 0) return false;

  size_t len = KEY_LEN;
  if (storage::getBlob(STORAGE_KEY_CHANNEL, s_key, &len) && len == KEY_LEN) {
    (void)storage::getU32(STORAGE_NONCE_CTR, &s_nonceCounter);
    s_inited = true;
    return true;
  }

  static const uint8_t DEFAULT_KEY[KEY_LEN] = {
      0x52, 0x69, 0x66, 0x74, 0x4c, 0x69, 0x6e, 0x6b, 0x2d, 0x52, 0x4c, 0x2d, 0x63, 0x68, 0x61, 0x6e,
      0x6e, 0x65, 0x6c, 0x2d, 0x6b, 0x65, 0x79, 0x2d, 0x32, 0x30, 0x32, 0x35, 0x21, 0x21, 0x21, 0x21};
  memcpy(s_key, DEFAULT_KEY, KEY_LEN);
  s_nonceCounter = 0;
  storage::setBlob(STORAGE_KEY_CHANNEL, s_key, KEY_LEN);
  storage::setU32(STORAGE_NONCE_CTR, s_nonceCounter);
  s_inited = true;
  return true;
}

bool encrypt(const uint8_t* plain, size_t len, uint8_t* out, size_t* outLen) {
  if (!s_inited || len > 4096) return false;

  size_t need = NONCE_LEN + len + TAG_LEN;
  if (outLen && *outLen < need) return false;

  uint8_t nonce[NONCE_LEN];
  memset(nonce, 0, NONCE_LEN);
  memcpy(nonce, &s_nonceCounter, 4);
  randombytes_buf(nonce + 4, NONCE_LEN - 4);

  unsigned long long clen;
  int r = crypto_aead_chacha20poly1305_ietf_encrypt(out + NONCE_LEN, &clen, plain, len, nullptr, 0, nullptr,
                                                    nonce, s_key);
  if (r != 0) return false;

  memcpy(out, nonce, NONCE_LEN);
  *outLen = NONCE_LEN + clen;
  s_nonceCounter++;
  /* Паритет с firmware/src/crypto/crypto.cpp (ESP): не persist nonce_ctr на каждый encrypt —
   * только при init (дефолтный ключ) и setChannelKey. Иначе InternalFS на каждом TX блокирует CPU
   * и давит SoftDevice/BLE; это не связано с записью peer-key (она только в RAM, см. x25519_keys). */
  return true;
}

bool decrypt(const uint8_t* cipher, size_t len, uint8_t* out, size_t* outLen) {
  if (!s_inited || len < NONCE_LEN + TAG_LEN) return false;

  const uint8_t* nonce = cipher;
  const uint8_t* c = cipher + NONCE_LEN;
  size_t clen = len - NONCE_LEN;

  unsigned long long mlen;
  int r = crypto_aead_chacha20poly1305_ietf_decrypt(out, &mlen, nullptr, c, clen, nullptr, 0, nonce, s_key);
  if (r != 0) return false;
  *outLen = (size_t)mlen;
  return true;
}

static bool encryptWithKey(const uint8_t* key, const uint8_t* plain, size_t len, uint8_t* out, size_t* outLen) {
  if (len > 16384) return false;
  size_t need = NONCE_LEN + len + TAG_LEN;
  if (outLen && *outLen < need) return false;

  uint8_t nonce[NONCE_LEN];
  memset(nonce, 0, NONCE_LEN);
  randombytes_buf(nonce, NONCE_LEN);

  unsigned long long clen;
  int r = crypto_aead_chacha20poly1305_ietf_encrypt(out + NONCE_LEN, &clen, plain, len, nullptr, 0, nullptr,
                                                    nonce, key);
  if (r != 0) return false;

  memcpy(out, nonce, NONCE_LEN);
  *outLen = NONCE_LEN + clen;
  return true;
}

static bool decryptWithKey(const uint8_t* key, const uint8_t* cipher, size_t len, uint8_t* out, size_t* outLen) {
  if (len < NONCE_LEN + TAG_LEN) return false;
  const uint8_t* nonce = cipher;
  const uint8_t* c = cipher + NONCE_LEN;
  size_t clen = len - NONCE_LEN;

  unsigned long long mlen;
  int r = crypto_aead_chacha20poly1305_ietf_decrypt(out, &mlen, nullptr, c, clen, nullptr, 0, nonce, key);
  if (r != 0) return false;
  *outLen = (size_t)mlen;
  return true;
}

bool encryptFor(const uint8_t* peerId, const uint8_t* plain, size_t len, uint8_t* out, size_t* outLen) {
  if (!s_inited || !outLen) return false;

  uint8_t peerKey[32];
  if (peerId && x25519_keys::getKeyFor(peerId, peerKey)) {
    return encryptWithKey(peerKey, plain, len, out, outLen);
  }
  return false;
}

bool decryptFrom(const uint8_t* senderId, const uint8_t* cipher, size_t len, uint8_t* out, size_t* outLen) {
  if (!s_inited || !outLen) return false;

  uint8_t peerKey[32];
  if (senderId && x25519_keys::getKeyFor(senderId, peerKey)) {
    if (decryptWithKey(peerKey, cipher, len, out, outLen)) return true;
  }
  return decrypt(cipher, len, out, outLen);
}

bool encryptWithGroupKey(const uint8_t* key32, const uint8_t* plain, size_t len, uint8_t* out, size_t* outLen) {
  if (!s_inited || !key32 || !outLen) return false;
  return encryptWithKey(key32, plain, len, out, outLen);
}

bool decryptWithGroupKey(const uint8_t* key32, const uint8_t* cipher, size_t len, uint8_t* out, size_t* outLen) {
  if (!s_inited || !key32 || !outLen) return false;
  return decryptWithKey(key32, cipher, len, out, outLen);
}

bool setChannelKey(const uint8_t* key) {
  if (!key) return false;
  memcpy(s_key, key, KEY_LEN);
  s_nonceCounter = 0;
  storage::setBlob(STORAGE_KEY_CHANNEL, s_key, KEY_LEN);
  storage::setU32(STORAGE_NONCE_CTR, s_nonceCounter);
  return true;
}

bool getChannelKey(uint8_t* out) {
  if (!s_inited || !out) return false;
  memcpy(out, s_key, KEY_LEN);
  return true;
}

static size_t base64Encode32(const uint8_t* src, size_t len, char* out, size_t cap) {
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = (uint32_t)src[i] << 16;
    if (i + 1 < len) v |= (uint32_t)src[i + 1] << 8;
    if (i + 2 < len) v |= (uint32_t)src[i + 2];
    size_t rem = len - i;
    if (o + 4 >= cap) return 0;
    out[o++] = tbl[(v >> 18) & 63];
    out[o++] = tbl[(v >> 12) & 63];
    if (rem >= 3) {
      out[o++] = tbl[(v >> 6) & 63];
      out[o++] = tbl[v & 63];
    } else if (rem == 2) {
      out[o++] = tbl[(v >> 6) & 63];
      out[o++] = '=';
    } else {
      out[o++] = '=';
      out[o++] = '=';
    }
  }
  if (o < cap) out[o] = '\0';
  return o;
}

bool getInvitePublicKeyBase64(char* out, size_t cap) {
  if (!out || cap < 48) return false;
  uint8_t pub[X25519_PUBKEY_LEN];
  if (!x25519_keys::getOurPublicKey(pub)) return false;
  return base64Encode32(pub, sizeof(pub), out, cap) > 0;
}

bool base64Encode(const uint8_t* data, size_t dataLen, char* out, size_t cap, size_t* outWritten) {
  if (!data || !out) return false;
  const size_t need = ((dataLen + 2u) / 3u) * 4u + 1u;
  if (cap < need) return false;
  const size_t n = base64Encode32(data, dataLen, out, cap);
  if (n == 0) return false;
  if (outWritten) *outWritten = n;
  return true;
}

static int8_t b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return (int8_t)(c - 'A');
  if (c >= 'a' && c <= 'z') return (int8_t)(26 + (c - 'a'));
  if (c >= '0' && c <= '9') return (int8_t)(52 + (c - '0'));
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool base64Decode(const char* in, size_t inLen, uint8_t* out, size_t maxOut, size_t* outLen) {
  if (!in || !out || !outLen) return false;
  *outLen = 0;
  int val = 0;
  int valb = -8;
  size_t op = 0;
  for (size_t i = 0; i < inLen; i++) {
    const char c = in[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    if (c == '=') break;
    const int8_t d = b64Value(c);
    if (d < 0) return false;
    val = (val << 6) + d;
    valb += 6;
    if (valb >= 0) {
      if (op >= maxOut) return false;
      out[op++] = (uint8_t)((val >> valb) & 0xFF);
      valb -= 8;
    }
  }
  *outLen = op;
  return true;
}

}  // namespace crypto
