/**
 * Ed25519 ключ владельца группы (libsodium) — паритет с NVS на ESP (ble.cpp).
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace group_owner_sign {

bool init();
bool ready();

const uint8_t* publicKey32();
const uint8_t* secretKey64();

constexpr size_t SIGNATURE_LEN = 64;

bool signDetached(const uint8_t* msg, size_t msgLen, uint8_t* sig64);

bool verifyDetached(const uint8_t* sig64, const uint8_t* msg, size_t msgLen, const uint8_t* pk32);

}  // namespace group_owner_sign
