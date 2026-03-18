/**
 * CP1251 → AdafruitGFXRusFonts glcdfont encoding
 * Официальный glcdfont: 128-143=р-я, 144-175=А-Я, 176=пусто, 177-192=а-п, 193=Ё, 194=ё
 */
#pragma once

/** Конвертирует 1 байт CP1251 в индекс шрифта AdafruitGFXRusFonts */
inline unsigned char cp1251_to_rusfont(unsigned char c) {
  if (c < 0x80) return c;  // ASCII
  if (c == 0xA8) return 193;  // Ё
  if (c == 0xB8) return 194;  // ё
  if (c >= 0xC0 && c <= 0xDF) return 144 + (c - 0xC0);  // А-Я
  if (c >= 0xE0 && c <= 0xEF) return 177 + (c - 0xE0);  // а-п (176=пусто в шрифте)
  if (c >= 0xF0 && c <= 0xFF) return 128 + (c - 0xF0);   // р-я
  return '?';
}
