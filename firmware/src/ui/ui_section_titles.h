/**
 * Заголовки экранов разделов (как подписи в главном меню) — одна точка для OLED/T-Pager/Paper.
 */
#pragma once

#include "display_tabs.h"
#include "locale/locale.h"

namespace ui_section {

inline const char* sectionTitleForContent(display_tabs::ContentTab ct) {
  switch (ct) {
    case display_tabs::CT_MAIN: return locale::getForDisplay("menu_home_node");
    case display_tabs::CT_MSG: return locale::getForDisplay("menu_home_msg");
    case display_tabs::CT_INFO: return locale::getForDisplay("menu_home_peers");
    case display_tabs::CT_GPS: return locale::getForDisplay("tab_gps");
    case display_tabs::CT_NET: return locale::getForDisplay("menu_home_lora");
    case display_tabs::CT_SYS: return locale::getForDisplay("menu_home_settings");
    case display_tabs::CT_POWER: return locale::getForDisplay("menu_home_power");
    default: return "";
  }
}

}  // namespace ui_section
