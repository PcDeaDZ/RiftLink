/**
 * FakeTech Radio — LoRa SX1262 (HT-RA62 / RA-01SH)
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace radio {

bool init();
bool send(const uint8_t* data, size_t len, uint8_t txSf = 0, bool priority = false);
int receive(uint8_t* buf, size_t maxLen);
int getLastRssi();
void applyRegion(float freq, int power);
void setSpreadingFactor(uint8_t sf);
uint8_t getSpreadingFactor();
uint32_t getTimeOnAir(size_t len);
bool isChannelFree();

}  // namespace radio
