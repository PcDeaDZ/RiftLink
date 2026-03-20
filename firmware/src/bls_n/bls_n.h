/**
 * BLS-N — BLE-LoRa Slot Negotiation
 * BLE как control channel для резервирования LoRa-слота (1–50 м).
 * RTS через manufacturer data в advertising (без телефона) + BLE scan при подключённом телефоне.
 *
 * Сборка с -DRIFTLINK_DISABLE_BLS_N: без RTS/слотов по BLE (ble.cpp не запускает BLS-scan).
 */

#pragma once

#include <cstdint>
#include "protocol/packet.h"

namespace bls_n {

#if defined(RIFTLINK_DISABLE_BLS_N)

inline void init() {}
inline bool sendRtsBeforeLora(const uint8_t*, size_t) { return false; }
inline bool shouldDeferTx(const uint8_t*) { return false; }
inline void addReceivedRts(const uint8_t*, const uint8_t*, uint16_t, uint32_t) {}

#else

void init();

/** Перед LoRa TX: отправить RTS по BLE (если в радиусе). Возвращает true если RTS отправлен. */
bool sendRtsBeforeLora(const uint8_t* to, size_t payloadLen);

/** Проверить: нужно ли отложить TX (получен RTS от другого). */
bool shouldDeferTx(const uint8_t* to);

/** Добавить полученный RTS в кэш (вызов из BLE scan callback). */
void addReceivedRts(const uint8_t* from, const uint8_t* to, uint16_t len, uint32_t txAt);

#endif

}  // namespace bls_n
