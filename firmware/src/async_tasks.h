/**
 * Async Tasks — packetTask, displayTask
 * Запуск из setup()
 */

#pragma once

#include <Arduino.h>

void asyncTasksStart();

/** txSf: 0 = baseSf, 7–12 = per-neighbor SF. priority=true → xQueueSendToFront */
bool queueSend(const uint8_t* buf, size_t len, uint8_t txSf = 0, bool priority = false);

/** ACK с задержкой — без блокировки packetTask, RX window для следующего MSG */
void queueDeferredAck(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs = 80);
/** Любой пакет с задержкой — MSG copy2, broadcast 2–3, KEY_EXCHANGE jitter */
void queueDeferredSend(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs);
/** Relay с задержкой — Managed flooding: отмена при услышанной ретрансляции */
void queueDeferredRelay(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs,
    const uint8_t* from, uint32_t payloadHash);
/** Уведомить: услышали ретрансляцию (from+hash) — отменить наш pending relay */
void relayHeard(const uint8_t* from, uint32_t payloadHash);
/** Переносит готовые deferred ACK/send в sendQueue. Вызывается из drainTask. */
void flushDeferredSends();

/** Поставить CMD_SET_LAST_MSG в displayQueue */
void queueDisplayLastMsg(const char* fromHex, const char* text);

/** Поставить CMD_REDRAW_SCREEN в displayQueue. priority=true → в начало очереди (смена вкладки кнопкой) */
void queueDisplayRedraw(uint8_t screen, bool priority = false);

/** Поставить CMD_REQUEST_INFO_REDRAW в displayQueue */
void queueDisplayRequestInfoRedraw();

/** Поставить CMD_LONG_PRESS в displayQueue (screen = текущая вкладка) */
void queueDisplayLongPress(uint8_t screen);
/** Поставить CMD_WAKE в displayQueue (пробуждение из sleep) */
void queueDisplayWake();
/** Поставить CMD_BLINK_LED — мигание без блокировки loop */
void queueDisplayLedBlink();
