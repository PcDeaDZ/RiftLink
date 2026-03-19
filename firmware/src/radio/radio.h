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
/** txSf: 0 = текущий baseSf, 7–12 = принудительный SF (per-neighbor) */
bool send(const uint8_t* data, size_t len, uint8_t txSf = 0, bool priority = false);
/** Отправка на радио (с mutex). Для drain — takeMutex, sendDirectInternal, releaseMutex */
bool sendDirect(const uint8_t* data, size_t len);
bool sendDirectInternal(const uint8_t* data, size_t len);  // без mutex — вызывать между take/release
bool takeMutex(TickType_t timeout);
void releaseMutex();
int receive(uint8_t* buf, size_t maxLen);
/** Асинхронный приём: startReceiveWithTimeout + readData. Для power save с light sleep. */
bool startReceiveWithTimeout(uint32_t timeoutMs);
int receiveAsync(uint8_t* buf, size_t maxLen);
/** RSSI последнего принятого пакета (dBm), 0 если недоступно */
int getLastRssi();
void applyRegion(float freq, int power);  // применить региональные параметры
void setSpreadingFactor(uint8_t sf);     // 7–12, для адаптивного SF
uint8_t getSpreadingFactor();            // текущий SF (для info/evt)
uint32_t getTimeOnAir(size_t len);       // мкс, для duty cycle
/** CAD — канал свободен? Перед retry для снижения коллизий */
bool isChannelFree();

}  // namespace radio
