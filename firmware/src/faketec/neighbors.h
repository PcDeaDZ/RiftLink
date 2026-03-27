/**
 * FakeTech Neighbors — упрощённый список соседей
 */

#pragma once

#include <cstdint>
#include "protocol/packet.h"

#define NEIGHBORS_MAX 16
#define NEIGHBOR_TIMEOUT_MS 180000

namespace neighbors {

void init();
bool onHello(const uint8_t* nodeId, int rssi = 0);
void updateRssi(const uint8_t* nodeId, int rssi);
void updateBattery(const uint8_t* nodeId, uint16_t batteryMv);
int getCount();
int getRssi(int i);
bool getId(int i, uint8_t* out);

int getRssiFor(const uint8_t* nodeId);
int getMinRssi();
int getAverageRssi();
bool isOnline(const uint8_t* nodeId);
uint32_t getFreshnessMs(const uint8_t* nodeId);
int getBatteryMv(const uint8_t* nodeId);
void recordAckSent(const uint8_t* nodeId);
void recordAckReceived(const uint8_t* nodeId);
int getAckRatePermille(const uint8_t* nodeId);

inline uint8_t rssiToSf(int rssi) {
  if (rssi <= -128 || rssi > 0) return 0;
  if (rssi >= -75) return 7;
  if (rssi >= -85) return 9;
  if (rssi >= -95) return 10;
  if (rssi >= -105) return 11;
  return 12;
}
uint8_t rssiToSfOrthogonal(const uint8_t* nodeId);

}  // namespace neighbors
