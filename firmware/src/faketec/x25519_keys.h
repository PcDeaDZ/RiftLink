/**
 * X25519 — per-peer ключи (libsodium crypto_box_beforenm → ChaCha), паритет с firmware/src/x25519_keys.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

#define X25519_PUBKEY_LEN 32
#define X25519_MAX_PEERS 16

namespace x25519_keys {

void init();

bool getOurPublicKey(uint8_t* out);

void onKeyExchange(const uint8_t* peerId, const uint8_t* theirPubKey);
/** true, если для peer уже сохранён pubKey и в эфире другой (ротация ключа у пира). */
bool isPeerPubKeyMismatch(const uint8_t* peerId, const uint8_t* theirPubKey);

bool hasKeyFor(const uint8_t* peerId);
bool getKeyFor(const uint8_t* peerId, uint8_t* keyOut);

void sendKeyExchange(const uint8_t* peerId, bool forceSend = false, bool hadKeyBefore = false,
                     const char* reason = nullptr);

uint32_t getLastKeyTxReadyMs();

}  // namespace x25519_keys
