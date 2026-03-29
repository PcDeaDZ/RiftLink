/**
 * RiftLink Locale — en/ru
 */

#include "locale.h"
#include <cstring>

#ifdef RIFTLINK_NRF52
#include "faketec/kv.h"
#else
#include <nvs.h>
#include <nvs_flash.h>
#endif

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
  {"tab_home", "Home", "\xCC\xE5\xED\xFE", "Home"},
  {"tab_main", "Main", "\xC3\xEB\xE0\xE2\xED", "Glavn"},
  {"tab_info", "Info", "\xC8\xED\xF4\xEE", "Info"},
  {"tab_wifi", "WiFi", "WiFi", "WiFi"},
  {"tab_sys", "Sys", "\xD1\xE8\xF1\xF2", "Syst"},
  {"tab_msg", "Msg", "\xD1\xEE\xEE\xE1", "Msg"},
  {"tab_lang", "Lang", "\xDF\xE7\xFB\xEA", "Yazyk"},
  {"btn", "BTN", "\xCA\xCD\xCF", "BTN"},
  {"not_set", "(not set)", "(\xED\xE5\xF2)", "(net)"},
  {"no_messages", "(no messages)", "(\xED\xE5\xF2 \xF1\xEE\xEE\xE1\xF9\xE5\xED\xE8\xE9)", "(no messages)"},
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
  {"shutting_down", "Shutting down...", "\xC2\xFB\xEA\xEB\xFE\xF7\xE5\xED\xE8\xE5...", "Off..."},
  {"low_battery", "Low battery!", "\xCD\xE8\xE7\xEA\xE8\xE9 \xE7\xE0\xF0\xFF\xE4!", "Low bat!"},
  {"antenna_warn", "LoRa: no antenna!", "LoRa: \xED\xE5\xF2 \xE0\xED\xF2\xE5\xED\xED\xFB!", "LoRa: no ant!"},
  {"antenna_check", "Check LoRa antenna", "\xCF\xF0\xEE\xE2\xE5\xF0\xFC\xF2\xE5 \xE0\xED\xF2\xE5\xED\xED\xF3", "Check antenna"},
  {"charging", "CHG", "\xC7\xE0\xF0", "CHG"},
  {"signal", "Signal", "\xD1\xE8\xE3\xED\xE0\xEB", "Signal"},
  {"hold_settings", "> Hold: settings", "> \xC3\xEB: \xED\xE0\xF1\xF2\xF0.", "> Hold: nastr."},
  {"hold_gps", "> Hold: GPS", "> \xC3\xEB: GPS", "> Hold: GPS"},
  {"hold_wifi", "> Hold: \xe2 WiFi", "> \xC3\xEB: \xe2 WiFi", "> Hold: WiFi"},
  {"hold_ble", "> Hold: \xe2 BLE", "> \xC3\xEB: \xe2 BLE", "> Hold: BLE"},
  {"peers", "Peers", "\xD3\xE7\xEB\xFB", "Peers"},
  {"tab_net", "Net", "\xD1\xE5\xF2\xFC", "Net"},
  {"ble_mode", "BLE mode", "BLE \xF0\xE5\xE6\xE8\xEC", "BLE mode"},
  {"wifi_mode", "WiFi mode", "WiFi \xF0\xE5\xE6\xE8\xEC", "WiFi mode"},
  {"pin", "PIN:", "PIN:", "PIN:"},
  {"ap_ssid", "AP:", "AP:", "AP:"},
  {"psave", "PowerSave", "\xDD\xED\xE5\xF0\xE3\xEE\xF1\xE1", "EnSber"},
  {"psave_on", "PowerSave: [ON]", "\xDD\xED\xE5\xF0\xE3\xEE\xF1\xE1: [\xC2\xEA\xEB]", "PS: [ON]"},
  {"psave_off", "PowerSave: [OFF]", "\xDD\xED\xE5\xF0\xE3\xEE\xF1\xE1: [\xC2\xFB\xEA\xEB]", "PS: [OFF]"},
  {"gps_phone", "GPS: Phone", "GPS: \xD2\xE5\xEB\xE5\xF4\xEE\xED", "GPS: Phone"},
  {"menu_back", "<< Back", "<< \xCD\xE0\xE7\xE0\xE4", "<< Nazad"},
  {"warn_title", "Alert", "\xC2\xED\xE8\xEC\xE0\xED\xE8\xE5", "Alert"},
  {"net_ble", "BLE", "BLE", "BLE"},
  {"net_wifi", "WiFi", "WiFi", "WiFi"},
  /** Смена режима (одна строка, без пробелов вокруг дефиса). */
  {"net_mode_line_ble", "BLE-WiFi", "BLE-WiFi", "BLE-WiFi"},
  {"net_mode_line_wifi", "WiFi-BLE", "WiFi-BLE", "WiFi-BLE"},
  {"net_disc", "No link", "\xCD\xE5\xF2", "No link"},
  {"net_wifi_connecting", "Connecting", "\xCF\xEE\xE4\xEA\xEB\xFE\xF7\xE5\xED\xE8\xE5", "Conn"},
  {"menu_selftest", "Selftest", "\xD2\xE5\xF1\xF2", "Test"},
  {"menu_modem", "Modem", "\xCC\xEE\xE4\xE5\xEC", "Modem"},
  {"scan_title", "Scanner", "\xD1\xEA\xE0\xED\xE5\xF0", "Skaner"},
  {"scanning", "Scanning...", "\xD1\xEA\xE0\xED\xE8\xF0\xEE\xE2\xE0\xED\xE8\xE5...", "Scanning..."},
  {"scan_found", "Found:", "\xCD\xE0\xE9\xE4\xE5\xED\xEE:", "Najdeno:"},
  {"scan_empty", "Nothing found", "\xCD\xE8\xF7\xE5\xE3\xEE \xED\xE5\xF2", "Nothing"},
  {"dash_mesh", "Mesh / BLE", "Mesh / BLE", "Mesh/BLE"},
  {"dash_ble_connected", "BLE: connected", "BLE: \xEF\xEE\xE4\xEA\xEB", "BLE: on"},
  {"dash_ble_adv", "BLE: advertising", "BLE: \xF0\xE5\xEA\xEB\xE0\xEC\xE0", "BLE: adv"},
  {"dash_power_title", "Power / time", "\xCF\xE8\xF2\xE0\xED\xE8\xE5 / \xE2\xF0\xE5\xEC\xFF", "Pwr / time"},
  {"dash_min_rssi", "minRSSI", "minRSSI", "minRSSI"},
  {"dash_heap", "heap", "\xEA\xF3\xF7\xE0", "heap"},
  {"dash_uptime", "up", "up", "up"},
  {"last_msg_title", "Last msg", "\xCF\xEE\xF1\xEB\xE5\xE4\xED\xE5\xE5", "Last msg"},
  {"dash_n_prefix", "n", "n", "n"},
  {"dash_ch", "ch", "ch", "ch"},
  {"selftest_ant_ok", "Ant OK", "Ant OK", "Ant OK"},
  {"selftest_ant_warn", "Ant WARN", "Ant WARN", "Ant WARN"},
  {"selftest_heap", "Heap", "Heap", "Heap"},
  {"power_nrf_note", "nRF: no WiFi off\nUSB/DFU update", "nRF: WiFi off\nUSB/DFU", "nRF: no WiFi\nUSB/DFU"},
  {"gps_hw", "HW", "HW", "HW"},
  {"peer_key_yes", "K", "K", "K"},
  {"peer_key_no", "-", "-", "-"},
  {"detail_unknown_title", "?", "?", "?"},
  {"detail_unknown_body", "-", "-", "-"},
  {"lora_sf", "SF", "SF", "SF"},
  {"lora_bw", "BW", "BW", "BW"},
  {"lora_cr", "CR", "CR", "CR"},
  {"lora_mhz", "MHz", "MHz", "MHz"},
  {"lora_dbm", "dBm", "dBm", "dBm"},
  {"boot_line1", "RiftLink", "RiftLink", "RiftLink"},
  {"boot_line2", "nRF52840", "nRF52840", "nRF52840"},
  {"menu_home_node", "Node", "\xCD\xEE\xE4\xE0", "Node"},
  {"menu_home_msg", "Messages", "\xD1\xEE\xEE\xE1\xF9\xE5\xED\xE8\xFF", "Soobshcheniya"},
  {"menu_home_peers", "Neighbors", "\xD1\xEE\xF1\xE5\xE4\xE8", "Sosedi"},
  {"menu_home_lora", "Mode", "\xD0\xE5\xE6\xE8\xEC", "Mode"},
  {"menu_home_settings", "Settings", "\xCD\xE0\xF1\xF2\xF0\xEE\xE9\xEA\xE8", "Nastrojki"},
  {"menu_home_power", "Power", "\xCF\xE8\xF2\xE0\xED\xE8\xE5", "Power"},
  {"menu_power_off", "Shut down", "\xCE\xF2\xEA\xEB\xFE\xF7\xE8\xF2\xFC", "Otkl"},
  {"menu_power_sleep", "Sleep", "\xD1\xEE\xED", "Son"},
  {"ui_hint_home", "S next L open", "\xCA \xE4\xE0\xEB\xFC\xF8\xE5 \xC4 \xE2\xE2\xEE\xE4", "S dal L vv"},
  {"ui_hint_section", "S back L menu", "\xCA \xED\xE0\xE7\xE0\xE4 \xC4 \xEC\xE5\xED\xFE", "S naz L men"},
  {"init_title", "Starting", "\xC7\xE0\xEF\xF3\xF1\xEA", "Start"},
  {"init_step_identity", "Node & crypto", "\xD3\xE7\xE5\xEB \xE8 \xEA\xEB\xFE\xF7\xE8", "Node+crypto"},
  {"init_step_radio", "LoRa radio", "LoRa \xF0\xE0\xE4\xE8\xEE", "LoRa RF"},
  {"init_step_region", "Choose region", "\xD0\xE5\xE3\xE8\xEE\xED", "Region"},
  {"init_step_ble", "Bluetooth LE", "BLE \xF1\xE2\xFF\xE7\xFC", "BLE"},
  {"init_step_done", "Ready", "\xC3\xEE\xF2\xEE\xE2\xEE", "Ready"},
  {"init_hint", "Please wait", "\xCF\xEE\xE4\xEE\xE6\xE4\xE8\xF2\xE5...", "Wait..."},
  {"nav_mode_list", "Nav: list", "Nav: \xF1\xEF\xE8\xF1\xEE\xEA", "Nav:list"},
  {"nav_mode_tabs", "Nav: tabs", "Nav: \xE2\xEA\xEB\xE0\xE4\xEA\xE8", "Nav:tabs"},
  {"menu_display_submenu", "Display", "\xC4\xE8\xF1\xEF\xEB\xE5\xE9", "Display"},
  /* Цельная строка UTF-8 (Да/Нет), без смешения с CP1251 в snprintf. */
  {"menu_display_flip_line_on", "Flip: Yes",
   "\xD0\x9F\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB2\xD1\x91\xD1\x80\xD0\xBD\xD1\x83\xD1\x82\xD1\x8C: \xD0\x94\xD0\xB0", "Flip:Y"},
  {"menu_display_flip_line_off", "Flip: No",
   "\xD0\x9F\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB2\xD1\x91\xD1\x80\xD0\xBD\xD1\x83\xD1\x82\xD1\x8C: \xD0\x9D\xD0\xB5\xD1\x82", "Flip:N"},
  {"menu_display_180", "Flip", "\xD0\x9F\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB2\xD1\x91\xD1\x80\xD0\xBD\xD1\x83\xD1\x82\xD1\x8C", "Flip"},
  {"menu_display_yes", "Yes", "\xC4\xE0", "Yes"},
  {"menu_display_no", "No", "\xCD\xE5\xF2", "No"},
  {"menu_style_label", "Style:", "\xD1\xF2\xE8\xEB\xFC:", "Style:"},
  {"menu_style_tabs", "tabs", "\xE2\xEA\xEB\xE0\xE4\xEA\xE8", "tabs"},
  {"menu_style_list", "list", "\xF1\xEF\xE8\xF1\xEE\xEA", "list"},
  {"disp_flip_title", "Flip", "\xD0\x9F\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB2\xD1\x91\xD1\x80\xD0\xBD\xD1\x83\xD1\x82\xD1\x8C", "Flip"},
  {"tab_hide_setting", "Tab bar hide", "Tab auto", "TabHide"},
  {"tab_power_hint", "Long: menu", "\xC4\xEE\xEB\xE3\xEE: \xEC\xE5\xED\xFE", "Long:menu"},
  {"tab_power_subhint", "Long: list", "\xC4\xEE\xEB\xE3\xEE: \xF1\xEF\xE8\xF1\xEE\xEA", "Long:list"},
};
#define N_STRINGS (sizeof(STRINGS) / sizeof(STRINGS[0]))

