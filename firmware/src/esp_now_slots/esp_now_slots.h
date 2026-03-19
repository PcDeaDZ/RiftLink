/**
 * ESP-NOW Slot Negotiation — 50–250 м
 * Slot negotiation через ESP-NOW без LoRa.
 * Формат RTS совместим с BLS-N: company 0x524C, "RTS", from(4), to(4), len(2), txAt(4).
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

namespace esp_now_slots {

void init();

/** Перед LoRa TX: отправить RTS по ESP-NOW (broadcast). Возвращает true если RTS отправлен. */
bool sendRtsBeforeLora(const uint8_t* to, size_t payloadLen);

/** Проверить: нужно ли отложить TX (получен RTS от другого). */
bool shouldDeferTx(const uint8_t* to);

/** Добавить полученный RTS в кэш (вызов из ESP-NOW recv callback). */
void addReceivedRts(const uint8_t* from, const uint8_t* to, uint16_t len, uint32_t txAt);

}  // namespace esp_now_slots
