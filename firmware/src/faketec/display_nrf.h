/**
 * Минимальный OLED SSD1306 (I2C) для nRF52840 — без полного ui/display (ESP).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace display_nrf {

/** Wire + SSD1306; при отсутствии дисплея возвращает false, дальнейшие вызовы безопасны. */
bool init();

bool is_ready();

/** Применить ui_display_prefs::getFlip180() к setRotation (ST7789 T114). Безопасно для OLED. */
void apply_rotation_from_prefs();

void show_boot(const char* line1, const char* line2);

/** Бутскрин как на Heltec V3: логотип из bootscreen_oled + «v»+версия внизу. */
void show_boot_screen();

/** Экран загрузки как на ESP: «Starting», точки шагов, строка статуса, init_hint. */
void show_init_progress(int doneCount, int totalSteps, const char* statusLine);

/** Предупреждение на весь экран, durationMs с опросом BLE (как displayShowWarning на ESP). */
void show_warning_blocking(const char* line1, const char* line2, uint32_t durationMs);

void show_selftest_summary(bool radioOk, bool antennaOk, uint16_t batteryMv, uint32_t heapFree);

/** Последнее сообщение mesh (две строки, усечение); отрисовка в poll(). */
void queue_last_msg(const char* fromHex, const char* text);

/** Четыре строки статуса (ST7789 T114 — text size 2; OLED — size 1), без очереди poll. */
void show_status_screen(const char* line1, const char* line2, const char* line3, const char* line4);

/** Вызывать из loop: отложенная перерисовка last msg. */
void poll();

/** Список меню: вертикальный список с выделением строки; scroll — индекс первой видимой строки. */
void show_menu_list(const char* title, const char* const* labels, int count, int selected, int scroll,
    const char* footerHint = nullptr, const uint8_t* const* icons = nullptr);

/** Удобная обёртка: то же, что show_menu_list(..., icons). */
void show_home_menu_strip(const char* title, const char* const* labels, const uint8_t* const* icons, int count,
    int selected, int scroll, const char* footerHint = nullptr);

/** После show_menu_list: фактический индекс первой видимой строки (для длинных списков и следующего кадра). OLED: 0. */
int menu_list_last_scroll();

/** Многострочный экран (body с \\n). */
void show_fullscreen_text(const char* title, const char* body);

/** Последнее сообщение для экрана «Сообщения». */
void get_last_msg_peek(char* fromBuf, size_t fromLen, char* textBuf, size_t textLen);

}  // namespace display_nrf
