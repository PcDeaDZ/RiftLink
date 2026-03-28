/**
 * RiftLink Crypto — ChaCha20-Poly1305 (libsodium)
 * Сеть: общий ключ. Unicast: X25519 per-peer (E2E).
 */

#include "crypto/crypto.h"
#include "x25519_keys/x25519_keys.h"
#include "kv.h"
#include <Arduino.h>
#include <nrf_error.h>
#include <nrf_soc.h>
#include <sodium.h>
#include <string.h>
#include "port/rtos_include.h"

/*
 * esphome/libsodium по умолчанию использует randombytes_sysrandom → open("/dev/urandom").
 * На bare-metal nRF52 этого нет — read() может висеть бесконечно; sodium_init() тогда
 * останавливается на randombytes_stir(). Регистрируем выдачу байт через SoftDevice.
 */
extern "C" {

static const char* riftlink_rng_name(void) { return "nrf_sd_rand"; }

static void riftlink_rng_buf(void* const buf, const size_t size) {
  auto* p = static_cast<uint8_t*>(buf);
  size_t left = size;
  while (left > 0U) {
    /* S140: за один вызов — не больше 16 байт (см. документацию sd_rand_application_vector_get). */
    uint8_t chunk = (left > 16U) ? 16U : static_cast<uint8_t>(left);
    uint32_t err;
    do {
      err = sd_rand_application_vector_get(p, chunk);
      if (err == NRF_ERROR_SOC_RAND_NOT_ENOUGH_VALUES) {
        yield();
      }
    } while (err == NRF_ERROR_SOC_RAND_NOT_ENOUGH_VALUES);
    if (err != NRF_SUCCESS) {
      for (uint8_t i = 0; i < chunk; i++) {
        p[i] = static_cast<uint8_t>(micros() >> ((i & 7U) * 4U));
      }
    }
    p += chunk;
    left -= chunk;
  }
}

static uint32_t riftlink_rng_random(void) {
  uint32_t v;
  riftlink_rng_buf(&v, sizeof v);
  return v;
}

static void riftlink_rng_stir(void) {}

static int riftlink_rng_close(void) { return 0; }

static void riftlink_install_libsodium_randombytes_nrf52(void) {
  static const randombytes_implementation impl = {
      riftlink_rng_name,
      riftlink_rng_random,
      riftlink_rng_stir,
      nullptr,
      riftlink_rng_buf,
      riftlink_rng_close,
  };
  (void)randombytes_set_implementation(&impl);
}

}  // extern "C"

#define NVS_NAMESPACE "riftlink"
#define MUTEX_TIMEOUT_MS 50
#define NVS_KEY_CHANNEL "chkey"
#define KEY_LEN crypto_aead_chacha20poly1305_ietf_KEYBYTES
#define NONCE_LEN crypto_aead_chacha20poly1305_ietf_NPUBBYTES
#define TAG_LEN crypto_aead_chacha20poly1305_ietf_ABYTES

static uint8_t s_key[KEY_LEN];
static uint32_t s_nonceCounter = 0;
static bool s_inited = false;
static SemaphoreHandle_t s_mutex = nullptr;

namespace crypto {

bool init() {
  if (s_inited) return true;

  // Чтение KV до sodium_init: при подвисании LittleFS будет видно до libsodium; ChaCha IETF ключ = 32 байта.
  Serial.println("[RiftLink] crypto: KV read chkey/nonce (before sodium)");
  Serial.flush();
  if (!riftlink_kv::is_ready()) {
    Serial.println("[RiftLink] crypto: KV not ready");
    Serial.flush();
    return false;
  }
  size_t len = KEY_LEN;
  const bool hasKey = riftlink_kv::getBlob(NVS_KEY_CHANNEL, s_key, &len) && len == KEY_LEN;
  uint32_t nonceFromKv = 0;
  if (hasKey) {
    (void)riftlink_kv::getU32("nonce_ctr", &nonceFromKv);
  }
  Serial.printf("[RiftLink] crypto: KV done has_chkey=%d\n", hasKey ? 1 : 0);
  Serial.flush();

  Serial.println("[RiftLink] crypto: randombytes -> SoftDevice (sd_rand), not /dev/urandom");
  Serial.flush();
  riftlink_install_libsodium_randombytes_nrf52();

  Serial.println("[RiftLink] crypto: sodium_init...");
  Serial.flush();
  if (sodium_init() < 0) {
    Serial.println("[RiftLink] crypto: sodium_init FAILED");
    Serial.flush();
    return false;
  }
  Serial.println("[RiftLink] crypto: sodium_init ok");
  Serial.flush();

  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) {
    Serial.println("[RiftLink] crypto: mutex create failed");
    Serial.flush();
    return false;
  }

  if (hasKey) {
    s_nonceCounter = nonceFromKv;
    s_inited = true;
    return true;
  }

