/**
 * Минимальный OLED SSD1306 (I2C) для nRF52840 — без полного ui/display (ESP).
 */
#pragma once

#include <cstdint>

namespace display_nrf {

/** Wire + SSD1306; при отсутствии дисплея возвращает false, дальнейшие вызовы безопасны. */
bool init();

bool is_ready();

void show_boot(const char* line1, const char* line2);

void show_selftest_summary(bool radioOk, bool antennaOk, uint16_t batteryMv, uint32_t heapFree);

/** Последнее сообщение mesh (две строки, усечение); отрисовка в poll(). */
void queue_last_msg(const char* fromHex, const char* text);

/** Четыре строки статуса (ST7789 T114 — text size 2; OLED — size 1), без очереди poll. */
void show_status_screen(const char* line1, const char* line2, const char* line3, const char* line4);

/** Вызывать из loop: отложенная перерисовка last msg. */
void poll();

}  // namespace display_nrf
