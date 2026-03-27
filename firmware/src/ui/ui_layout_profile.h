/**
 * Профили вёрстки: топ-бар и область контента (контракт для OLED / T‑Pager / Paper).
 * Значения соответствуют текущим константам в display*.cpp — единая точка документации.
 */
#pragma once

#include <cstdint>

namespace ui_layout {

struct Profile {
  int16_t screenWidth;
  int16_t screenHeight;
  /** Высота полосы статуса (сигнал, время, батарея) до разделителя */
  int16_t statusBarHeight;
  /** Y нижней границы «хрома» под статусом (линии разделителя) */
  int16_t chromeBottomY;
  /** Полоса заголовка подменю (top) */
  int16_t submenuTitleTop;
  int16_t submenuTitleHeight;
  /** Первая строка списка контента под заголовком подменю */
  int16_t submenuListY0;
  /**
   * Высота строки списка меню (HOME/popup/power, полоса выбора = menuListRowHeight).
   * Отдельно от плотного текста вкладок (submenuContentLineHeight).
   */
  int16_t menuListRowHeight;
  /**
   * Смещение курсора текста от верха строки списка (без иконки).
   * Adafruit GFX: для size 1 глиф ~8 px — центрирование в строке (напр. +4 при высоте 16).
   */
  int16_t menuListTextOffsetY;
  /** Смещение верха иконки 8×8 от верха строки списка. */
  int16_t menuListIconTopOffsetY;
  /** Межстрочный интервал многострочного текста на вкладках (MAIN/MSG/…), плотнее чем menu list. */
  int16_t submenuContentLineHeight;
  int16_t maxLineChars;
};

/** SSD1306 128×64 — синхронизировано с display.cpp */
inline constexpr Profile profileOled128x64() {
  return Profile{
      128,
      64,
      11,
      13,
      14,
      7,
      23,
      9,
      0,
      0,
      8,  // SUBMENU_CONTENT_LINE_H; «Узел» — nodeMsgLineStepOled()
      20};
}

/** ST7796 T‑Pager — CONTENT_Y=34, submenuListY0 = CONTENT_Y + SUBMENU_TITLE_H + 6 = 60 */
inline constexpr Profile profileTpager480x222() {
  return Profile{
      480,
      222,
      30,
      32,
      34,
      20,
      60,
      16,
      4,
      4,
      16,
      52};
}

/** E‑Ink Paper — submenuListY0 = CONTENT_Y + SUBMENU_TITLE_H_PAPER + 4 = 38; списки меню 16 px, контент вкладок 12 px */
inline constexpr Profile profilePaper250x122() {
  return Profile{
      250,
      122,
      18,
      20,
      22,
      16,
      38,
      16,
      4,
      2,
      12,
      32};
}

/** Первый Y для области контента под топ-баром (без полосы заголовка подменю). */
inline int contentAreaTopY(const Profile& p) { return p.chromeBottomY + 1; }

}  // namespace ui_layout
