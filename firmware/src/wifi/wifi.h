/**
 * RiftLink WiFi — STA режим, хранение в NVS
 * Подключение к домашней сети для OTA и др.
 */

#pragma once

#include <cstddef>

namespace wifi {

/** Инициализация WiFi. Возвращает true при успехе. При ошибке (OOM и т.д.) — false, WiFi/OTA/ESP-NOW отключены. */
bool init();
/** WiFi доступен (init успешен)? */
bool isAvailable();
bool setCredentials(const char* ssid, const char* pass);
void connect();
void disconnect();
bool isConnected();
void getStatus(char* ssid, size_t ssidLen, char* ip, size_t ipLen);
bool hasCredentials();

}  // namespace wifi
