/**
 * Radio Layer — LoRa (SX1262)
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <freertos/FreeRTOS.h>

namespace radio {

enum ModemPreset : uint8_t {
  MODEM_SPEED     = 0,  // SF7  BW250 CR5 — город, скорость
  MODEM_NORMAL    = 1,  // SF7  BW125 CR5 — баланс (дефолт)
  MODEM_RANGE     = 2,  // SF10 BW125 CR5 — дальность
  MODEM_MAX_RANGE = 3,  // SF12 BW125 CR8 — максимальная дальность
  MODEM_CUSTOM    = 4,  // ручные SF/BW/CR
  MODEM_PRESET_COUNT = 5
};

struct ModemConfig {
  uint8_t sf;
  float   bw;
  uint8_t cr;
};

const ModemConfig& modemPresetConfig(ModemPreset p);
const char* modemPresetName(ModemPreset p);

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

// --- Modem config (SF + BW + CR) ---
/** Применить пресет (0–3) или CUSTOM (4) с текущими NVS-значениями. Вызов под mutex. */
void setModemPreset(ModemPreset p);
/** Применить ручные SF/BW/CR и сохранить как CUSTOM в NVS. Вызов под mutex. */
void setCustomModem(uint8_t sf, float bw, uint8_t cr);
/** Постановка в radioCmdQueue — безопасно из любого контекста. */
void requestModemPreset(ModemPreset p);
void requestCustomModem(uint8_t sf, float bw, uint8_t cr);

ModemPreset getModemPreset();
uint8_t getSpreadingFactor();
float   getBandwidth();
uint8_t getCodingRate();

/** Legacy: mesh SF → переводит в CUSTOM с текущими BW/CR. */
void setSpreadingFactor(uint8_t sf);
/** Только чип (планировщик RX/TX) — не трогает mesh и NVS. */
void applyHardwareSpreadingFactor(uint8_t sf);
/** Только чип (для сканера) — меняет SF+BW+CR без NVS и mesh state. */
void applyHardwareModem(uint8_t sf, float bw, uint8_t cr);
/** Запрос смены SF из BLE/UI — постановка в `radioCmdQueue`. */
void requestSpreadingFactor(uint8_t sf);

uint32_t getTimeOnAir(size_t len);       // мкс, для duty cycle
/** CAD (mutex + SPI). Предпочтительно не вызывать из прикладного кода — CSMA в планировщике при TX. */
bool isChannelFree();
/** Сигнал перегрузки (NACK, undelivered) — увеличивает BEB CW для следующих TX */
void notifyCongestion();

}  // namespace radio
