#include "group_owner_sign.h"
#include "storage.h"

extern "C" {
#include "sodium/crypto_sign.h"
}

#include <string.h>

namespace {

constexpr const char* kNvsPk = "gospk";
constexpr const char* kNvsSk = "gossk";

uint8_t s_pk[crypto_sign_PUBLICKEYBYTES];
uint8_t s_sk[crypto_sign_SECRETKEYBYTES];
bool s_ready = false;

}  // namespace

namespace group_owner_sign {

bool init() {
  if (s_ready) return true;
  size_t pkLen = sizeof(s_pk);
  size_t skLen = sizeof(s_sk);
  const bool hasPk = storage::getBlob(kNvsPk, s_pk, &pkLen) && pkLen == sizeof(s_pk);
  const bool hasSk = storage::getBlob(kNvsSk, s_sk, &skLen) && skLen == sizeof(s_sk);
  if (hasPk && hasSk) {
    s_ready = true;
    return true;
  }
  if (crypto_sign_keypair(s_pk, s_sk) != 0) return false;
  if (!storage::setBlob(kNvsPk, s_pk, sizeof(s_pk))) return false;
  if (!storage::setBlob(kNvsSk, s_sk, sizeof(s_sk))) return false;
  s_ready = true;
  return true;
}

bool ready() { return s_ready; }

const uint8_t* publicKey32() { return s_pk; }

const uint8_t* secretKey64() { return s_sk; }

bool signDetached(const uint8_t* msg, size_t msgLen, uint8_t* sig64) {
  if (!s_ready || !msg || !sig64) return false;
  unsigned long long sigLen = 0;
  if (crypto_sign_detached(sig64, &sigLen, msg, (unsigned long long)msgLen, s_sk) != 0) return false;
  return sigLen == crypto_sign_BYTES;
}

bool verifyDetached(const uint8_t* sig64, const uint8_t* msg, size_t msgLen, const uint8_t* pk32) {
  if (!sig64 || !msg || !pk32) return false;
  return crypto_sign_verify_detached(sig64, msg, (unsigned long long)msgLen, pk32) == 0;
}

}  // namespace group_owner_sign
