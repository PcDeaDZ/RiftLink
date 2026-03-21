/**
 * X25519 — ключи на пару узлов, E2E
 * libsodium crypto_box (X25519 + XSalsa20-Poly1305 beforenm → ChaCha20-Poly1305)
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

#define X25519_PUBKEY_LEN 32
#define X25519_MAX_PEERS 16

namespace x25519_keys {

void init();

/** Получить наш публичный ключ (32 байта) */
bool getOurPublicKey(uint8_t* out);

/** Обработать KEY_EXCHANGE от peer: сохранить их pub_key, вычислить shared secret */
void onKeyExchange(const uint8_t* peerId, const uint8_t* theirPubKey);
/** Проверка pinning: true, если для peer уже есть известный pubKey и пришёл другой. */
bool isPeerPubKeyMismatch(const uint8_t* peerId, const uint8_t* theirPubKey);

/** Есть ли shared secret для peer? */
bool hasKeyFor(const uint8_t* peerId);

/** Получить ключ для peer (32 байта). Возвращает false если нет */
bool getKeyFor(const uint8_t* peerId, uint8_t* keyOut);

/** Отправить наш публичный ключ peer (TX всегда на текущем mesh SF из NVS). forceSend → ответ на KEY_EXCHANGE. hadKeyBefore → уже был ключ, не спамить ответ (длинный троттл). reason — метка в логе (hello, key_rx, retry, ble, …). */
void sendKeyExchange(const uint8_t* peerId, bool forceSend = false, bool hadKeyBefore = false, const char* reason = nullptr);

/** Последний момент, когда KEY_EXCHANGE был реально поставлен в TX (millis). */
uint32_t getLastKeyTxReadyMs();

}  // namespace x25519_keys
