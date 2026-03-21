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

/** Шифрование для peer (unicast E2E). Требует pairwise-ключ, fallback запрещен. */
bool encryptFor(const uint8_t* peerId, const uint8_t* plain, size_t len, uint8_t* out, size_t* outLen);

/** Расшифровка от sender. Сначала per-peer, затем сетевой ключ */
bool decryptFrom(const uint8_t* senderId, const uint8_t* cipher, size_t len, uint8_t* out, size_t* outLen);

/** Шифрование/дешифрование произвольным 32-байт ключом (например, private group key). */
bool encryptWithGroupKey(const uint8_t* key32, const uint8_t* plain, size_t len, uint8_t* out, size_t* outLen);
bool decryptWithGroupKey(const uint8_t* key32, const uint8_t* cipher, size_t len, uint8_t* out, size_t* outLen);

/** Установить ключ канала (32 байта), сохранить в NVS, сбросить nonce_ctr */
bool setChannelKey(const uint8_t* key);

/** Получить текущий ключ канала (32 байта) для экспорта в QR/BLE evt */
bool getChannelKey(uint8_t* out);

// Размер overhead: nonce 12 + tag 16 = 28 байт
constexpr size_t OVERHEAD = 12 + 16;

}  // namespace crypto
