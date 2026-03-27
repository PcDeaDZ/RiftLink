/**
 * Прокрутка многострочного текста сообщения (по строкам фиксированной ширины).
 */
#pragma once

#include <cstring>

namespace ui_msg_scroll {

inline size_t lineEndPos(const char* src, size_t len, size_t pos, int maxChars) {
  if (pos >= len) return len;
  size_t e = pos;
  while (e < len && (e - pos) < (size_t)maxChars && src[e] != '\n') e++;
  if (e < len && src[e] == '\n') return e + 1;
  return e;
}

inline void advanceOneLine(size_t& scroll, const char* src, size_t len, int maxChars) {
  if (len <= 1) return;
  size_t e = lineEndPos(src, len, scroll, maxChars);
  if (e >= len || e == scroll) scroll = 0;
  else scroll = e;
}

/** Есть ли содержимое ниже numLines строк от scroll. */
inline bool hasOverflowPastLines(const char* src, size_t len, size_t scroll, int maxChars, int numLines) {
  size_t pos = scroll;
  for (int i = 0; i < numLines; i++) {
    if (pos >= len) return false;
    pos = lineEndPos(src, len, pos, maxChars);
  }
  return pos < len;
}

}  // namespace ui_msg_scroll
