#include "nrf_ui_parity.h"
#include "board_pins.h"
#include "display_nrf.h"
#include "locale/locale.h"
#include "region/region.h"
#include "ble/ble.h"

#include <Arduino.h>
#include <cstring>

static constexpr uint32_t kBtnDebounceMs = 40;
static constexpr uint32_t kShortMs = 500;

/** 0 = short (next), 1 = long (OK). */
static int blocking_press_short_or_long() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  bool prev = false;
  uint32_t downAt = 0;
  for (;;) {
    ble::update();
    const bool pressed = digitalRead(T114_BUTTON_PIN) == LOW;
    const uint32_t now = millis();
    if (pressed && !prev) downAt = now;
    if (!pressed && prev) {
      const uint32_t dur = now - downAt;
      if (dur >= kBtnDebounceMs) {
        if (dur < kShortMs) return 0;
        return 1;
      }
    }
    prev = pressed;
    delay(5);
  }
#else
  for (;;) {
    ble::update();
    if (Serial.available()) {
      String line = Serial.readStringUntil('\n');
      line.trim();
      line.toLowerCase();
      if (line == "l" || line == "long") return 1;
      if (line == "s" || line == "short") return 0;
    }
    delay(20);
  }
#endif
}

/**
 * Список меню: short — следующий пункт, long — выбрать текущий.
 * FakeTech: цифра 1..count на строке — мгновенный выбор (для быстрого ввода).
 */
static int blocking_list_pick(const char* title, const char* const* labels, int count, int start_sel) {
  int sel = start_sel;
  if (sel < 0) sel = 0;
  if (sel >= count) sel = count - 1;
  int scroll = 0;
  for (;;) {
    if (!display_nrf::is_ready()) return sel;
    display_nrf::show_menu_list(title, labels, count, sel, scroll, locale::getForDisplay("short_long_hint"));
    scroll = display_nrf::menu_list_last_scroll();
#if !defined(RIFTLINK_BOARD_HELTEC_T114)
    if (Serial.available()) {
      String line = Serial.readStringUntil('\n');
      line.trim();
      const int n = line.toInt();
      if (n >= 1 && n <= count) return n - 1;
    }
#endif
    const int r = blocking_press_short_or_long();
    if (r == 0) {
      sel = (sel + 1) % count;
    } else {
      return sel;
    }
  }
}

void nrf_ui_run_language_until_done() {
#if !defined(RIFTLINK_BOARD_HELTEC_T114)
  Serial.println("[RiftLink] UI: на OLED — s/l; в Serial — s|l или номер строки + Enter");
#endif
  while (!locale::isSet()) {
    if (!display_nrf::is_ready()) {
      (void)locale::setLang(LANG_EN);
      break;
    }
    const char* items[] = {
        locale::getForDisplay("lang_en"),
        locale::getForDisplay("lang_ru"),
        locale::getForDisplay("menu_back"),
    };
    const int sel =
        blocking_list_pick(locale::getForDisplay("lang_picker_title"), items, 3, locale::getLang() == LANG_RU ? 1 : 0);
    if (sel == 0) {
      (void)locale::setLang(LANG_EN);
      break;
    }
    if (sel == 1) {
      (void)locale::setLang(LANG_RU);
      break;
    }
  }
}

void nrf_ui_run_region_until_done() {
#if !defined(RIFTLINK_BOARD_HELTEC_T114)
  Serial.println("[RiftLink] UI: выбор региона — s/l или номер строки + Enter");
#endif
  if (!region::isSet() && !display_nrf::is_ready()) {
    return;
  }
  while (!region::isSet()) {
    if (!display_nrf::is_ready()) {
      return;
    }
    int nPresets = region::getPresetCount();
    if (nPresets <= 0) {
      (void)region::setRegion("EU");
      break;
    }
    if (nPresets > 24) nPresets = 24;

    static char codeBuf[25][8];
    static const char* presetPtrs[26];
    for (int i = 0; i < nPresets; i++) {
      strncpy(codeBuf[i], region::getPresetCode(i), sizeof(codeBuf[0]) - 1);
      codeBuf[i][sizeof(codeBuf[0]) - 1] = 0;
      presetPtrs[i] = codeBuf[i];
    }
    presetPtrs[nPresets] = locale::getForDisplay("menu_back");

    int pickIdx = 0;
    for (int i = 0; i < nPresets; i++) {
      if (strcasecmp(region::getPresetCode(i), region::getCode()) == 0) {
        pickIdx = i;
        break;
      }
    }

    const int sel = blocking_list_pick(locale::getForDisplay("select_country"), presetPtrs, nPresets + 1, pickIdx);
    if (sel == nPresets) continue;
    if (sel < 0 || sel >= nPresets) continue;
    (void)region::setRegion(region::getPresetCode(sel));

    const int nCh = region::getChannelCount();
    if (nCh > 0) {
      static char chBuf[6][28];
      static const char* chItems[7];
      for (int i = 0; i < nCh; i++) {
        snprintf(chBuf[i], sizeof(chBuf[0]), "%.1f MHz", (double)region::getChannelMHz(i));
        chItems[i] = chBuf[i];
      }
      chItems[nCh] = locale::getForDisplay("menu_back");
      int curCh = region::getChannel();
      if (curCh < 0) curCh = 0;
      if (curCh >= nCh) curCh = nCh - 1;
      const int chSel = blocking_list_pick(locale::getForDisplay("dash_ch"), chItems, nCh + 1, curCh);
      if (chSel >= 0 && chSel < nCh) (void)region::setChannel(chSel);
    }
  }
}
