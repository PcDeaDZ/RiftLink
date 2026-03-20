/**
 * RiftLink Display Tabs — общая логика вкладок для OLED и E-Ink
 *
 * Порядок: Main → Msg → Peers → [GPS] → Net → Sys
 * GPS — видна при наличии модуля ИЛИ синхронизации с телефоном
 * Net — всегда видна (адаптируется к BLE/WiFi режиму)
 * Sys — всегда последний (настройки: powersave, регион, язык, selftest)
 */

#pragma once

#include "gps/gps.h"
#include <cstdint>

namespace display_tabs {

enum ContentTab { CT_MAIN, CT_MSG, CT_INFO, CT_SYS, CT_NET, CT_GPS };

static const uint8_t ICON_MAIN[]  = {0x08,0x0C,0x0E,0x08,0x08,0x08,0x1C,0x00};
static const uint8_t ICON_MSG[]   = {0x3C,0x42,0x42,0x42,0x42,0x3C,0x7E,0x00};
static const uint8_t ICON_INFO[]  = {0x00,0x1C,0x08,0x08,0x08,0x08,0x1C,0x00};
static const uint8_t ICON_SYS[]   = {0x18,0x3C,0x5A,0xBD,0xBD,0x5A,0x3C,0x18};
static const uint8_t ICON_NET[]   = {0x00,0x08,0x14,0x22,0x41,0x22,0x14,0x08};
static const uint8_t ICON_GPS[]   = {0x0C,0x1E,0x1E,0x1E,0x3F,0x1E,0x0C,0x00};

static const uint8_t* const TAB_ICONS[] = {
  ICON_MAIN, ICON_MSG, ICON_INFO, ICON_SYS, ICON_NET, ICON_GPS
};

inline bool isGpsTabVisible() {
  return gps::isPresent() || gps::hasPhoneSync();
}

inline int getTabCount() {
  int n = 5;  // Main, Msg, Peers, Net, Sys
  if (isGpsTabVisible()) n++;
  return n;
}

/** Main(0) → Msg(1) → Peers(2) → [GPS] → Net → Sys(last) */
inline ContentTab contentForTab(int tab) {
  if (tab == 0) return CT_MAIN;
  if (tab == 1) return CT_MSG;
  if (tab == 2) return CT_INFO;
  int pos = 3;
  if (isGpsTabVisible()) {
    if (tab == pos) return CT_GPS;
    pos++;
  }
  if (tab == pos) return CT_NET;
  return CT_SYS;
}

inline const uint8_t* getIconForTab(int tab) {
  return TAB_ICONS[(int)contentForTab(tab)];
}

}  // namespace display_tabs
