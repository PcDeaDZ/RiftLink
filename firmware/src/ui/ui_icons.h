/**
 * Иконки пунктов меню (ссылки на 8×8 bitmap из display_tabs).
 */
#pragma once

#include "display_tabs.h"

namespace ui_icons {

inline const uint8_t* sysMenuIcon(int i) {
  switch (i) {
    case 0: return display_tabs::ICON_SYS;    // Дисплей
    case 1: return display_tabs::ICON_SYS;    // PS
    case 2: return display_tabs::ICON_GPS;    // регион
    case 3: return display_tabs::ICON_NET;    // модем
    case 4: return display_tabs::ICON_MAIN;   // скан
    case 5: return display_tabs::ICON_INFO;   // тест
    case 6: return display_tabs::ICON_HOME;   // назад (на OLED ESP не рисуется — отдельная ветка idx==6)
    case 7: return display_tabs::ICON_HOME;   // зарезервировано
    case 8: return display_tabs::ICON_MAIN;   // зарезервировано
    default: return display_tabs::ICON_MAIN;
  }
}

inline const uint8_t* gpsMenuIcon(int i) {
  return i == 0 ? display_tabs::ICON_GPS : nullptr; /* «Назад» — без иконки */
}

}  // namespace ui_icons
