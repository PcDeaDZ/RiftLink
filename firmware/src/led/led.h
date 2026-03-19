/**
 * LED — мигание без блокировки (для обратной связи при нажатии кнопки)
 */

#pragma once

#include <cstdint>

void ledInit(uint8_t pin);
void ledBlink(uint32_t durationMs = 20);
void ledUpdate();
