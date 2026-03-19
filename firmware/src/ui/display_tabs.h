/**
 * RiftLink Display Tabs — общая логика вкладок для OLED и E-Ink
 * DRY: getTabCount, contentForTab, иконки — используются в display.cpp и display_paper.cpp
 */

#pragma once

#include "wifi/wifi.h"
#include "gps/gps.h"
#include <cstdint>

namespace display_tabs {

enum ContentTab { CT_MAIN, CT_INFO, CT_WIFI, CT_SYS, CT_MSG, CT_LANG, CT_GPS };

// Иконки вкладок 8x8 (1=пиксель, MSB слева)
static const uint8_t ICON_MAIN[]  = {0x08,0x0C,0x0E,0x08,0x08,0x08,0x1C,0x00};
static const uint8_t ICON_INFO[]  = {0x00,0x1C,0x08,0x08,0x08,0x08,0x1C,0x00};
static const uint8_t ICON_WIFI[]  = {0x00,0x08,0x14,0x22,0x41,0x22,0x14,0x08};
static const uint8_t ICON_SYS[]   = {0x18,0x3C,0x5A,0xBD,0xBD,0x5A,0x3C,0x18};
static const uint8_t ICON_MSG[]   = {0x3C,0x42,0x42,0x42,0x42,0x3C,0x7E,0x00};
static const uint8_t ICON_LANG[]  = {0x3C,0x42,0x99,0xBD,0xBD,0x99,0x42,0x3C};
static const uint8_t ICON_GPS[]   = {0x0C,0x1E,0x1E,0x1E,0x3F,0x1E,0x0C,0x00};

static const uint8_t* const TAB_ICONS[] = {
  ICON_MAIN, ICON_INFO, ICON_WIFI, ICON_SYS, ICON_MSG, ICON_LANG, ICON_GPS
};

inline int getTabCount() {
  return wifi::isAvailable() ? (gps::isPresent() ? 7 : 6) : (gps::isPresent() ? 6 : 5);
}

inline ContentTab contentForTab(int tab) {
  if (wifi::isAvailable()) {
    switch (tab) {
      case 0: return CT_MAIN; case 1: return CT_INFO; case 2: return CT_WIFI;
      case 3: return CT_SYS; case 4: return CT_MSG; case 5: return CT_LANG;
      default: return CT_GPS;
    }
  } else {
    switch (tab) {
      case 0: return CT_MAIN; case 1: return CT_INFO; case 2: return CT_SYS;
      case 3: return CT_MSG; case 4: return CT_LANG;
      default: return CT_GPS;
    }
  }
}

inline const uint8_t* getIconForTab(int tab) {
  return TAB_ICONS[(int)contentForTab(tab)];
}

}  // namespace display_tabs
