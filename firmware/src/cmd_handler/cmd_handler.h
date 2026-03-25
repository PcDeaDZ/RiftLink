/**
 * Unified command handler — обработка JSON-команд из BLE и WebSocket.
 * Вызывается из BLE onWrite callback и ws_server onMessage.
 */

#pragma once

#include <cstddef>

namespace cmd_handler {

/** Обработать входящую JSON-команду. Общий парсер для BLE и WebSocket. */
void process(const char* json, size_t len);

/** Инициализация: установить WS callback. Вызвать до ws_server::start(), чтобы первый клиент не попал на пустой cb. */
void init();

}  // namespace cmd_handler
