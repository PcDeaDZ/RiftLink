/**
 * FakeTech Crypto — ChaCha20-Poly1305 (libsodium), паритет с firmware/src/crypto (Heltec V3).
 * Сеть: общий ключ. Unicast: X25519 per-peer (E2E).
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace crypto {

bool init();
bool encrypt(const uint8_t* plain, size_t len, uint8_t* out, size_t* outLen);
bool decrypt(const uint8_t* cipher, size_t len, uint8_t* out, size_t* outLen);

bool encryptFor(const uint8_t* peerId, const uint8_t* plain, size_t len, uint8_t* out, size_t* outLen);
bool decryptFrom(const uint8_t* senderId, const uint8_t* cipher, size_t len, uint8_t* out, size_t* outLen);

bool encryptWithGroupKey(const uint8_t* key32, const uint8_t* plain, size_t len, uint8_t* out, size_t* outLen);
bool decryptWithGroupKey(const uint8_t* key32, const uint8_t* cipher, size_t len, uint8_t* out, size_t* outLen);

bool setChannelKey(const uint8_t* key);
bool getChannelKey(uint8_t* out);

/** Base64 публичного ключа X25519 для evt:invite (32 B сырого ключа). */
bool getInvitePublicKeyBase64(char* out, size_t cap);

/** Универсальный base64 (без переносов), паритет с mbedtls_base64_* на ESP. */
bool base64Encode(const uint8_t* data, size_t dataLen, char* out, size_t cap, size_t* outWritten);
bool base64Decode(const char* in, size_t inLen, uint8_t* out, size_t maxOut, size_t* outLen);

/** nonce 12 + tag 16 (как на ESP) */
constexpr size_t OVERHEAD = 12 + 16;

}  // namespace crypto
