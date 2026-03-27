/**
 * RiftLink — экраны UI без полоски вкладок
 *
 * Индекс 0: главное меню (HOME), в нём последний пункт «Питание» — не вкладка.
 * Далее: Main, Msg, Peers, [GPS], Net, Sys.
 * GPS — при модуле или синхронизации с телефоном.
 */

#pragma once

#include "gps/gps.h"
#include <cstdint>

namespace display_tabs {

enum ContentTab { CT_HOME, CT_MAIN, CT_MSG, CT_INFO, CT_SYS, CT_NET, CT_GPS, CT_POWER };

static const uint8_t ICON_HOME[]  = {0x3C,0x42,0x5A,0x42,0x42,0x5A,0x42,0x3C};
static const uint8_t ICON_MAIN[]  = {0x08,0x0C,0x0E,0x08,0x08,0x08,0x1C,0x00};
static const uint8_t ICON_MSG[]   = {0x3C,0x42,0x42,0x42,0x42,0x3C,0x7E,0x00};
static const uint8_t ICON_INFO[]  = {0x00,0x1C,0x08,0x08,0x08,0x08,0x1C,0x00};
static const uint8_t ICON_SYS[]   = {0x18,0x3C,0x5A,0xBD,0xBD,0x5A,0x3C,0x18};
static const uint8_t ICON_NET[]   = {0x00,0x08,0x14,0x22,0x41,0x22,0x14,0x08};
static const uint8_t ICON_GPS[]   = {0x0C,0x1E,0x1E,0x1E,0x3F,0x1E,0x0C,0x00};
/** Питание (главное меню): условный символ «кнопка питания» */
static const uint8_t ICON_POWER[] = {0x18,0x3C,0x66,0x66,0x66,0x3C,0x18,0x00};

static const uint8_t* const TAB_ICONS[] = {
  ICON_HOME, ICON_MAIN, ICON_MSG, ICON_INFO, ICON_SYS, ICON_NET, ICON_GPS, ICON_POWER
};

inline const uint8_t* iconForContent(ContentTab ct) {
  return TAB_ICONS[(int)ct];
}

inline bool isGpsTabVisible() {
  return gps::isPresent() || gps::hasPhoneSync();
}

/** Всего экранов: Home + Main + Msg + Info + [GPS] + Net + Sys */
inline int getTabCount() {
  int n = 6;
  if (isGpsTabVisible()) n++;
  return n;
}

/** Home(0) → Main(1) → Msg(2) → Peers(3) → [GPS] → Net → Sys(last) */
inline ContentTab contentForTab(int tab) {
  if (tab == 0) return CT_HOME;
  if (tab == 1) return CT_MAIN;
  if (tab == 2) return CT_MSG;
  if (tab == 3) return CT_INFO;
  if (isGpsTabVisible()) {
    if (tab == 4) return CT_GPS;
    if (tab == 5) return CT_NET;
    return CT_SYS;
  }
  if (tab == 4) return CT_NET;
  return CT_SYS;
}

inline const uint8_t* getIconForTab(int tab) {
  return iconForContent(contentForTab(tab));
}

/**
 * Режим «только вкладки» (ui_nav_mode::isTabMode): без HOME.
 * Порядок: MAIN, MSG, INFO, [GPS], NET, SYS, POWER (последняя).
 */
inline int getNavTabCount() {
  int n = 6;
  if (isGpsTabVisible()) n++;
  return n;
}

/** Индекс вкладки в таб-режиме; при смене числа вкладок (GPS) защита от выхода за [0, n). */
inline int clampNavTabIndex(int tab) {
  const int n = getNavTabCount();
  if (n < 1) return 0;
  if (tab < 0) return 0;
  if (tab >= n) return n - 1;
  return tab;
}

inline ContentTab contentForNavTab(int tab) {
  if (!isGpsTabVisible()) {
    switch (tab) {
      case 0: return CT_MAIN;
      case 1: return CT_MSG;
      case 2: return CT_INFO;
      case 3: return CT_NET;
      case 4: return CT_SYS;
      case 5: return CT_POWER;
      default: return CT_MAIN;
    }
  }
  switch (tab) {
    case 0: return CT_MAIN;
    case 1: return CT_MSG;
    case 2: return CT_INFO;
    case 3: return CT_GPS;
    case 4: return CT_NET;
    case 5: return CT_SYS;
    case 6: return CT_POWER;
    default: return CT_MAIN;
  }
}

inline const uint8_t* iconForNavTab(int tab) {
  return iconForContent(contentForNavTab(tab));
}

inline int homeMenuCount() {
  return isGpsTabVisible() ? 7 : 6;
}

/** Последний пункт главного меню — «Питание» (модальное окно, не вкладка). */
inline bool homeMenuIsPowerSlot(int slot) {
  return slot == homeMenuCount() - 1;
}

inline ContentTab homeMenuContentAt(int slot) {
  if (!isGpsTabVisible()) {
    switch (slot) {
      case 0: return CT_MAIN;
      case 1: return CT_MSG;
      case 2: return CT_INFO;
      case 3: return CT_NET;
      case 4: return CT_SYS;
      case 5: return CT_POWER;
      default: return CT_MAIN;
    }
  }
  switch (slot) {
    case 0: return CT_MAIN;
    case 1: return CT_MSG;
    case 2: return CT_INFO;
    case 3: return CT_GPS;
    case 4: return CT_NET;
    case 5: return CT_SYS;
    case 6: return CT_POWER;
    default: return CT_MAIN;
  }
}

/** Слот главного меню (HOME) для раздела; зеркало к homeMenuContentAt. Неизвестные — 0. */
inline int homeMenuSlotForContent(ContentTab ct) {
  if (!isGpsTabVisible()) {
    switch (ct) {
      case CT_MAIN: return 0;
      case CT_MSG: return 1;
      case CT_INFO: return 2;
      case CT_NET: return 3;
      case CT_SYS: return 4;
      case CT_POWER: return 5;
      default: return 0;
    }
  }
  switch (ct) {
    case CT_MAIN: return 0;
    case CT_MSG: return 1;
    case CT_INFO: return 2;
    case CT_GPS: return 3;
    case CT_NET: return 4;
    case CT_SYS: return 5;
    case CT_POWER: return 6;
    default: return 0;
  }
}

/** Индекс экрана s_currentScreen для пункта главного меню (не 0). Для «Питания» — -1 (отдельное окно). */
inline int homeMenuTargetScreen(int slot) {
  if (homeMenuIsPowerSlot(slot)) return -1;
  if (!isGpsTabVisible()) {
    switch (slot) {
      case 0: return 1;
      case 1: return 2;
      case 2: return 3;
      case 3: return 4;
      case 4: return 5;
      default: return 0;
    }
  }
  switch (slot) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 3;
    case 3: return 4;
    case 4: return 5;
    case 5: return 6;
    default: return 0;
  }
}

