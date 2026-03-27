/**
 * Общая математика прокрутки списков (без привязки к дисплею).
 */
#pragma once

namespace ui_scroll {

/** Синхронизировать scrollOff с selected и размером окна (как в displayShowPopupMenu). */
inline void syncListWindow(int selected, int count, int showMax, int& scrollOff) {
  if (count <= 0 || showMax <= 0) return;
  if (selected < 0) selected = 0;
  if (selected >= count) selected = count - 1;
  if (selected < scrollOff) scrollOff = selected;
  if (selected >= scrollOff + showMax) scrollOff = selected - showMax + 1;
  if (scrollOff < 0) scrollOff = 0;
  int maxOff = count - showMax;
  if (maxOff < 0) maxOff = 0;
  if (scrollOff > maxOff) scrollOff = maxOff;
}

inline bool canScrollUp(int scrollOff) { return scrollOff > 0; }

inline bool canScrollDown(int scrollOff, int count, int showMax) {
  return count > 0 && scrollOff + showMax < count;
}

}  // namespace ui_scroll
