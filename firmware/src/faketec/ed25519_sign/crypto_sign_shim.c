/**
 * Верхний уровень libsodium crypto_sign_* — в esphome/libsodium для nRF не собираются.
 * Делегируем в crypto_sign_ed25519_* (keypair.c / sign.c / open.c + SHA-512).
 */

#include <sodium/crypto_sign.h>
#include <sodium/crypto_sign_ed25519.h>

int crypto_sign_detached(unsigned char* sig, unsigned long long* siglen_p, const unsigned char* m,
    unsigned long long mlen, const unsigned char* sk) {
  return crypto_sign_ed25519_detached(sig, siglen_p, m, mlen, sk);
}

int crypto_sign_verify_detached(const unsigned char* sig, const unsigned char* m, unsigned long long mlen,
    const unsigned char* pk) {
  return crypto_sign_ed25519_verify_detached(sig, m, mlen, pk);
}

int crypto_sign_keypair(unsigned char* pk, unsigned char* sk) { return crypto_sign_ed25519_keypair(pk, sk); }

int crypto_sign_seed_keypair(unsigned char* pk, unsigned char* sk, const unsigned char* seed) {
  return crypto_sign_ed25519_seed_keypair(pk, sk, seed);
}
