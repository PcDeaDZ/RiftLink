/**
 * Вертикальная прокрутка контента вкладки (пиксели): clamp и оценка max при известных высотах.
 */
#pragma once

namespace ui_content_scroll {

inline int maxScrollForOverflow(int contentHeight, int viewportHeight) {
  int d = contentHeight - viewportHeight;
  return (d > 0) ? d : 0;
}

inline int clampScroll(int scroll, int maxScroll) {
  if (maxScroll <= 0) return 0;
  if (scroll < 0) return 0;
  if (scroll > maxScroll) return maxScroll;
  return scroll;
}

}  // namespace ui_content_scroll
