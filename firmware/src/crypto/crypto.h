/**
 * Crypto Layer — ChaCha20-Poly1305 (libsodium)
 * Сеть: общий ключ. Unicast: X25519 per-peer (E2E).
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace crypto {

bool init();
bool encrypt(const uint8_t* plain, size_t len, uint8_t* out, size_t* outLen);
bool decrypt(const uint8_t* cipher, size_t len, uint8_t* out, size_t* outLen);

/** Шифрование для peer (unicast E2E). При отсутствии ключа — fallback на сетевой */
bool encryptFor(const uint8_t* peerId, const uint8_t* plain, size_t len, uint8_t* out, size_t* outLen);

/** Расшифровка от sender. Сначала per-peer, затем сетевой ключ */
bool decryptFrom(const uint8_t* senderId, const uint8_t* cipher, size_t len, uint8_t* out, size_t* outLen);

// Размер overhead: nonce 12 + tag 16 = 28 байт
constexpr size_t OVERHEAD = 12 + 16;

}  // namespace crypto
