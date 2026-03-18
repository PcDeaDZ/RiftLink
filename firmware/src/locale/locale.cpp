/**
 * RiftLink Locale — en/ru
 */

#include "locale.h"
#include <nvs.h>
#include <nvs_flash.h>
#include <cstring>

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_LANG "lang"

static int s_lang = LANG_EN;
static bool s_inited = false;
static bool s_isSet = false;

struct StrEntry {
  const char* key;
  const char* en;
  const char* ru;
  const char* ru_oled;  // транслит для OLED (шрифт без кириллицы)
};

/* ru: CP1251 hex escapes — не зависит от компилятора и кодировки файла */
static const StrEntry STRINGS[] = {
  {"tab_main", "Main", "\xC3\xEB\xE0\xE2\xED", "Glavn"},
  {"tab_info", "Info", "\xC8\xED\xF4\xEE", "Info"},
  {"tab_wifi", "WiFi", "WiFi", "WiFi"},
  {"tab_sys", "Sys", "\xD1\xE8\xF1\xF2", "Syst"},
  {"tab_msg", "Msg", "\xD1\xEE\xEE\xE1", "Msg"},
  {"tab_lang", "Lang", "\xDF\xE7\xFB\xEA", "Yazyk"},
  {"btn", "BTN", "\xCA\xCD\xCF", "BTN"},
  {"not_set", "(not set)", "(\xED\xE5\xF2)", "(net)"},
  {"no_messages", "(no messages)", "(\xED\xE5\xF2 \xF1\xEE\xEE\xE1\xF9)", "(net)"},
  {"ota_ap", "OTA AP", "OTA AP", "OTA AP"},
  {"connected", "Connected", "\xCF\xEE\xE4\xEA\xEB", "Podkl"},
  {"sta_mode", "STA mode", "STA", "STA"},
  {"wifi_off", "WiFi Off", "WiFi \xC2\xFB\xEA\xEB", "WiFi Off"},
  {"reconnecting", "Reconnecting...", "\xCF\xE5\xF0\xE5\xEF\xEE\xE4\xEA\xEB...", "Reconn..."},
  {"no_config", "No config", "\xCD\xE5\xF2 \xED\xE0\xF1\xF2\xF0", "No config"},
  {"ble_wifi", "BLE app: wifi", "BLE: wifi", "BLE: wifi"},
  {"ssid_pass", "ssid+pass", "ssid+\xEF\xE0\xF0\xEE\xEB\xFC", "ssid+pass"},
  {"pass", "pass:", "\xEF\xE0\xF0\xEE\xEB\xFC:", "pass:"},
  {"ota_cmd", "OTA: BLE cmd", "OTA: BLE", "OTA: BLE"},
  {"from", "From:", "\xCE\xF2:", "From:"},
  {"init", "Init...", "\xC8\xED\xE8\xF2...", "Init..."},
  {"radio_fail", "Radio FAIL", "\xD0\xE0\xE4\xE8\xEE \xCE\xD8\xC8\xC1", "Radio FAIL"},
  {"radio_ok", "Radio OK", "\xD0\xE0\xE4\xE8\xEE \xCE\xCA", "Radio OK"},
  {"neighbors", "Neighbors", "\xD1\xEE\xF1\xE5\xE4\xE8", "Sosedi"},
  {"region", "Region", "\xD0\xE5\xE3\xE8\xEE\xED", "Region"},
  {"id", "ID", "ID", "ID"},
  {"ch", "CH", "\xCA\xE0\xED\xE0\xEB", "CH"},
  {"nickname", "Nickname", "\xCD\xE8\xEA", "Nick"},
  {"select_lang", "Language", "\xDF\xE7\xFB\xEA", "Yazyk"},
  {"lang_picker_title", "Select language", "\xC2\xFB\xE1\xEE\xF0 \xDF\xE7\xFB\xEA\xE0", "Vybor yazyka"},
  {"hold_next", "S=next L=OK", "\xCA=\xE4\xE0\xEB\xFC\xF8\xE5 \xC4=OK", "S=next L=OK"},
  {"short_long_hint", "S=chg L=OK", "\xCA=\xF1\xEC\xE5\xED\xE0 \xC4=OK", "S=chg L=OK"},
  {"lang_en", "English", "English", "English"},
  {"lang_ru", "Russian", "\xD0\xF3\xF1\xF1\xEA\xE8\xE9", "Russian"},
  {"press_btn", "Press BTN", "\xCD\xE0\xE6\xEC\xE8\xF2\xE5 \xCA\xCD\xCF", "Press BTN"},
  {"tab_gps", "GPS", "GPS", "GPS"},
  {"gps_off", "GPS Off", "GPS \xC2\xFB\xEA\xEB", "GPS Off"},
  {"gps_on", "GPS On", "GPS \xC2\xEA\xEB", "GPS On"},
  {"gps_no_fix", "No fix", "\xCD\xE5\xF2 \xF4\xE8\xEA\xF1\xE0", "No fix"},
  {"gps_fix", "Fix", "\xD4\xE8\xEA\xF1", "Fix"},
  {"gps_not_present", "Not present", "\xCD\xE5\xF2 \xEC\xEE\xE4\xF3\xEB\xFF", "No module"},
  {"select_country", "Select country", "\xC2\xFB\xE1\xEE\xF0 \xF1\xF2\xF0\xE0\xED\xFB", "Country"},
  {"country_rules", "Regional rules", "\xD0\xE5\xE3. \xEF\xF0\xE0\xE2\xE8\xEB\xE0", "Region"},
  {"battery", "Battery", "\xC7\xE0\xF0\xFF\xF4", "Bat"},
};
#define N_STRINGS (sizeof(STRINGS) / sizeof(STRINGS[0]))

namespace locale {

void init() {
  if (s_inited) return;

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
    char buf[8] = {0};
    size_t len = sizeof(buf);
    if (nvs_get_str(h, NVS_KEY_LANG, buf, &len) == ESP_OK && buf[0]) {
      s_isSet = true;
      if (strcmp(buf, "ru") == 0) s_lang = LANG_RU;
      else s_lang = LANG_EN;
    }
    nvs_close(h);
  }
  s_inited = true;
}

bool isSet() {
  return s_isSet;
}

int getLang() {
  return s_lang;
}

bool setLang(int lang) {
  if (lang != LANG_EN && lang != LANG_RU) return false;

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;

  nvs_set_str(h, NVS_KEY_LANG, lang == LANG_RU ? "ru" : "en");
  nvs_commit(h);
  nvs_close(h);

  s_lang = lang;
  s_isSet = true;
  return true;
}

const char* get(const char* key) {
  for (size_t i = 0; i < N_STRINGS; i++) {
    if (strcmp(STRINGS[i].key, key) == 0) {
      return s_lang == LANG_RU ? STRINGS[i].ru : STRINGS[i].en;
    }
  }
  return key;
}

const char* getForDisplay(const char* key) {
  return getForLang(key, s_lang);
}

const char* getForLang(const char* key, int lang) {
  for (size_t i = 0; i < N_STRINGS; i++) {
    if (strcmp(STRINGS[i].key, key) == 0) {
      if (lang == LANG_RU) return STRINGS[i].ru;
      return STRINGS[i].en;
    }
  }
  return key;
}

const char* getLangName(int lang) {
  return lang == LANG_RU ? "Russian" : "English";
}

}  // namespace locale
