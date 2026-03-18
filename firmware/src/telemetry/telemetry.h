/**
 * RiftLink — телеметрия (батарея, память)
 * OP_TELEMETRY, broadcast
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace telemetry {

// Payload: battery_mV (uint16), heapKb (uint16) = 4 bytes
constexpr size_t TELEM_PAYLOAD_LEN = 4;

void init();
void send();  // Отправка broadcast TELEMETRY

// Чтение батареи (mV), 0 если недоступно
uint16_t readBatteryMv();

}  // namespace telemetry