  // Дефолтный ключ канала (при первом запуске). Смена через BLE: channelKey, invite/acceptInvite.
  static const uint8_t DEFAULT_KEY[KEY_LEN] = {
    0x52, 0x69, 0x66, 0x74, 0x4c, 0x69, 0x6e, 0x6b,
    0x2d, 0x52, 0x4c, 0x2d, 0x63, 0x68, 0x61, 0x6e,
    0x6e, 0x65, 0x6c, 0x2d, 0x6b, 0x65, 0x79, 0x2d,
    0x32, 0x30, 0x32, 0x35, 0x21, 0x21, 0x21, 0x21
  };
  memcpy(s_key, DEFAULT_KEY, KEY_LEN);
  s_nonceCounter = 0;

  Serial.println("[RiftLink] crypto: writing default chkey to KV");
  Serial.flush();
  (void)riftlink_kv::setBlob(NVS_KEY_CHANNEL, s_key, KEY_LEN);
  (void)riftlink_kv::setU32("nonce_ctr", s_nonceCounter);
  s_inited = true;
  return true;
}

bool encrypt(const uint8_t* plain, size_t len, uint8_t* out, size_t* outLen) {
  if (!s_inited || len > 4096) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;

  size_t need = NONCE_LEN + len + TAG_LEN;
  if (outLen && *outLen < need) { xSemaphoreGive(s_mutex); return false; }

  uint8_t nonce[NONCE_LEN];
  memset(nonce, 0, NONCE_LEN);
  memcpy(nonce, &s_nonceCounter, 4);
  randombytes_buf(nonce + 4, NONCE_LEN - 4);

  unsigned long long clen;
  int r = crypto_aead_chacha20poly1305_ietf_encrypt(
      out + NONCE_LEN, &clen,
      plain, len,
      nullptr, 0,  // no additional data
      nullptr, nonce, s_key);

  if (r != 0) { xSemaphoreGive(s_mutex); return false; }

  memcpy(out, nonce, NONCE_LEN);
  *outLen = NONCE_LEN + clen;

  s_nonceCounter++;
  xSemaphoreGive(s_mutex);
  return true;
}

bool decrypt(const uint8_t* cipher, size_t len, uint8_t* out, size_t* outLen) {
  if (!s_inited || len < NONCE_LEN + TAG_LEN) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;

  const uint8_t* nonce = cipher;
  const uint8_t* c = cipher + NONCE_LEN;
  size_t clen = len - NONCE_LEN;

  unsigned long long mlen;
  int r = crypto_aead_chacha20poly1305_ietf_decrypt(
      out, &mlen, nullptr,
      c, clen,
      nullptr, 0,
      nonce, s_key);

  xSemaphoreGive(s_mutex);
  if (r != 0) return false;
  *outLen = (size_t)mlen;
  return true;
}

static bool encryptWithKey(const uint8_t* key, const uint8_t* plain, size_t len,
                           uint8_t* out, size_t* outLen) {
  if (len > 16384) return false;
  size_t need = NONCE_LEN + len + TAG_LEN;
  if (outLen && *outLen < need) return false;

  uint8_t nonce[NONCE_LEN];
  memset(nonce, 0, NONCE_LEN);
  randombytes_buf(nonce, NONCE_LEN);

  unsigned long long clen;
  int r = crypto_aead_chacha20poly1305_ietf_encrypt(
      out + NONCE_LEN, &clen,
      plain, len, nullptr, 0, nullptr, nonce, key);
  if (r != 0) return false;

  memcpy(out, nonce, NONCE_LEN);
  *outLen = NONCE_LEN + clen;
  return true;
}

static bool decryptWithKey(const uint8_t* key, const uint8_t* cipher, size_t len,
                          uint8_t* out, size_t* outLen) {
  if (len < NONCE_LEN + TAG_LEN) return false;
  const uint8_t* nonce = cipher;
  const uint8_t* c = cipher + NONCE_LEN;
  size_t clen = len - NONCE_LEN;

  unsigned long long mlen;
  int r = crypto_aead_chacha20poly1305_ietf_decrypt(
      out, &mlen, nullptr, c, clen, nullptr, 0, nonce, key);
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
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;

  memcpy(s_key, key, KEY_LEN);
  s_nonceCounter = 0;

  (void)riftlink_kv::setBlob(NVS_KEY_CHANNEL, s_key, KEY_LEN);
  (void)riftlink_kv::setU32("nonce_ctr", s_nonceCounter);
  xSemaphoreGive(s_mutex);
  return true;
}

bool getChannelKey(uint8_t* out) {
  if (!s_inited || !out) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;
  memcpy(out, s_key, KEY_LEN);
  xSemaphoreGive(s_mutex);
  return true;
}

}  // namespace crypto
