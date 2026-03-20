/**
 * Radio Mode Manager — переключение BLE ↔ WiFi (time-sharing).
 * Одновременно работает только один стек: BLE (Mode A, default) или WiFi (Mode B).
 */

#pragma once

#include <cstdint>

namespace radio_mode {

enum Mode : uint8_t {
  BLE,   // default: телефон по GATT, BLE OTA
  WIFI   // on-demand: ESP-NOW, WebSocket, WiFi OTA, mDNS
};

enum WifiVariant : uint8_t {
  AP,    // собственная точка (192.168.4.1)
  STA    // подключение к внешней сети
};

/** Текущий активный режим. */
Mode current();

/** Идёт ли сейчас переключение (brief coexistence). */
bool isSwitching();

/**
 * Переключить на target.
 * Для WIFI: variant определяет AP или STA; ssid/pass для STA.
 * Возвращает true если переключение запущено.
 */
bool switchTo(Mode target, WifiVariant variant = AP,
              const char* ssid = nullptr, const char* pass = nullptr);

/** Вызвать из loop() — обработка отложенных переключений. */
void update();

}  // namespace radio_mode
