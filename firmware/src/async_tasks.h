/**
 * Async Tasks — packetTask, displayTask
 * Запуск из setup()
 */

#pragma once

#include <Arduino.h>

void asyncTasksStart();

/** txSf: 0 = baseSf, 7–12 = per-neighbor SF. priority=true → xQueueSendToFront */
bool queueSend(const uint8_t* buf, size_t len, uint8_t txSf = 0, bool priority = false);

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
