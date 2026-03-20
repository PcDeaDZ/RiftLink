/**
 * ESP-NOW Slot Negotiation — 50–250 м
 * Slot negotiation через ESP-NOW без LoRa.
 * Формат RTS совместим с BLS-N: company 0x524C, "RTS", from(4), to(4), len(2), txAt(4).
 *
 * Сборка с -DRIFTLINK_DISABLE_ESP_NOW: без esp_now_* (нет RTS/слотов по Wi‑Fi), остальной код без изменений.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

namespace esp_now_slots {

#if defined(RIFTLINK_DISABLE_ESP_NOW)

inline void init() {}
inline void deinit() {}
inline uint8_t getChannel() { return 6; }
inline bool setChannel(uint8_t) { return false; }
inline bool isAdaptive() { return false; }
inline bool setAdaptive(bool) { return false; }
inline void tickAdaptive() {}
inline bool sendRtsBeforeLora(const uint8_t*, size_t) { return false; }
inline bool shouldDeferTx(const uint8_t*) { return false; }
inline void addReceivedRts(const uint8_t*, const uint8_t*, uint16_t, uint32_t) {}

#else

void init();
/** Деинициализация ESP-NOW: esp_now_deinit, сброс peer-ов и кэша. */
void deinit();
/** Текущий WiFi канал ESP-NOW (1–13). По умолчанию 6. */
uint8_t getChannel();
/** Установить канал (1–13). Требует перезапуска ESP-NOW. Возвращает true при успехе. */
bool setChannel(uint8_t ch);
/** Режим: true = адаптивный (scan 1,6,11 → выбор наименее загруженного), false = фиксированный. */
bool isAdaptive();
bool setAdaptive(bool on);
/** Вызвать из loop: периодический re-scan при adaptive (раз в 5 мин). */
void tickAdaptive();

/** Перед LoRa TX: отправить RTS по ESP-NOW (broadcast). Возвращает true если RTS отправлен. */
bool sendRtsBeforeLora(const uint8_t* to, size_t payloadLen);

/** Проверить: нужно ли отложить TX (получен RTS от другого). */
bool shouldDeferTx(const uint8_t* to);

/** Добавить полученный RTS в кэш (вызов из ESP-NOW recv callback). */
void addReceivedRts(const uint8_t* from, const uint8_t* to, uint16_t len, uint32_t txAt);

#endif

}  // namespace esp_now_slots