/** Индекс вкладки в режиме «только вкладки» (0 = Main, …, Power = последний). */
inline int navTabIndexForContent(ContentTab ct) {
  if (!isGpsTabVisible()) {
    switch (ct) {
      case CT_MAIN: return 0;
      case CT_MSG: return 1;
      case CT_INFO: return 2;
      case CT_NET: return 3;
      case CT_SYS: return 4;
      case CT_POWER: return 5;
      default: return 0;
    }
  }
  switch (ct) {
    case CT_MAIN: return 0;
    case CT_MSG: return 1;
    case CT_INFO: return 2;
    case CT_GPS: return 3;
    case CT_NET: return 4;
    case CT_SYS: return 5;
    case CT_POWER: return 6;
    default: return 0;
  }
}

/** Индекс экрана в режиме списка (см. contentForTab): HOME=0; для Power — HOME. */
inline int listTabIndexForContent(ContentTab ct) {
  if (ct == CT_HOME) return 0;
  if (!isGpsTabVisible()) {
    switch (ct) {
      case CT_MAIN: return 1;
      case CT_MSG: return 2;
      case CT_INFO: return 3;
      case CT_NET: return 4;
      case CT_SYS: return 5;
      case CT_POWER: return 0;
      default: return 0;
    }
  }
  switch (ct) {
    case CT_MAIN: return 1;
    case CT_MSG: return 2;
    case CT_INFO: return 3;
    case CT_GPS: return 4;
    case CT_NET: return 5;
    case CT_SYS: return 6;
    case CT_POWER: return 0;
    default: return 0;
  }
}

/** Раздел при переходе список↔вкладки: на HOME — выбранный пункт меню, иначе contentForTab. */
inline ContentTab contentTabFromListScreen(int screen, int homeMenuIndex) {
  if (screen == 0) return homeMenuContentAt(homeMenuIndex);
  return contentForTab(screen);
}

}  // namespace display_tabs
