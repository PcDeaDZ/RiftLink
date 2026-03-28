#pragma once

#include <cstddef>
#include <cstdint>

void handlePacket(const uint8_t* buf, size_t len, int rssi, uint8_t sf);
