/**
 * Radio Layer — LoRa (SX1262)
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <freertos/FreeRTOS.h>

namespace radio {

bool init();
void setAsyncMode(bool on);
/** TX только через очередь → `radioSchedulerTask` (без синхронного SPI из loop/packetTask).
 * txSf 7–12 — SF пакета; 0 — mesh SF. reasonBuf — причина отказа (HELLO drop и т.п.). */
bool send(const uint8_t* data, size_t len, uint8_t txSf = 0, bool priority = false,
    char* reasonBuf = nullptr, size_t reasonLen = 0);
/** Устаревшее имя: то же, что `send(..., priority=true)` — только очередь, без прямого TX. */
bool sendDirect(const uint8_t* data, size_t len, char* reasonBuf = nullptr, size_t reasonLen = 0);
/** Только из `radioSchedulerTask` под `takeMutex` — не вызывать из прикладного кода. */
bool sendDirectInternal(const uint8_t* data, size_t len, char* reasonBuf = nullptr, size_t reasonLen = 0);
/** Только: планировщик радио, E-Ink SPI (Paper). CAD — внутри `sendDirectInternal`. */
bool takeMutex(TickType_t timeout);
void releaseMutex();
/** Планировщик: между startReceive и receiveAsync mutex отпускается на delay/sleep. */
void setRxListenActive(bool on);
/** Только под `takeMutex`: перевести SX1262 в standby (перед переключением SPI на E-Ink). */
void standbyChipUnderMutex();
int receive(uint8_t* buf, size_t maxLen);
/** Асинхронный приём: startReceiveWithTimeout + readData. Для power save с light sleep. */
bool startReceiveWithTimeout(uint32_t timeoutMs);
int receiveAsync(uint8_t* buf, size_t maxLen);
/** RSSI последнего принятого пакета (dBm), 0 если недоступно */
int getLastRssi();
void applyRegion(float freq, int power);  // низкоуровнево; из приложения — requestApplyRegion
/** Смена частоты/мощности из BLE/UI — постановка в `radioCmdQueue` (применение в планировщике под mutex). */
void requestApplyRegion(float freq, int power);
/** Mesh SF: NVS + чип. Только из BLE ApplySf / явной смены — не вызывать из TX с rssiToSf. */
void setSpreadingFactor(uint8_t sf);
/** Только чип (планировщик RX/TX) — не трогает mesh и NVS. */
void applyHardwareSpreadingFactor(uint8_t sf);
/** Запрос смены SF из BLE/UI — постановка в `radioCmdQueue`. */
void requestSpreadingFactor(uint8_t sf);
uint8_t getSpreadingFactor();            // текущий SF (для info/evt)
uint32_t getTimeOnAir(size_t len);       // мкс, для duty cycle
/** CAD (mutex + SPI). Предпочтительно не вызывать из прикладного кода — CSMA в планировщике при TX. */
bool isChannelFree();
/** Сигнал перегрузки (NACK, undelivered) — увеличивает BEB CW для следующих TX */
void notifyCongestion();

}  // namespace radio
