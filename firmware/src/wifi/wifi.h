/**
 * RiftLink WiFi — STA режим, хранение в NVS
 * Подключение к домашней сети для OTA и др.
 */

#pragma once

#include <cstddef>

namespace wifi {

/** Инициализация WiFi. Возвращает true при успехе. При ошибке (OOM и т.д.) — false, WiFi/OTA/ESP-NOW отключены. */
bool init();
/** Полная деинициализация WiFi: esp_wifi_stop + esp_wifi_deinit, освобождение DMA-буферов. */
void deinit();
/** Ленивая инициализация: вызывает init() + esp_now_slots::init() если ещё не сделано. Для OTA/Wi‑Fi команд из BLE. */
bool ensureInit();
/** WiFi доступен (init успешен)? */
bool isAvailable();
bool setCredentials(const char* ssid, const char* pass);
/** Запуск подключения STA с пред-проверкой SSID/auth. false = старт невозможен. */
bool connect();
void disconnect();
bool isConnected();
void getStatus(char* ssid, size_t ssidLen, char* ip, size_t ipLen);
bool hasCredentials();

}  // namespace wifi
