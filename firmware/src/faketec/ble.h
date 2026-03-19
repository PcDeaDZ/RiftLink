/**
 * FakeTech BLE — GATT для приложения (ArduinoBLE)
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace ble {

bool init();
void update();

void setOnSend(void (*cb)(const uint8_t* to, const char* text, uint8_t ttlMinutes));
void notifyMsg(const uint8_t* from, const char* text, uint32_t msgId = 0, int rssi = 0, uint8_t ttlMinutes = 0);
void notifyInfo();
void notifyNeighbors();
void notifyError(const char* code, const char* msg);
bool isConnected();

}  // namespace ble
