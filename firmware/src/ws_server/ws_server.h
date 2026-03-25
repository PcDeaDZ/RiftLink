/**
 * WebSocket server — WiFi-режим коммуникации с телефоном.
 * Тот же JSON-протокол (cmd/evt) что и BLE GATT.
 */

#pragma once

#include <cstddef>

namespace ws_server {

/** Запустить HTTP + WebSocket сервер + mDNS. */
void start();

/** Остановить сервер. */
void stop();

/** Вызвать из loop() — обработка клиентов. */
void update();

/** Есть подключённый клиент? */
bool hasClient();

/** Отправить JSON-событие клиенту WebSocket. false при ошибке (в т.ч. сессия не найдена). */
bool sendEvent(const char* json, int len);

/** Установить callback для входящих JSON-команд. */
void setOnCommand(void (*cb)(const char* json, size_t len));

}  // namespace ws_server