namespace locale {

void init() {
  if (s_inited) return;

#ifdef RIFTLINK_NRF52
  // KV уже поднят в main; не вызывать begin() повторно (избегаем лишнего кода и путаницы с порядком init).
  if (riftlink_kv::is_ready()) {
    uint8_t buf[8];
    size_t len = sizeof(buf);
    if (riftlink_kv::getBlob(NVS_KEY_LANG, buf, &len) && len > 0 && len < sizeof(buf)) {
      buf[len] = '\0';
      s_isSet = true;
      if (strcmp(reinterpret_cast<char*>(buf), "ru") == 0) s_lang = LANG_RU;
      else s_lang = LANG_EN;
    }
  }
#else
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
#endif
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
  /* Уже выбран — без повторной записи NVS (BLE/Serial могут слать lang часто → моргание TFT). */
  if (s_inited && s_lang == lang) return true;

#ifdef RIFTLINK_NRF52
  const char* s = (lang == LANG_RU) ? "ru" : "en";
  if (!riftlink_kv::setBlob(NVS_KEY_LANG, reinterpret_cast<const uint8_t*>(s), strlen(s) + 1)) return false;
#else
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;

  nvs_set_str(h, NVS_KEY_LANG, lang == LANG_RU ? "ru" : "en");
  nvs_commit(h);
  nvs_close(h);
#endif

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
