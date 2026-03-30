/**
 * Каркас навигации T114 (фаза 1 эпика): цепочка «родитель > дочерний» в заголовке списка.
 * Только RIFTLINK_BOARD_HELTEC_T114; не тянет ESP ui/display.
 */
#pragma once

#if defined(RIFTLINK_BOARD_HELTEC_T114)

#include <cstdio>
#include <cstring>

namespace ui_t114_nav {

/** Два уровня в buf: «parent > child» (ASCII, узкий TFT). */
inline void format_two(char* buf, size_t buf_sz, const char* parent, const char* child) {
  if (!buf || buf_sz < 4) return;
  if (!parent || !parent[0]) {
    if (child && child[0]) {
      strncpy(buf, child, buf_sz - 1);
      buf[buf_sz - 1] = 0;
    } else {
      buf[0] = 0;
    }
    return;
  }
  if (!child || !child[0]) {
    strncpy(buf, parent, buf_sz - 1);
    buf[buf_sz - 1] = 0;
    return;
  }
  snprintf(buf, buf_sz, "%s > %s", parent, child);
}

}  // namespace ui_t114_nav

#endif
