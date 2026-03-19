/**
 * FakeTech Neighbors — упрощённый список соседей
 */

#pragma once

#include <cstdint>
#include "protocol/packet.h"

#define NEIGHBORS_MAX 8
#define NEIGHBOR_TIMEOUT_MS 120000

namespace neighbors {

void init();
bool onHello(const uint8_t* nodeId, int rssi = 0);
void updateRssi(const uint8_t* nodeId, int rssi);
int getCount();
int getRssi(int i);
bool getId(int i, uint8_t* out);

}  // namespace neighbors
