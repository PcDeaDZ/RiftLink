/**
 * Radio Mode Manager — переключение BLE ↔ WiFi (time-sharing).
 * Одновременно работает только один стек: BLE (Mode A, default) или WiFi (Mode B).
 */

#pragma once

#include <cstdint>

namespace radio_mode {

enum Mode : uint8_t {
  BLE,   // default: телефон по GATT, BLE OTA
  WIFI   // on-demand: ESP-NOW, WebSocket, mDNS
};

enum WifiVariant : uint8_t {
  STA    // подключение к внешней сети
};

/** Текущий активный режим. */
Mode current();
WifiVariant currentWifiVariant();

/** Идёт ли сейчас переключение (brief coexistence). */
bool isSwitching();

/**
 * Переключить на target.
 * Для WIFI: поддерживается только STA; ssid/pass опциональны (можно использовать сохранённые credentials).
 * Возвращает true если переключение запущено.
 */
bool switchTo(Mode target, WifiVariant variant = STA,
              const char* ssid = nullptr, const char* pass = nullptr);

/** Вызвать из loop() — обработка отложенных переключений. */
void update();

/**
 * Старт: пробовать поднять Wi‑Fi STA (если есть креды), если в NVS сохранён предпочтительный режим Wi‑Fi.
 * Явный выбор BLE в приложении пишется в NVS и имеет приоритет над наличием SSID.
 * Если ключа boot_radio ещё нет (старые прошивки) — поведение как раньше: при SSID в NVS пробуем Wi‑Fi.
 */
bool bootShouldTryWifi();

}  // namespace radio_mode
