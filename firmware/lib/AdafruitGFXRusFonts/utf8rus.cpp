/**
 * UTF-8 или CP1251 → CP1251 для кириллицы
 * Поддерживает оба варианта: если исходник в UTF-8 или в CP1251 (Windows).
 */

#include "utf8rus.h"
#include <cstring>

#define BUF_SIZE 64
static char s_buf[BUF_SIZE];

const char* utf8rus(const char* utf8) {
  if (!utf8) return "";
  char* out = s_buf;
  const char* end = s_buf + BUF_SIZE - 1;
  while (*utf8 && out < end) {
    unsigned char c = (unsigned char)*utf8++;
    if (c < 0x80) {
      *out++ = (char)c;
    } else if (c == 0xD0) {
      unsigned char c2 = (unsigned char)*utf8;
      if (c2 >= 0x80 && c2 <= 0xBF) {
        if (c2 >= 0x90 && c2 <= 0xAF) {
          *out++ = (char)(0xC0 + (c2 - 0x90));  // А-Я
        } else if (c2 >= 0xB0 && c2 <= 0xBF) {
          *out++ = (char)(0xE0 + (c2 - 0xB0));  // а-п
        } else if (c2 == 0x81) {
          *out++ = (char)0xA8;  // Ё
        } else {
          *out++ = '?';
        }
        utf8++;
      } else {
        /* Не UTF-8 (пробел, цифра, конец) — 0xD0 как CP1251 Р */
        *out++ = (char)0xD0;
      }
    } else if (c == 0xD1) {
      unsigned char c2 = (unsigned char)*utf8;
      if (c2 >= 0x80 && c2 <= 0xBF) {
        if (c2 >= 0x80 && c2 <= 0x8F) {
          *out++ = (char)(0xF0 + (c2 - 0x80));  // р-я
        } else if (c2 == 0x91) {
          *out++ = (char)0xB8;  // ё
        } else {
          *out++ = '?';
        }
        utf8++;
      } else {
        /* Не UTF-8 — 0xD1 как CP1251 С */
        *out++ = (char)0xD1;
      }
    } else if (c >= 0x80 && c <= 0xFF) {
      /* Уже CP1251 (исходник в Windows-1251) — пропускаем как есть */
      *out++ = (char)c;
    } else {
      *out++ = '?';
    }
  }
  *out = '\0';
  return s_buf;
}
