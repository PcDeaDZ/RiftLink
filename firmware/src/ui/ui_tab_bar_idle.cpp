#include "ui_tab_bar_idle.h"
#include "ui_display_prefs.h"
#include "ui_nav_mode.h"
#include <Arduino.h>

static bool s_inited = false;
static bool s_tabStripVisible = true;
static uint32_t s_lastActivityMs = 0;

namespace ui_tab_bar_idle {

void init() {
  if (s_inited) return;
  s_tabStripVisible = true;
  s_lastActivityMs = millis();
  s_inited = true;
}

void onInput() {
  if (!ui_nav_mode::isTabMode()) {
    s_tabStripVisible = true;
    return;
  }
  s_lastActivityMs = millis();
  s_tabStripVisible = true;
}

void tick(bool tabDrillIn) {
  if (!ui_nav_mode::isTabMode()) {
    s_tabStripVisible = true;
    return;
  }
  const uint8_t sec = ui_display_prefs::getTabBarHideIdleSeconds();
  if (tabDrillIn) {
    s_tabStripVisible = true;
    return;
  }
  const uint32_t now = millis();
  if ((now - s_lastActivityMs) >= (uint32_t)sec * 1000u) s_tabStripVisible = false;
}

bool tabStripVisible() {
  if (!ui_nav_mode::isTabMode()) return false;
  return s_tabStripVisible;
}

bool tryRevealFirstShortOnly() {
  if (!ui_nav_mode::isTabMode()) return false;
  if (s_tabStripVisible) return false;
  s_tabStripVisible = true;
  s_lastActivityMs = millis();
  return true;
}

}  // namespace ui_tab_bar_idle

bool displayTryRevealTabBarRowOnly() {
  return ui_tab_bar_idle::tryRevealFirstShortOnly();
}

void displayNotifyTabChromeActivity() {
  ui_tab_bar_idle::onInput();
}
