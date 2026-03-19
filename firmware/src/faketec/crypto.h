/**
 * FakeTech Crypto — ChaCha20-Poly1305 (упрощённо: pass-through для первой версии)
 * TODO: добавить libsodium или mbedTLS
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
bool setChannelKey(const uint8_t* key);
bool getChannelKey(uint8_t* out);

constexpr size_t OVERHEAD = 0;  // Пока без шифрования

}  // namespace crypto
