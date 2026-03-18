/**
 * UTF-8 → CP1251 для вывода кириллицы на OLED
 * Adafruit GFX шрифты используют CP1251
 */

#pragma once

/** Конвертирует UTF-8 в CP1251. Результат в статическом буфере. */
const char* utf8rus(const char* utf8);
