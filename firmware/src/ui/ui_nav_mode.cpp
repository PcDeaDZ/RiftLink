#include "ui_nav_mode.h"
#include <Arduino.h>
#include <nvs.h>
#include <nvs_flash.h>

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_UI_NAV "ui_nav"

static bool s_tabMode = false;
static bool s_inited = false;

namespace ui_nav_mode {

void init() {
  if (s_inited) return;
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
    uint8_t v = 0;
    if (nvs_get_u8(h, NVS_KEY_UI_NAV, &v) == ESP_OK && v != 0) s_tabMode = true;
    nvs_close(h);
  }
  s_inited = true;
}

bool isTabMode() { return s_tabMode; }

bool setTabMode(bool tabs) {
  s_tabMode = tabs;
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t e = nvs_set_u8(h, NVS_KEY_UI_NAV, tabs ? 1u : 0u);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}

}  // namespace ui_nav_mode
