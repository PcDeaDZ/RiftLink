#include "ui_display_prefs.h"
#include <Arduino.h>
#include <nvs.h>
#include <nvs_flash.h>

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_DISP_FLIP "disp_flip"
#define NVS_KEY_TAB_HIDE "tab_hide_s"

static bool s_inited = false;
static bool s_flip180 = false;
static uint8_t s_tabHideS = 10;

static uint8_t normalizeTabHideFromNvs(uint8_t raw) {
  if (raw == 5 || raw == 10 || raw == 15) return raw;
  if (raw == 0) return 10;
  if (raw == 30) return 15;
  return 10;
}

namespace ui_display_prefs {

void init() {
  if (s_inited) return;
  nvs_handle_t h;
  uint8_t rawTabHide = 10;
  bool haveTabHide = false;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
    uint8_t v = 0;
    if (nvs_get_u8(h, NVS_KEY_DISP_FLIP, &v) == ESP_OK && v != 0) s_flip180 = true;
    if (nvs_get_u8(h, NVS_KEY_TAB_HIDE, &rawTabHide) == ESP_OK) {
      haveTabHide = true;
      s_tabHideS = normalizeTabHideFromNvs(rawTabHide);
    }
    nvs_close(h);
  }
  if (haveTabHide && s_tabHideS != rawTabHide) {
    nvs_handle_t hw;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &hw) == ESP_OK) {
      esp_err_t e = nvs_set_u8(hw, NVS_KEY_TAB_HIDE, s_tabHideS);
      if (e == ESP_OK) e = nvs_commit(hw);
      nvs_close(hw);
      (void)e;
    }
  }
  s_inited = true;
}

bool getFlip180() { return s_flip180; }

bool setFlip180(bool on) {
  s_flip180 = on;
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t e = nvs_set_u8(h, NVS_KEY_DISP_FLIP, on ? 1u : 0u);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}

uint8_t getTabBarHideIdleSeconds() { return s_tabHideS; }

bool setTabBarHideIdleSeconds(uint8_t seconds) {
  if (seconds != 5 && seconds != 10 && seconds != 15) return false;
  s_tabHideS = seconds;
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t e = nvs_set_u8(h, NVS_KEY_TAB_HIDE, s_tabHideS);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}

void cycleTabBarHideIdleSeconds() {
  if (s_tabHideS == 5)
    (void)setTabBarHideIdleSeconds(10);
  else if (s_tabHideS == 10)
    (void)setTabBarHideIdleSeconds(15);
  else
    (void)setTabBarHideIdleSeconds(5);
}

}  // namespace ui_display_prefs
