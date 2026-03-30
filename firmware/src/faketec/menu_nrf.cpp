/**
 * Меню nRF: зеркало display_tabs (Heltec V3/V4) — главный список и экраны разделов.
 */

#include "menu_nrf.h"
#include "display_nrf.h"
#include "ui/display_tabs.h"
#include "ui/ui_nav_mode.h"
#include "ble/ble.h"
#include "gps/gps.h"
#include "locale/locale.h"
#include "neighbors/neighbors.h"
#include "node/node.h"
#include "protocol/packet.h"
#include "radio/radio.h"
#include "region/region.h"
#include "selftest/selftest.h"
#include "telemetry/telemetry.h"
#include "ui/ui_display_prefs.h"
#if defined(RIFTLINK_BOARD_HELTEC_T114)
#include "ui/ui_icons.h"
#endif
#include "ui/ui_tab_bar_idle.h"
#include "nrf_ui_parity.h"

#include <Arduino.h>
#include <cctype>
#include <cstdio>
#include <cstring>

#if __has_include(<nrf_sdh.h>)
#include <nrf_sdh.h>
#define RIFTLINK_HAS_NRF_SDH 1
#else
#define RIFTLINK_HAS_NRF_SDH 0
#endif

extern "C" __attribute__((weak)) size_t xPortGetFreeHeapSize(void);

static void draw_detail(display_tabs::ContentTab ct);
static void draw_net_menu();
static void draw_sys_menu();
static void draw_power_screen();
static void draw_net_tab_summary();
static void draw_sys_tab_summary();
static void render_nav_tab();
static void advance_nav_tab();

/** Как strcasecmp в displayShowRegionPicker (ESP). */
static int region_code_ieq(const char* a, const char* b) {
  if (!a || !b) return a == b ? 0 : 1;
  while (*a && *b) {
    int ca = tolower((unsigned char)*a++);
    int cb = tolower((unsigned char)*b++);
    if (ca != cb) return ca - cb;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static uint32_t nrf_heap_kb_local() {
  size_t f = xPortGetFreeHeapSize();
#if RIFTLINK_HAS_NRF_SDH
  uint32_t sd = nrf_sdh_get_free_heap_size();
  if (sd > f) f = sd;
#endif
  return (uint32_t)(f / 1024U);
}

/**
 * Дашборд по страницам: стр.0 — как display.cpp drawContentMain (ник / ID / соседи), без «рекламы» версии и без SF/региона
 * на первой странице (это другие вкладки/экраны на V3). Стр.1 — mesh/BLE, стр.2 — питание/куча.
 */
static void fill_dashboard_lines(uint8_t page, char* l1, size_t z1, char* l2, size_t z2, char* l3, size_t z3, char* l4, size_t z4) {
  if (page > 2) page = 0;
  const uint8_t* nid = node::getId();
  char idHex[20];
  snprintf(idHex, sizeof(idHex), "%02X%02X%02X%02X%02X%02X%02X%02X", nid[0], nid[1], nid[2], nid[3], nid[4], nid[5], nid[6],
      nid[7]);
  if (page == 0) {
    char nick[33];
    node::getNickname(nick, sizeof(nick));
    const int nc = neighbors::getCount();
    const int avgRssi = neighbors::getAverageRssi();
    if (nick[0]) {
      snprintf(l1, z1, "%s", nick);
      snprintf(l2, z2, "%s %s", locale::getForDisplay("id"), idHex);
      snprintf(l3, z3, "%s: %d", locale::getForDisplay("neighbors"), nc);
      if (nc > 0)
        snprintf(l4, z4, "%d dBm", avgRssi);
      else
        l4[0] = 0;
    } else {
      snprintf(l1, z1, "%s", idHex);
      if (nc > 0)
        snprintf(l2, z2, "%s: %d %ddBm", locale::getForDisplay("neighbors"), nc, avgRssi);
      else
        snprintf(l2, z2, "%s: 0", locale::getForDisplay("neighbors"));
      l3[0] = 0;
      l4[0] = 0;
    }
  } else if (page == 1) {
    snprintf(l1, z1, "%s", locale::getForDisplay("dash_mesh"));
    snprintf(l2, z2, "%s=%d", locale::getForDisplay("dash_n_prefix"), neighbors::getCount());
    snprintf(l3, z3, "%s",
        ble::isConnected() ? locale::getForDisplay("dash_ble_connected") : locale::getForDisplay("dash_ble_adv"));
    if (neighbors::getCount() <= 0) {
      snprintf(l4, z4, "%s --", locale::getForDisplay("dash_min_rssi"));
    } else {
      int mr = neighbors::getMinRssi();
      snprintf(l4, z4, "%s %d", locale::getForDisplay("dash_min_rssi"), mr < 0 ? mr : -120);
    }
  } else {
    uint16_t mv = telemetry::readBatteryMv();
    snprintf(l1, z1, "%s", locale::getForDisplay("dash_power_title"));
    snprintf(l2, z2, "%s %umV", locale::getForDisplay("battery"), (unsigned)mv);
    snprintf(l3, z3, "%s %u kB", locale::getForDisplay("dash_heap"), (unsigned)nrf_heap_kb_local());
    snprintf(l4, z4, "%s %lus", locale::getForDisplay("dash_uptime"), (unsigned long)(millis() / 1000UL));
  }
}

enum class View : uint8_t {
  Dashboard,
  MenuHome,
  Detail,
  Power,
  NetMenu,
  SysMenu,
  /** Как displayShowModemPicker на ESP: short — строка, long — выбор / «Назад». */
  SysModemPicker,
  /** Как displayShowRegionPicker: список кодов пресетов + «Назад». */
  SysRegionPicker,
  /** Второй шаг: частоты каналов + «Назад». */
  SysRegionChannelPicker,
  /** После modemScan: список результатов или «пусто» + «Назад» (как popup на ESP). */
  SysModemScanMenu,
  InfoScreen,
  SysAfterSelftest,
};

static View s_view = View::MenuHome;
static uint8_t s_dashPage = 0;
static int s_homeSel = 0;
/** Индекс первой видимой строки главного меню (длинные списки на T114). */
static int s_homeScroll = 0;
static int s_navTab = 0;
static bool s_tabDrillIn = false;

#if defined(RIFTLINK_BOARD_HELTEC_T114)
static void nrf_tab_chrome(display_nrf::StatusScreenChrome* o) {
  o->draw_tab_row = ui_nav_mode::isTabMode() && ui_tab_bar_idle::tabStripVisible();
  o->selected_tab = s_navTab;
  o->tab_count = display_tabs::getNavTabCount();
}
#endif

/** Полноэкранный текст и списки: на T114 с полоской вкладок (как V3), если таб-режим и полоска видна. */
static void nrf_show_fs(const char* t, const char* b) {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  display_nrf::StatusScreenChrome ch;
  nrf_tab_chrome(&ch);
  display_nrf::show_fullscreen_text(t, b, &ch);
#else
  display_nrf::show_fullscreen_text(t, b);
#endif
}

static void nrf_show_menu(const char* title, const char* const* labels, int count, int selected, int scroll,
    const char* footerHint, const uint8_t* const* icons) {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  display_nrf::StatusScreenChrome ch;
  nrf_tab_chrome(&ch);
  display_nrf::show_menu_list(title, labels, count, selected, scroll, footerHint, icons, &ch);
#else
  display_nrf::show_menu_list(title, labels, count, selected, scroll, footerHint, icons);
#endif
}

static int s_netMenuSel = 0;
static int s_sysMenuSel = 0;
/** Как s_powerMenuIndex в display.cpp: выбор в drill-меню POWER (0 off, 1 sleep, 2 back). */
static int s_powerMenuSel = 0;
/** Как display.cpp s_gpsMenuIndex: 0 — строка вкл/выкл GPS, 1 — «Назад». */
static int s_gpsMenuIndex = 0;
/** Подменю «Дисплей» в SYS — как s_sysInDisplaySubmenu в display.cpp. */
static bool s_sysInDisplaySubmenu = false;
/** Куда вернуться с InfoScreen (скан / заглушка PS). */
static View s_infoReturnView = View::SysMenu;

/** Picker модема (как displayShowModemPicker). */
static int s_modemPickerSel = 0;
/** Picker региона: индекс строки 0..nPresets (последняя — «Назад»). */
static int s_regionPickerSel = 0;
static int s_regionPickerCount = 0;
/** Picker канала: 0..nCh-1 и «Назад». */
static int s_channelPickerSel = 0;
static int s_channelPickerCount = 0;
/** Меню после скана эфира. */
static View s_scanReturnView = View::SysMenu;
static int s_scanMenuSel = 0;
static int s_scanMenuCount = 0;
static int s_scanFound = 0;
static selftest::ScanResult s_scanResults[6];
/** Последний раздел Detail — для перерисовки после смены языка. */
static display_tabs::ContentTab s_detailTab = display_tabs::CT_MAIN;
/** Заголовок и текст для InfoScreen (скан эфира и т.п.). */
static char s_infoTitle[24];
static char s_infoBody[420];

#if defined(RIFTLINK_BOARD_HELTEC_T114)
static uint64_t s_t114LiveDigest = 0;
/** Дайджест без «живой» телеметрии эфира/АКБ/времени — тело меню «Режим» и т.п. не меняется, полоса сверху может. */
static uint64_t s_t114MenuSkeletonDigest = 0;

static uint64_t t114_digest_mix(uint64_t h, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; i++) {
    h ^= (uint64_t)b[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t t114_compute_digest(bool include_mesh_telemetry) {
  uint64_t h = 14695981039346656037ULL;
  uint8_t u8;
  uint32_t u32;
  int32_t i32;

  u8 = (uint8_t)s_view;
  h = t114_digest_mix(h, &u8, 1);
  u8 = s_dashPage;
  h = t114_digest_mix(h, &u8, 1);
  i32 = (int32_t)s_navTab;
  h = t114_digest_mix(h, &i32, sizeof(i32));
  u8 = s_tabDrillIn ? 1 : 0;
  h = t114_digest_mix(h, &u8, 1);
  u8 = ui_tab_bar_idle::tabStripVisible() ? 1 : 0;
  h = t114_digest_mix(h, &u8, 1);
  i32 = (int32_t)s_homeSel;
  h = t114_digest_mix(h, &i32, sizeof(i32));
  i32 = (int32_t)s_homeScroll;
  h = t114_digest_mix(h, &i32, sizeof(i32));
  i32 = (int32_t)s_netMenuSel;
  h = t114_digest_mix(h, &i32, sizeof(i32));
  i32 = (int32_t)s_sysMenuSel;
  h = t114_digest_mix(h, &i32, sizeof(i32));
  i32 = (int32_t)s_powerMenuSel;
  h = t114_digest_mix(h, &i32, sizeof(i32));
  i32 = (int32_t)s_gpsMenuIndex;
  h = t114_digest_mix(h, &i32, sizeof(i32));
  u8 = s_sysInDisplaySubmenu ? 1 : 0;
  h = t114_digest_mix(h, &u8, 1);
  u8 = (uint8_t)s_detailTab;
  h = t114_digest_mix(h, &u8, 1);

  if (include_mesh_telemetry) {
    const int nc = neighbors::getCount();
    h = t114_digest_mix(h, &nc, sizeof(nc));
    /* Сглаживание для дайджеста: иначе дребезг RSSI даёт полный редро дашборда/меню в режиме списка. */
    i32 = (int32_t)((neighbors::getAverageRssi() / 5) * 5);
    h = t114_digest_mix(h, &i32, sizeof(i32));
    i32 = (int32_t)((neighbors::getMinRssi() / 5) * 5);
    h = t114_digest_mix(h, &i32, sizeof(i32));
    u8 = ble::isConnected() ? 1 : 0;
    h = t114_digest_mix(h, &u8, 1);
    u32 = (uint32_t)telemetry::readBatteryMv();
    h = t114_digest_mix(h, &u32, sizeof(u32));
    i32 = (int32_t)telemetry::batteryPercent();
    h = t114_digest_mix(h, &i32, sizeof(i32));
    u8 = telemetry::isCharging() ? 1 : 0;
    h = t114_digest_mix(h, &u8, 1);
    u8 = gps::hasTime() ? 1 : 0;
    h = t114_digest_mix(h, &u8, 1);
    u8 = (uint8_t)(gps::getLocalHour() & 0xff);
    h = t114_digest_mix(h, &u8, 1);
    u8 = (uint8_t)(gps::getLocalMinute() & 0xff);
    h = t114_digest_mix(h, &u8, 1);
    u8 = (uint8_t)region::getChannel();
    h = t114_digest_mix(h, &u8, 1);
    u8 = (uint8_t)radio::getSpreadingFactor();
    h = t114_digest_mix(h, &u8, 1);
    u8 = (uint8_t)radio::getModemPreset();
    h = t114_digest_mix(h, &u8, 1);
    /* Куча дрожит каждый тик — иначе в режиме списка полный редро меню/дашборда без остановки. */
    u32 = (uint32_t)(nrf_heap_kb_local() / 4u);
    h = t114_digest_mix(h, &u32, sizeof(u32));
    u32 = (uint32_t)(region::getFreq() * 1000.0);
    h = t114_digest_mix(h, &u32, sizeof(u32));
    char rc[8];
    strncpy(rc, region::getCode(), sizeof(rc) - 1);
    rc[sizeof(rc) - 1] = 0;
    h = t114_digest_mix(h, rc, strlen(rc));
  }

  switch (s_view) {
    case View::InfoScreen:
      h = t114_digest_mix(h, s_infoBody, strlen(s_infoBody));
      break;
    case View::SysModemPicker:
      i32 = (int32_t)s_modemPickerSel;
      h = t114_digest_mix(h, &i32, sizeof(i32));
      break;
    case View::SysRegionPicker:
      i32 = (int32_t)s_regionPickerSel;
      h = t114_digest_mix(h, &i32, sizeof(i32));
      break;
    case View::SysRegionChannelPicker:
      i32 = (int32_t)s_channelPickerSel;
      h = t114_digest_mix(h, &i32, sizeof(i32));
      break;
    case View::SysModemScanMenu:
      i32 = (int32_t)s_scanMenuSel;
      h = t114_digest_mix(h, &i32, sizeof(i32));
      i32 = (int32_t)s_scanFound;
      h = t114_digest_mix(h, &i32, sizeof(i32));
      break;
    case View::Detail:
      switch (s_detailTab) {
        case display_tabs::CT_MSG: {
          char from[20], text[48];
          display_nrf::get_last_msg_peek(from, sizeof(from), text, sizeof(text));
          h = t114_digest_mix(h, from, strlen(from));
          h = t114_digest_mix(h, text, strlen(text));
          break;
        }
        case display_tabs::CT_GPS:
          u8 = gps::isPresent() ? 1 : 0;
          h = t114_digest_mix(h, &u8, 1);
          u8 = gps::isEnabled() ? 1 : 0;
          h = t114_digest_mix(h, &u8, 1);
          u8 = gps::hasFix() ? 1 : 0;
          h = t114_digest_mix(h, &u8, 1);
          u32 = gps::getSatellites();
          h = t114_digest_mix(h, &u32, sizeof(u32));
          i32 = (int32_t)(gps::getLat() * 100000.0);
          h = t114_digest_mix(h, &i32, sizeof(i32));
          i32 = (int32_t)(gps::getLon() * 100000.0);
          h = t114_digest_mix(h, &i32, sizeof(i32));
          break;
        case display_tabs::CT_INFO: {
          const int nc = neighbors::getCount();
          for (int i = 0; i < nc && i < 16; i++) {
            char hx[20];
            neighbors::getIdHex(i, hx);
            h = t114_digest_mix(h, hx, 4);
            int r = neighbors::getRssi(i);
            h = t114_digest_mix(h, &r, sizeof(r));
          }
          break;
        }
        case display_tabs::CT_MAIN: {
          char nick[33];
          node::getNickname(nick, sizeof(nick));
          h = t114_digest_mix(h, nick, strlen(nick));
          break;
        }
        case display_tabs::CT_NET:
          if (!s_tabDrillIn) {
            char adv[24];
            ble::getAdvertisingName(adv, sizeof(adv));
            h = t114_digest_mix(h, adv, strlen(adv));
            u32 = ble::getPasskey();
            h = t114_digest_mix(h, &u32, sizeof(u32));
          }
          break;
        default:
          break;
      }
      break;
    default:
      break;
  }
  return h;
}

static bool t114_periodic_allow_chrome_only_refresh() {
  /* Свойства вкладок NET/SYS до long: полноэкранный текст без drill — тело из локали/adv/PIN, не из RSSI/heap. */
  if (s_view == View::Detail && !s_tabDrillIn &&
      (s_detailTab == display_tabs::CT_NET || s_detailTab == display_tabs::CT_SYS)) {
    return true;
  }
  /* Главное меню в режиме списка: строки из локали, без тела — без полного clear_for_screen при каждом тике дайджеста. */
  if (s_view == View::MenuHome) return true;
  /* Без экранов редактирования модема/региона: тело зависит от пресета/SF, см. include_mesh_telemetry. */
  switch (s_view) {
    case View::NetMenu:
    case View::SysMenu:
    case View::SysModemPicker:
    case View::SysRegionPicker:
    case View::SysRegionChannelPicker:
    case View::SysModemScanMenu:
    case View::Power:
    case View::InfoScreen:
      return true;
    default:
      return false;
  }
}

static void t114_sync_digest_after_draw() {
  s_t114LiveDigest = t114_compute_digest(true);
  s_t114MenuSkeletonDigest = t114_compute_digest(false);
}
#endif

void nrf_render_dashboard(uint8_t page) {
  if (!display_nrf::is_ready()) return;
  if (ui_nav_mode::isTabMode()) {
    s_dashPage = page > 2 ? 0 : page;
    render_nav_tab();
#if defined(RIFTLINK_BOARD_HELTEC_T114)
    t114_sync_digest_after_draw();
#endif
    return;
  }
  if (page > 2) page = 0;
  char l1[32], l2[32], l3[40], l4[40];
  fill_dashboard_lines(page, l1, sizeof(l1), l2, sizeof(l2), l3, sizeof(l3), l4, sizeof(l4));
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  display_nrf::StatusScreenChrome ch;
  nrf_tab_chrome(&ch);
  display_nrf::show_status_screen(l1, l2, l3, l4, &ch);
#else
  display_nrf::show_status_screen(l1, l2, l3, l4);
#endif
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  t114_sync_digest_after_draw();
#endif
}

static constexpr uint32_t kDebounceMs = 40;
static constexpr uint32_t kShortMs = 500;

#if defined(RIFTLINK_BOARD_HELTEC_T114)
static bool s_btnPrev = false;
static uint32_t s_btnDownAt = 0;
#endif

static const char* label_key_for_home_slot(int slot) {
  const display_tabs::ContentTab ct = display_tabs::homeMenuContentAt(slot);
  switch (ct) {
    case display_tabs::CT_MAIN:
      return "menu_home_node";
    case display_tabs::CT_MSG:
      return "menu_home_msg";
    case display_tabs::CT_INFO:
      return "menu_home_peers";
    case display_tabs::CT_GPS:
      return "tab_gps";
    case display_tabs::CT_NET:
      return "menu_home_lora";
    case display_tabs::CT_SYS:
      return "menu_home_settings";
    case display_tabs::CT_POWER:
      return "menu_home_power";
    default:
      return "menu_home_node";
  }
}

/** Как ESP drawContentNet в drill: только 2 позиции — строка режима и «Назад» (без модема/региона/сканера в NET). */
static constexpr int kNetMenuItems = 2;
/** Главное меню SYS: 6 пунктов + «Назад» (как kSysListItems в display.cpp). */
static constexpr int kSysMainMenuItems = 7;

static int sys_display_sub_menu_count() { return ui_nav_mode::isTabMode() ? 5 : 4; }
static int sys_display_sub_back_idx() { return sys_display_sub_menu_count() - 1; }

static void clamp_sys_display_sub_menu_index() {
  const int c = sys_display_sub_menu_count();
  if (s_sysMenuSel >= c) s_sysMenuSel = c - 1;
  if (s_sysMenuSel < 0) s_sysMenuSel = 0;
}

static int sys_sys_menu_row_count() {
  return s_sysInDisplaySubmenu ? sys_display_sub_menu_count() : kSysMainMenuItems;
}

/** Как sysMainMenuIndexToExecSel в display.cpp. */
static int sys_main_menu_index_to_exec_sel(int idx) {
  switch (idx) {
    case 1:
      return 2;
    case 2:
      return 3;
    case 3:
      return 0;
    case 4:
      return 1;
    case 5:
      return 4;
    default:
      return -1;
  }
}

static void fill_sys_display_sub_label(int idx, char* buf, size_t bufSz) {
  const bool tabs = ui_nav_mode::isTabMode();
  const int backIdx = sys_display_sub_back_idx();
  if (idx == 0) {
    strncpy(buf, locale::getForDisplay("select_lang"), bufSz);
  } else if (idx == 1) {
    strncpy(buf,
        locale::getForDisplay(ui_display_prefs::getFlip180() ? "menu_display_flip_line_on" : "menu_display_flip_line_off"),
        bufSz);
  } else if (idx == 2) {
    snprintf(buf, bufSz, "%s %s", locale::getForDisplay("menu_style_label"),
        tabs ? locale::getForDisplay("menu_style_tabs") : locale::getForDisplay("menu_style_list"));
  } else if (tabs && idx == 3 && backIdx == 4) {
    snprintf(buf, bufSz, "%s: %us", locale::getForDisplay("tab_hide_setting"),
        (unsigned)ui_display_prefs::getTabBarHideIdleSeconds());
  } else {
    buf[0] = '\0';
  }
  buf[bufSz - 1] = '\0';
}

static void fill_sys_main_label(int idx, char* buf, size_t bufSz) {
  switch (idx) {
    case 0:
      strncpy(buf, locale::getForDisplay("menu_display_submenu"), bufSz);
      break;
    case 1:
      /* Как sysMenuFillLabelOled: nRF без powersave.cpp — суффикс как у V3 при выкл. (OFF->ON). */
      snprintf(buf, bufSz, "PS:%s", locale::getForDisplay("gps_toggle_off_on"));
      break;
    case 2:
      strncpy(buf, locale::getForDisplay("region"), bufSz);
      break;
    case 3:
      strncpy(buf, locale::getForDisplay("menu_modem"), bufSz);
      break;
    case 4:
      strncpy(buf, locale::getForDisplay("scan_title"), bufSz);
      break;
    case 5:
      strncpy(buf, locale::getForDisplay("menu_selftest"), bufSz);
      break;
    default:
      buf[0] = '\0';
      break;
  }
  buf[bufSz - 1] = '\0';
}

/** Как displayShowModemPicker (display.cpp). */
static void draw_modem_picker_screen() {
  static char lines[6][40];
  static const char* ptrs[6];
  const char* names[] = {"Speed", "Normal", "Range", "MaxRange", "Custom"};
  const char* desc[] = {"SF7 BW250", "SF7 BW125", "SF10 BW125", "SF12 BW125", ""};
  for (int i = 0; i < 4; i++) {
    snprintf(lines[i], sizeof(lines[i]), "%s %s", names[i], desc[i]);
  }
  char b[28];
  snprintf(b, sizeof(b), "SF%u BW%.0f CR%u", (unsigned)radio::getSpreadingFactor(), (double)radio::getBandwidth(),
      (unsigned)radio::getCodingRate());
  snprintf(lines[4], sizeof(lines[4]), "%s %s", names[4], b);
  strncpy(lines[5], locale::getForDisplay("menu_back"), sizeof(lines[5]) - 1);
  lines[5][sizeof(lines[5]) - 1] = '\0';
  for (int i = 0; i < 6; i++) ptrs[i] = lines[i];
  if (s_modemPickerSel < 0) s_modemPickerSel = 0;
  if (s_modemPickerSel > 5) s_modemPickerSel = 5;
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  display_nrf::StatusScreenChrome ch;
  nrf_tab_chrome(&ch);
  display_nrf::show_menu_list(nullptr, ptrs, 6, s_modemPickerSel, 0, nullptr, nullptr, &ch);
  t114_sync_digest_after_draw();
#else
  nrf_show_menu(nullptr, ptrs, 6, s_modemPickerSel, 0, nullptr, nullptr);
#endif
}

static char s_regionLineBuf[25][16];
static const char* s_regionPtrs[26];

/** Как displayShowRegionPicker: список кодов + «Назад». */
static void draw_region_picker_screen() {
  int n = region::getPresetCount();
  if (n <= 0) {
    s_view = View::SysMenu;
    draw_sys_menu();
    return;
  }
  if (n > 24) n = 24;
  s_regionPickerCount = n + 1;
  for (int i = 0; i < n; i++) {
    strncpy(s_regionLineBuf[i], region::getPresetCode(i), sizeof(s_regionLineBuf[i]) - 1);
    s_regionLineBuf[i][sizeof(s_regionLineBuf[i]) - 1] = '\0';
    s_regionPtrs[i] = s_regionLineBuf[i];
  }
  strncpy(s_regionLineBuf[n], locale::getForDisplay("menu_back"), sizeof(s_regionLineBuf[n]) - 1);
  s_regionLineBuf[n][sizeof(s_regionLineBuf[n]) - 1] = '\0';
  s_regionPtrs[n] = s_regionLineBuf[n];
  if (s_regionPickerSel < 0) s_regionPickerSel = 0;
  if (s_regionPickerSel >= s_regionPickerCount) s_regionPickerSel = s_regionPickerCount - 1;
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  display_nrf::StatusScreenChrome ch;
  nrf_tab_chrome(&ch);
  display_nrf::show_menu_list(nullptr, s_regionPtrs, s_regionPickerCount, s_regionPickerSel, 0, nullptr, nullptr, &ch);
  t114_sync_digest_after_draw();
#else
  nrf_show_menu(nullptr, s_regionPtrs, s_regionPickerCount, s_regionPickerSel, 0, nullptr, nullptr);
#endif
}

static char s_chLineBuf[16][24];
static const char* s_chPtrs[17];

/** Второй шаг displayShowRegionPicker — каналы в МГц. */
static void draw_region_channel_picker_screen() {
  int nCh = region::getChannelCount();
  if (nCh <= 0) {
    s_view = View::SysMenu;
    draw_sys_menu();
    return;
  }
  if (nCh > 16) nCh = 16;
  s_channelPickerCount = nCh + 1;
  for (int i = 0; i < nCh; i++) {
    snprintf(s_chLineBuf[i], sizeof(s_chLineBuf[i]), "%.1f MHz", (double)region::getChannelMHz(i));
    s_chLineBuf[i][sizeof(s_chLineBuf[i]) - 1] = '\0';
    s_chPtrs[i] = s_chLineBuf[i];
  }
  strncpy(s_chLineBuf[nCh], locale::getForDisplay("menu_back"), sizeof(s_chLineBuf[nCh]) - 1);
  s_chLineBuf[nCh][sizeof(s_chLineBuf[nCh]) - 1] = '\0';
  s_chPtrs[nCh] = s_chLineBuf[nCh];
  if (s_channelPickerSel < 0) s_channelPickerSel = 0;
  if (s_channelPickerSel >= s_channelPickerCount) s_channelPickerSel = s_channelPickerCount - 1;
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  display_nrf::StatusScreenChrome ch;
  nrf_tab_chrome(&ch);
  display_nrf::show_menu_list(nullptr, s_chPtrs, s_channelPickerCount, s_channelPickerSel, 0, nullptr, nullptr, &ch);
  t114_sync_digest_after_draw();
#else
  nrf_show_menu(nullptr, s_chPtrs, s_channelPickerCount, s_channelPickerSel, 0, nullptr, nullptr);
#endif
}

/** После modemScan — как popup на ESP (displayRunModemScan). */
static void draw_modem_scan_menu() {
  static char lines[8][44];
  static const char* ptrs[8];
  if (s_scanFound <= 0) {
    strncpy(lines[0], locale::getForDisplay("scan_empty"), sizeof(lines[0]) - 1);
    strncpy(lines[1], locale::getForDisplay("menu_back"), sizeof(lines[1]) - 1);
    lines[0][sizeof(lines[0]) - 1] = lines[1][sizeof(lines[1]) - 1] = '\0';
    ptrs[0] = lines[0];
    ptrs[1] = lines[1];
    s_scanMenuCount = 2;
    if (s_scanMenuSel < 0 || s_scanMenuSel > 1) s_scanMenuSel = 1;
  } else {
    int nShow = s_scanFound > 4 ? 4 : s_scanFound;
    strncpy(lines[0], locale::getForDisplay("scan_found"), sizeof(lines[0]) - 1);
    lines[0][sizeof(lines[0]) - 1] = '\0';
    ptrs[0] = lines[0];
    for (int i = 0; i < nShow; i++) {
      snprintf(lines[1 + i], sizeof(lines[1 + i]), "SF%u BW%.0f %ddBm", (unsigned)s_scanResults[i].sf, (double)s_scanResults[i].bw,
          s_scanResults[i].rssi);
      ptrs[1 + i] = lines[1 + i];
    }
    strncpy(lines[1 + nShow], locale::getForDisplay("menu_back"), sizeof(lines[1 + nShow]) - 1);
    lines[1 + nShow][sizeof(lines[1 + nShow]) - 1] = '\0';
    ptrs[1 + nShow] = lines[1 + nShow];
    s_scanMenuCount = nShow + 2;
    if (s_scanMenuSel < 0 || s_scanMenuSel >= s_scanMenuCount) s_scanMenuSel = s_scanMenuCount - 1;
  }
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  display_nrf::StatusScreenChrome ch;
  nrf_tab_chrome(&ch);
  display_nrf::show_menu_list(nullptr, ptrs, s_scanMenuCount, s_scanMenuSel, 0, nullptr, nullptr, &ch);
  t114_sync_digest_after_draw();
#else
  nrf_show_menu(nullptr, ptrs, s_scanMenuCount, s_scanMenuSel, 0, nullptr, nullptr);
#endif
}

static void open_modem_picker_from_sys() {
  int pickIdx = (int)radio::getModemPreset();
  if (pickIdx < 0 || pickIdx > 4) pickIdx = 1;
  s_modemPickerSel = pickIdx;
  s_view = View::SysModemPicker;
  draw_modem_picker_screen();
}

static void open_region_picker_from_sys() {
  int nPresets = region::getPresetCount();
  if (nPresets <= 0) return;
  if (nPresets > 24) nPresets = 24;
  int pickIdx = 0;
  for (int i = 0; i < nPresets; i++) {
    if (region_code_ieq(region::getPresetCode(i), region::getCode()) == 0) {
      pickIdx = i;
      break;
    }
  }
  s_regionPickerSel = pickIdx;
  s_view = View::SysRegionPicker;
  draw_region_picker_screen();
}

static void apply_region_picker_and_maybe_channel() {
  const int n = s_regionPickerCount - 1;
  if (s_regionPickerSel < 0 || s_regionPickerSel >= n) return;
  (void)region::setRegion(region::getPresetCode(s_regionPickerSel));
  const int nCh = region::getChannelCount();
  if (nCh > 0) {
    int curCh = region::getChannel();
    if (curCh < 0) curCh = 0;
    if (curCh >= nCh) curCh = nCh - 1;
    s_channelPickerSel = curCh;
    s_view = View::SysRegionChannelPicker;
    draw_region_channel_picker_screen();
  } else {
    s_view = View::SysMenu;
    draw_sys_menu();
  }
}

static void draw_net_menu() {
  if (s_netMenuSel < 0 || s_netMenuSel >= kNetMenuItems) s_netMenuSel = 0;
  static char adv[24];
  static char pinLine[40];
  static char modeLine[24];
  ble::getAdvertisingName(adv, sizeof(adv));
  snprintf(pinLine, sizeof(pinLine), "%s %06u", locale::getForDisplay("pin"), (unsigned)ble::getPasskey());
  strncpy(modeLine, locale::getForDisplay("net_ble"), sizeof(modeLine) - 1);
  modeLine[sizeof(modeLine) - 1] = '\0';
  const char* advLine = adv[0] ? adv : "-";
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  display_nrf::StatusScreenChrome ch;
  nrf_tab_chrome(&ch);
  display_nrf::show_net_drill(nullptr, modeLine, advLine, pinLine, s_netMenuSel, &ch);
  t114_sync_digest_after_draw();
#else
  display_nrf::show_net_drill(nullptr, modeLine, advLine, pinLine, s_netMenuSel, nullptr);
#endif
}

static void draw_sys_menu() {
  const int n = sys_sys_menu_row_count();
  if (s_sysMenuSel < 0) s_sysMenuSel = 0;
  if (s_sysMenuSel >= n) s_sysMenuSel = n - 1;

  static char buf[8][28];
  static const char* ptrs[8];
  for (int i = 0; i < n && i < 8; i++) {
    if (s_sysInDisplaySubmenu) {
      const int bi = sys_display_sub_back_idx();
      if (i == bi) {
        strncpy(buf[i], locale::getForDisplay("menu_back"), sizeof(buf[i]) - 1);
        buf[i][sizeof(buf[i]) - 1] = 0;
      } else {
        fill_sys_display_sub_label(i, buf[i], sizeof(buf[i]));
      }
    } else {
      if (i == 6) {
        strncpy(buf[i], locale::getForDisplay("menu_back"), sizeof(buf[i]) - 1);
        buf[i][sizeof(buf[i]) - 1] = 0;
      } else {
        fill_sys_main_label(i, buf[i], sizeof(buf[i]));
      }
    }
    ptrs[i] = buf[i];
  }
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  /* Как display.cpp drawContentSys: главное меню — иконки у строк; подменю «Дисплей» — только текст. */
  display_nrf::StatusScreenChrome ch;
  nrf_tab_chrome(&ch);
  const char* secTitle = nullptr;
  static const uint8_t* drillIcons[8];
  const uint8_t* const* iconArg = nullptr;
  if (!s_sysInDisplaySubmenu) {
    for (int i = 0; i < n; i++) {
      /* Строка «Назад» — только текст, без иконки (как на OLED display.cpp для idx==6). */
      drillIcons[i] = (i == 6) ? nullptr : ui_icons::sysMenuIcon(i);
    }
    iconArg = drillIcons;
  }
  display_nrf::show_menu_list(secTitle, ptrs, n, s_sysMenuSel, 0, nullptr, iconArg, &ch);
  t114_sync_digest_after_draw();
#else
  nrf_show_menu(nullptr, ptrs, n, s_sysMenuSel, 0, nullptr, nullptr);
#endif
}

/** Как displayRunModemScan: ожидание, modemScan(6), затем меню результатов. */
static void run_modem_scan_ui(View returnAfter) {
  char waitBody[96];
  snprintf(waitBody, sizeof(waitBody), "%s\n~36s ...", locale::getForDisplay("scanning"));
  nrf_show_fs("", waitBody);
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  t114_sync_digest_after_draw();
#endif
  s_scanFound = selftest::modemScan(s_scanResults, 6);
  s_scanReturnView = returnAfter;
  s_view = View::SysModemScanMenu;
  if (s_scanFound <= 0) {
    s_scanMenuCount = 2;
    s_scanMenuSel = 1;
  } else {
    int nShow = s_scanFound > 4 ? 4 : s_scanFound;
    s_scanMenuCount = nShow + 2;
    s_scanMenuSel = s_scanMenuCount - 1;
  }
  draw_modem_scan_menu();
}

static void run_selftest_ui() {
  selftest::Result r;
  selftest::run(&r);
  /* show_selftest_summary уже вызывается в selftest::run для nRF */
  s_view = View::SysAfterSelftest;
}

static void draw_home_menu() {
  const int n = display_tabs::homeMenuCount();
  if (n < 1) return;
  if (s_homeSel < 0) s_homeSel = 0;
  if (s_homeSel >= n) s_homeSel = n - 1;

  static char buf[8][28];
  static const char* ptrs[8];
  const int maxSlots = (int)(sizeof(buf) / sizeof(buf[0]));
  int use = n < maxSlots ? n : maxSlots;
  for (int i = 0; i < use; i++) {
    const char* k = label_key_for_home_slot(i);
    strncpy(buf[i], locale::getForDisplay(k), sizeof(buf[i]) - 1);
    buf[i][sizeof(buf[i]) - 1] = 0;
    ptrs[i] = buf[i];
  }
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  static const uint8_t* homeIcons[8];
  for (int i = 0; i < use; i++) {
    homeIcons[i] = display_tabs::iconForContent(display_tabs::homeMenuContentAt(i));
  }
  display_nrf::StatusScreenChrome ch;
  nrf_tab_chrome(&ch);
  /* Как drawHomeMenu на ESP: без заголовка над списком — chrome и строки с иконками. */
  display_nrf::show_menu_list(nullptr, ptrs, use, s_homeSel, s_homeScroll, nullptr, homeIcons, &ch);
  s_homeScroll = display_nrf::menu_list_last_scroll();
  t114_sync_digest_after_draw();
#else
  nrf_show_menu(locale::getForDisplay("tab_home"), ptrs, use, s_homeSel, s_homeScroll, nullptr, nullptr);
  s_homeScroll = display_nrf::menu_list_last_scroll();
#endif
}

void menu_nrf_show_boot_screen() {
  if (!display_nrf::is_ready()) return;
  s_tabDrillIn = false;
  s_sysInDisplaySubmenu = false;
  s_navTab = 0;
  s_dashPage = 0;
  if (ui_nav_mode::isTabMode()) {
    s_view = View::Dashboard;
    nrf_render_dashboard(0);
  } else {
    s_homeSel = 0;
    s_homeScroll = 0;
    s_view = View::MenuHome;
    draw_home_menu();
  }
}

static void draw_detail(display_tabs::ContentTab ct) {
  char body[480];
  body[0] = 0;

  switch (ct) {
    case display_tabs::CT_MAIN: {
      /* Как display.cpp drawContentMain: ник, ID 8 байт, соседи + средний RSSI (без блока region/SF на этой вкладке). */
      const uint8_t* nid = node::getId();
      char idHex[20];
      snprintf(idHex, sizeof(idHex), "%02X%02X%02X%02X%02X%02X%02X%02X", nid[0], nid[1], nid[2], nid[3], nid[4], nid[5],
          nid[6], nid[7]);
      char nick[33];
      node::getNickname(nick, sizeof(nick));
      const int nc = neighbors::getCount();
      const int avgRssi = neighbors::getAverageRssi();
      if (nick[0]) {
        if (nc > 0) {
          snprintf(body, sizeof(body), "%s\n%s %s\n%s: %d %ddBm", nick, locale::getForDisplay("id"), idHex,
              locale::getForDisplay("neighbors"), nc, avgRssi);
        } else {
          snprintf(body, sizeof(body), "%s\n%s %s\n%s: 0", nick, locale::getForDisplay("id"), idHex,
              locale::getForDisplay("neighbors"));
        }
      } else {
        if (nc > 0) {
          snprintf(body, sizeof(body), "%s\n%s: %d %ddBm", idHex, locale::getForDisplay("neighbors"), nc, avgRssi);
        } else {
          snprintf(body, sizeof(body), "%s\n%s: 0", idHex, locale::getForDisplay("neighbors"));
        }
      }
      break;
    }
    case display_tabs::CT_MSG: {
      char from[20], text[48];
      display_nrf::get_last_msg_peek(from, sizeof(from), text, sizeof(text));
      /* Как drawContentMsg: «От …» на первой строке, текст ниже. */
      snprintf(body, sizeof(body), "%s %s\n%s", locale::getForDisplay("from"), from[0] ? from : "-",
          text[0] ? text : locale::getForDisplay("no_messages"));
      break;
    }
    case display_tabs::CT_INFO: {
      int nc = neighbors::getCount();
      snprintf(body, sizeof(body), "%s: %d\n", locale::getForDisplay("neighbors"), nc);
      size_t off = strlen(body);
      constexpr int kPeerLines = 8;
      for (int i = 0; i < nc && i < kPeerLines && off < sizeof(body) - 24; i++) {
        char hx[20];
        neighbors::getIdHex(i, hx);
        int r = neighbors::getRssi(i);
        /* Как drawContentInfo: 4 символа id + dBm. */
        off += (size_t)snprintf(body + off, sizeof(body) - off, "%.4s %ddBm\n", hx, r);
      }
      if (nc > kPeerLines && off < sizeof(body) - 20) {
        snprintf(body + off, sizeof(body) - off, "+%d", nc - kPeerLines);
      }
      break;
    }
    case display_tabs::CT_GPS: {
      /* Как display.cpp drawContentGps: при модуле на шине — строка вкл/выкл после long (табы) или сразу (список). */
      const bool tabs = ui_nav_mode::isTabMode();
      const bool showToggle = gps::isPresent() && (!tabs || s_tabDrillIn);
#if defined(RIFTLINK_BOARD_HELTEC_T114)
      if (showToggle) {
        if (s_gpsMenuIndex < 0 || s_gpsMenuIndex > 1) s_gpsMenuIndex = 0;
        static char toggLabel[36];
        static char backStr[28];
        static const char* rows[2];
        strncpy(toggLabel,
            locale::getForDisplay(gps::isEnabled() ? "gps_toggle_on_off" : "gps_toggle_off_on"), sizeof(toggLabel) - 1);
        toggLabel[sizeof(toggLabel) - 1] = 0;
        strncpy(backStr, locale::getForDisplay("menu_back"), sizeof(backStr) - 1);
        backStr[sizeof(backStr) - 1] = 0;
        rows[0] = toggLabel;
        rows[1] = backStr;
        static char foot[72];
        if (!gps::isEnabled()) {
          snprintf(foot, sizeof(foot), "%s", locale::getForDisplay("gps_off"));
        } else if (!gps::hasFix()) {
          snprintf(foot, sizeof(foot), "%s", locale::getForDisplay("gps_search"));
        } else {
          snprintf(foot, sizeof(foot), "%.5f %.5f", (double)gps::getLat(), (double)gps::getLon());
        }
        display_nrf::StatusScreenChrome ch;
        nrf_tab_chrome(&ch);
        display_nrf::show_menu_list(nullptr, rows, 2, s_gpsMenuIndex, 0, foot, nullptr, &ch);
        t114_sync_digest_after_draw();
        return;
      }
#endif
      if (!gps::isPresent()) {
        snprintf(body, sizeof(body), "%s", locale::getForDisplay("gps_not_present"));
      } else if (gps::isEnabled() && !gps::hasFix()) {
        snprintf(body, sizeof(body), "%s", locale::getForDisplay("gps_search"));
      } else if (gps::isEnabled() && gps::hasFix()) {
        uint32_t sat = gps::getSatellites();
        float course = gps::getCourseDeg();
        const char* card = gps::getCourseCardinal();
        char line1[48];
        if (course >= 0 && card && card[0])
          snprintf(line1, sizeof(line1), "%u sat %.0f %s", (unsigned)sat, (double)course, card);
        else
          snprintf(line1, sizeof(line1), "%u sat", (unsigned)sat);
        snprintf(body, sizeof(body), "%s\n%.5f %.5f", line1, (double)gps::getLat(), (double)gps::getLon());
      } else if (gps::hasFix()) {
        snprintf(body, sizeof(body), "%.5f %.5f", (double)gps::getLat(), (double)gps::getLon());
      } else {
        snprintf(body, sizeof(body), "%s", locale::getForDisplay("gps_off"));
      }
      break;
    }
    default:
      snprintf(body, sizeof(body), "%s", locale::getForDisplay("detail_unknown_body"));
      break;
  }
  /* Заголовок не дублируем: раздел уже виден по вкладке / пункту меню. */
  nrf_show_fs("", body);
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  t114_sync_digest_after_draw();
#endif
}

static void draw_power_screen() {
  /* Как display.cpp drawContentPower: в табах до long — 2 строки; после long — 3 с выделением. */
  static char buf[3][28];
  static const char* ptrs[3];
  strncpy(buf[0], locale::getForDisplay("menu_power_off"), sizeof(buf[0]) - 1);
  buf[0][sizeof(buf[0]) - 1] = '\0';
  strncpy(buf[1], locale::getForDisplay("menu_power_sleep"), sizeof(buf[1]) - 1);
  buf[1][sizeof(buf[1]) - 1] = '\0';
  strncpy(buf[2], locale::getForDisplay("menu_back"), sizeof(buf[2]) - 1);
  buf[2][sizeof(buf[2]) - 1] = '\0';
  ptrs[0] = buf[0];
  ptrs[1] = buf[1];
  ptrs[2] = buf[2];

  const bool tabs = ui_nav_mode::isTabMode();
  const int nLines = (!tabs || s_tabDrillIn) ? 3 : 2;
  int sel;
  if (tabs && s_tabDrillIn) {
    if (s_powerMenuSel < 0 || s_powerMenuSel > 2) s_powerMenuSel = 0;
    sel = s_powerMenuSel;
  } else if (!tabs) {
    /* Режим списка: три строки с курсором (как после long на вкладке POWER). */
    if (s_powerMenuSel < 0 || s_powerMenuSel > 2) s_powerMenuSel = 0;
    sel = s_powerMenuSel;
  } else {
    sel = -1;
  }
  const char* secTitle = nullptr;
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  display_nrf::StatusScreenChrome ch;
  nrf_tab_chrome(&ch);
  nrf_show_menu(secTitle, ptrs, nLines, sel, 0, nullptr, nullptr);
  t114_sync_digest_after_draw();
#else
  nrf_show_menu(secTitle, ptrs, nLines, sel, 0, nullptr, nullptr);
#endif
}

static void advance_nav_tab() {
  const int n = display_tabs::getNavTabCount();
  const int prevTab = s_navTab;
  const display_tabs::ContentTab prevCt = display_tabs::contentForNavTab(prevTab);
  /* Как displayShowScreen при смене вкладки: сброс drill и курсора POWER. */
  s_tabDrillIn = false;
  s_sysInDisplaySubmenu = false;
  s_powerMenuSel = 0;
  s_gpsMenuIndex = 0;
  s_navTab = (s_navTab + 1) % n;
  const display_tabs::ContentTab nextCt = display_tabs::contentForNavTab(s_navTab);
  const bool sysToSys = (prevCt == display_tabs::CT_SYS && nextCt == display_tabs::CT_SYS);
  if (!sysToSys && nextCt == display_tabs::CT_SYS && prevCt != display_tabs::CT_SYS) {
    s_sysMenuSel = 0;
  }
}

static void draw_net_tab_summary() {
  /* Как V3 drawContentNet в BLE: режим, имя рекламы, PIN. На nRF нет Wi‑Fi — не net_mode_line_ble («BLE-WiFi»). */
  char body[200];
  char adv[24];
  ble::getAdvertisingName(adv, sizeof(adv));
  snprintf(body, sizeof(body), "%s\n%s\n%s %06u", locale::getForDisplay("net_ble"), adv[0] ? adv : "-",
      locale::getForDisplay("pin"), (unsigned)ble::getPasskey());
  nrf_show_fs("", body);
  s_view = View::Detail;
  s_detailTab = display_tabs::CT_NET;
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  t114_sync_digest_after_draw();
#endif
}

static void draw_sys_tab_summary() {
  /* Как display.cpp drawContentSys при sysTabBrowse: 6 пунктов с иконками, без «Назад» и без полосы заголовка (только вкладки). */
  static char buf[6][28];
  static const char* ptrs[6];
  for (int i = 0; i < 6; i++) {
    fill_sys_main_label(i, buf[i], sizeof(buf[i]));
    ptrs[i] = buf[i];
  }
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  display_nrf::StatusScreenChrome ch;
  nrf_tab_chrome(&ch);
  const char* secTitle = nullptr;
  static const uint8_t* sumIcons[6];
  for (int i = 0; i < 6; i++) sumIcons[i] = ui_icons::sysMenuIcon(i);
  display_nrf::show_menu_list(secTitle, ptrs, 6, -1, 0, nullptr, sumIcons, &ch);
  t114_sync_digest_after_draw();
#else
  nrf_show_menu(nullptr, ptrs, 6, -1, 0, nullptr, nullptr);
#endif
  s_view = View::Detail;
  s_detailTab = display_tabs::CT_SYS;
}

static void render_nav_tab() {
  const int n = display_tabs::getNavTabCount();
  if (s_navTab < 0) s_navTab = 0;
  if (s_navTab >= n) s_navTab = n - 1;
  const display_tabs::ContentTab ct = display_tabs::contentForNavTab(s_navTab);
  switch (ct) {
    case display_tabs::CT_MAIN:
      /* Как V3 drawContentMain: ник / ID / соседи (не 4-строчный «дашборд»). */
      s_detailTab = ct;
      s_view = View::Detail;
      draw_detail(ct);
      break;
    case display_tabs::CT_MSG:
      s_detailTab = ct;
      s_view = View::Detail;
      draw_detail(ct);
      break;
    case display_tabs::CT_INFO:
      s_detailTab = ct;
      s_view = View::Detail;
      draw_detail(ct);
      break;
    case display_tabs::CT_GPS:
      s_detailTab = ct;
      s_view = View::Detail;
      draw_detail(ct);
      break;
    case display_tabs::CT_NET:
      if (s_tabDrillIn) {
        s_view = View::NetMenu;
        draw_net_menu();
      } else {
        draw_net_tab_summary();
      }
      break;
    case display_tabs::CT_SYS:
      if (s_tabDrillIn) {
        s_view = View::SysMenu;
        draw_sys_menu();
      } else {
        draw_sys_tab_summary();
      }
      break;
    case display_tabs::CT_POWER:
      s_view = View::Power;
      draw_power_screen();
      break;
    default:
      break;
  }
}

static void open_detail_for_selection() {
  const int n = display_tabs::homeMenuCount();
  if (s_homeSel < 0 || s_homeSel >= n) return;
  if (display_tabs::homeMenuIsPowerSlot(s_homeSel)) {
    s_tabDrillIn = false;
    s_powerMenuSel = 0;
    s_view = View::Power;
    draw_power_screen();
    return;
  }
  const display_tabs::ContentTab ct = display_tabs::homeMenuContentAt(s_homeSel);
  if (ct == display_tabs::CT_NET) {
    s_view = View::NetMenu;
    s_netMenuSel = 0;
    draw_net_menu();
    return;
  }
  if (ct == display_tabs::CT_SYS) {
    s_view = View::SysMenu;
    s_sysMenuSel = 0;
    s_sysInDisplaySubmenu = false;
    draw_sys_menu();
    return;
  }
  s_detailTab = ct;
  s_view = View::Detail;
  if (ct == display_tabs::CT_GPS) s_gpsMenuIndex = 0;
  draw_detail(ct);
}

void menu_nrf_init() {
  s_view = View::MenuHome;
  s_dashPage = 0;
  s_homeSel = 0;
  s_homeScroll = 0;
  s_navTab = 0;
  s_tabDrillIn = false;
  s_netMenuSel = 0;
  s_sysMenuSel = 0;
  s_powerMenuSel = 0;
  s_gpsMenuIndex = 0;
  s_sysInDisplaySubmenu = false;
  s_detailTab = display_tabs::CT_MAIN;
}

void menu_nrf_redraw_after_locale() {
  if (!display_nrf::is_ready()) return;
  switch (s_view) {
    case View::Dashboard:
      nrf_render_dashboard(s_dashPage);
      break;
    case View::MenuHome:
      draw_home_menu();
      break;
    case View::NetMenu:
      draw_net_menu();
      break;
    case View::SysMenu:
      draw_sys_menu();
      break;
    case View::SysModemPicker:
      draw_modem_picker_screen();
      break;
    case View::SysRegionPicker:
      draw_region_picker_screen();
      break;
    case View::SysRegionChannelPicker:
      draw_region_channel_picker_screen();
      break;
    case View::SysModemScanMenu:
      draw_modem_scan_menu();
      break;
    case View::InfoScreen:
      nrf_show_fs(s_infoTitle, s_infoBody);
      break;
    case View::Power:
      draw_power_screen();
      break;
    case View::Detail:
      if (ui_nav_mode::isTabMode() && !s_tabDrillIn &&
          (s_detailTab == display_tabs::CT_NET || s_detailTab == display_tabs::CT_SYS)) {
        if (s_detailTab == display_tabs::CT_NET)
          draw_net_tab_summary();
        else
          draw_sys_tab_summary();
      } else {
        draw_detail(s_detailTab);
      }
      break;
    case View::SysAfterSelftest:
      break;
    default:
      break;
  }
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  t114_sync_digest_after_draw();
#endif
}

void menu_nrf_open_menu() {
  if (!display_nrf::is_ready()) return;
  s_view = View::MenuHome;
  s_homeSel = 0;
  s_homeScroll = 0;
  s_netMenuSel = 0;
  s_sysMenuSel = 0;
  s_powerMenuSel = 0;
  s_sysInDisplaySubmenu = false;
  draw_home_menu();
}

void menu_nrf_dashboard_next_page() {
  if (ui_nav_mode::isTabMode()) {
    advance_nav_tab();
    s_view = View::Dashboard;
    nrf_render_dashboard(s_dashPage);
    return;
  }
  s_dashPage = (uint8_t)((s_dashPage + 1) % 3);
  s_view = View::Dashboard;
  nrf_render_dashboard(s_dashPage);
}

uint8_t menu_nrf_dashboard_page() {
  return s_dashPage;
}

void menu_nrf_set_dashboard_page(uint8_t page) {
  if (page > 2) page = 0;
  s_dashPage = page;
}

void menu_nrf_goto_dashboard(uint8_t page) {
  if (page > 2) page = 0;
  s_dashPage = page;
  s_view = View::Dashboard;
  s_tabDrillIn = false;
  s_sysInDisplaySubmenu = false;
  nrf_render_dashboard(page);
}

#if defined(RIFTLINK_BOARD_HELTEC_T114)

void menu_nrf_periodic_refresh(uint32_t) {
  if (!display_nrf::is_ready()) return;
  /* Полный дайджест меняется от эфира/АКБ/времени; тело меню «Режим»/SYS/Power — нет. Без чёрной вспышки: только полоска. */
  const uint64_t d = t114_compute_digest(true);
  if (d == s_t114LiveDigest) return;
  if (t114_periodic_allow_chrome_only_refresh()) {
    const uint64_t sk = t114_compute_digest(false);
    if (sk == s_t114MenuSkeletonDigest) {
      display_nrf::StatusScreenChrome ch;
      nrf_tab_chrome(&ch);
      display_nrf::refresh_top_chrome_only(&ch);
      s_t114LiveDigest = d;
      return;
    }
  }
  menu_nrf_redraw_after_locale();
}

void menu_nrf_tab_idle_tick() {
  ui_tab_bar_idle::tick(s_tabDrillIn);
  static bool s_prevStrip = true;
  if (!ui_nav_mode::isTabMode()) {
    s_prevStrip = true;
    return;
  }
  const bool sh = ui_tab_bar_idle::tabStripVisible();
  if (sh != s_prevStrip) menu_nrf_redraw_after_locale();
  s_prevStrip = sh;
}

static void on_short_press() {
  if (!display_nrf::is_ready()) return;
  switch (s_view) {
    case View::Dashboard:
      if (ui_nav_mode::isTabMode()) {
        ui_tab_bar_idle::tick(s_tabDrillIn);
        if (ui_tab_bar_idle::tryRevealFirstShortOnly()) {
          nrf_render_dashboard(s_dashPage);
          break;
        }
        /* Короткое нажатие — следующая вкладка (как полоска на V3/V4). */
        advance_nav_tab();
        s_view = View::Dashboard;
        nrf_render_dashboard(s_dashPage);
      } else {
        s_dashPage = (uint8_t)((s_dashPage + 1) % 3);
        nrf_render_dashboard(s_dashPage);
      }
      break;
    case View::MenuHome: {
      const int n = display_tabs::homeMenuCount();
      if (n > 0) s_homeSel = (s_homeSel + 1) % n;
      draw_home_menu();
      break;
    }
    case View::NetMenu:
      s_netMenuSel = (s_netMenuSel + 1) % kNetMenuItems;
      draw_net_menu();
      break;
    case View::SysMenu: {
      const int nrows = sys_sys_menu_row_count();
      s_sysMenuSel = (s_sysMenuSel + 1) % nrows;
      draw_sys_menu();
      break;
    }
    case View::SysModemPicker:
      s_modemPickerSel = (s_modemPickerSel + 1) % 6;
      draw_modem_picker_screen();
      break;
    case View::SysRegionPicker:
      if (s_regionPickerCount > 0)
        s_regionPickerSel = (s_regionPickerSel + 1) % s_regionPickerCount;
      draw_region_picker_screen();
      break;
    case View::SysRegionChannelPicker:
      if (s_channelPickerCount > 0)
        s_channelPickerSel = (s_channelPickerSel + 1) % s_channelPickerCount;
      draw_region_channel_picker_screen();
      break;
    case View::SysModemScanMenu:
      if (s_scanMenuCount > 0)
        s_scanMenuSel = (s_scanMenuSel + 1) % s_scanMenuCount;
      draw_modem_scan_menu();
      break;
    case View::InfoScreen:
      s_view = s_infoReturnView;
      if (s_view == View::NetMenu)
        draw_net_menu();
      else if (s_view == View::SysMenu)
        draw_sys_menu();
      else if (s_view == View::Power)
        draw_power_screen();
      else {
        menu_nrf_show_boot_screen();
      }
      break;
    case View::SysAfterSelftest:
      s_view = View::SysMenu;
      draw_sys_menu();
      break;
    case View::Power:
      if (ui_nav_mode::isTabMode()) {
        ui_tab_bar_idle::tick(s_tabDrillIn);
        if (ui_tab_bar_idle::tryRevealFirstShortOnly()) {
          menu_nrf_redraw_after_locale();
          break;
        }
        if (s_tabDrillIn) {
          /* Как displayCycleShort: на «Назад» short выходит из drill, иначе листает 0→1→2. */
          if (s_powerMenuSel == 2) {
            s_tabDrillIn = false;
            s_powerMenuSel = 0;
            draw_power_screen();
          } else {
            s_powerMenuSel = (s_powerMenuSel + 1) % 3;
            draw_power_screen();
          }
        } else {
          advance_nav_tab();
          s_view = View::Dashboard;
          nrf_render_dashboard(s_dashPage);
        }
      } else {
        /* Режим списка: короткое листает пункты; на «Назад» — в главное меню. */
        if (s_powerMenuSel < 0 || s_powerMenuSel > 2) s_powerMenuSel = 0;
        if (s_powerMenuSel == 2) {
          s_powerMenuSel = 0;
          s_view = View::MenuHome;
          draw_home_menu();
        } else {
          s_powerMenuSel = s_powerMenuSel + 1;
          draw_power_screen();
        }
      }
      break;
    case View::Detail:
      if (ui_nav_mode::isTabMode()) {
        ui_tab_bar_idle::tick(s_tabDrillIn);
        /* Как V3: пока полоска скрыта по idle — первое короткое только показывает вкладки, не листает. */
        if (ui_tab_bar_idle::tryRevealFirstShortOnly()) {
          menu_nrf_redraw_after_locale();
          break;
        }
        {
          const display_tabs::ContentTab ct = display_tabs::contentForNavTab(s_navTab);
          if (ct == display_tabs::CT_GPS && gps::isPresent() && s_tabDrillIn) {
            s_gpsMenuIndex = (s_gpsMenuIndex + 1) % 2;
            draw_detail(display_tabs::CT_GPS);
            break;
          }
        }
        advance_nav_tab();
        s_view = View::Dashboard;
        nrf_render_dashboard(s_dashPage);
      } else {
        if (s_detailTab == display_tabs::CT_GPS && gps::isPresent()) {
          s_gpsMenuIndex = (s_gpsMenuIndex + 1) % 2;
          draw_detail(display_tabs::CT_GPS);
          break;
        }
        s_view = View::MenuHome;
        draw_home_menu();
      }
      break;
    default:
      break;
  }
  /* onInput() нельзя вызывать до tryReveal (poll): иначе полоска уже «видима» и листаем вкладку с первого short. */
  if (ui_nav_mode::isTabMode()) {
    ui_tab_bar_idle::onInput();
  }
}

static void on_long_press() {
  if (!display_nrf::is_ready()) return;
  switch (s_view) {
    case View::Dashboard:
      if (ui_nav_mode::isTabMode()) {
        const display_tabs::ContentTab ct = display_tabs::contentForNavTab(s_navTab);
        if (ct == display_tabs::CT_MAIN) {
          break;
        }
        if (ct == display_tabs::CT_MSG || ct == display_tabs::CT_INFO) {
          s_tabDrillIn = false;
          s_navTab = 0;
          nrf_render_dashboard(s_dashPage);
          break;
        }
        if (ct == display_tabs::CT_GPS) {
          s_tabDrillIn = true;
          s_gpsMenuIndex = 0;
          s_view = View::Detail;
          s_detailTab = display_tabs::CT_GPS;
          draw_detail(display_tabs::CT_GPS);
          break;
        }
        if (ct == display_tabs::CT_NET) {
          s_tabDrillIn = true;
          s_netMenuSel = 0;
          s_view = View::NetMenu;
          draw_net_menu();
        } else if (ct == display_tabs::CT_SYS) {
          s_tabDrillIn = true;
          s_sysMenuSel = 0;
          s_sysInDisplaySubmenu = false;
          s_view = View::SysMenu;
          draw_sys_menu();
        } else if (ct == display_tabs::CT_POWER) {
          s_tabDrillIn = true;
          s_powerMenuSel = 0;
          s_view = View::Power;
          draw_power_screen();
        }
      } else {
        s_view = View::MenuHome;
        s_homeSel = 0;
        s_homeScroll = 0;
        draw_home_menu();
      }
      break;
    case View::MenuHome:
      open_detail_for_selection();
      break;
    case View::NetMenu:
      /* Как V3/V4: short листает режим/Назад; long на режиме — на ESP смена BLE/WiFi, на nRF Wi‑Fi нет; long на Назад — выход. */
      if (s_netMenuSel == 1) {
        if (ui_nav_mode::isTabMode()) {
          s_tabDrillIn = false;
          s_view = View::Dashboard;
          nrf_render_dashboard(s_dashPage);
        } else {
          s_view = View::MenuHome;
          draw_home_menu();
        }
      }
      break;
    case View::SysMenu:
      if (s_sysInDisplaySubmenu) {
        const int bi = sys_display_sub_back_idx();
        if (s_sysMenuSel == bi) {
          s_sysInDisplaySubmenu = false;
          s_sysMenuSel = 0;
        } else if (s_sysMenuSel == 0) {
          nrf_ui_run_language_until_done();
        } else if (s_sysMenuSel == 1) {
          (void)ui_display_prefs::setFlip180(!ui_display_prefs::getFlip180());
          display_nrf::apply_rotation_from_prefs();
          menu_nrf_redraw_after_locale();
        } else if (s_sysMenuSel == 2) {
          const bool wasTabs = ui_nav_mode::isTabMode();
          const display_tabs::ContentTab styleAnchor = display_tabs::CT_SYS;
          (void)ui_nav_mode::setTabMode(!wasTabs);
          if (wasTabs) {
            s_navTab = display_tabs::listTabIndexForContent(styleAnchor);
          } else {
            s_navTab = display_tabs::navTabIndexForContent(styleAnchor);
          }
          clamp_sys_display_sub_menu_index();
          if (!wasTabs && s_sysMenuSel == 3) s_sysMenuSel = 4;
          if (ui_nav_mode::isTabMode() && s_sysInDisplaySubmenu) s_tabDrillIn = true;
        } else if (s_sysMenuSel == 3 && ui_nav_mode::isTabMode()) {
          ui_display_prefs::cycleTabBarHideIdleSeconds();
        }
        draw_sys_menu();
      } else if (s_sysMenuSel == 6) {
        if (ui_nav_mode::isTabMode()) {
          s_sysMenuSel = 0;
          s_tabDrillIn = false;
          s_sysInDisplaySubmenu = false;
          s_view = View::Dashboard;
          nrf_render_dashboard(s_dashPage);
        } else {
          s_view = View::MenuHome;
          draw_home_menu();
        }
      } else if (s_sysMenuSel == 0) {
        s_sysInDisplaySubmenu = true;
        s_sysMenuSel = 0;
        draw_sys_menu();
      } else {
        const int e = sys_main_menu_index_to_exec_sel(s_sysMenuSel);
        if (e == 2) {
          s_infoTitle[0] = 0;
          snprintf(s_infoBody, sizeof(s_infoBody), "%s", locale::getForDisplay("power_nrf_note"));
          s_infoReturnView = View::SysMenu;
          s_view = View::InfoScreen;
          nrf_show_fs("", s_infoBody);
        } else if (e == 3) {
          open_region_picker_from_sys();
        } else if (e == 0) {
          open_modem_picker_from_sys();
        } else if (e == 1) {
          run_modem_scan_ui(View::SysMenu);
        } else if (e == 4) {
          run_selftest_ui();
        }
      }
      break;
    case View::SysModemPicker:
      if (s_modemPickerSel == 5) {
        s_view = View::SysMenu;
        draw_sys_menu();
      } else if (s_modemPickerSel >= 0 && s_modemPickerSel < 4) {
        (void)radio::requestModemPreset((radio::ModemPreset)s_modemPickerSel);
        s_view = View::SysMenu;
        draw_sys_menu();
      } else {
        /* Custom (4): как на ESP — без смены пресета, только выход. */
        s_view = View::SysMenu;
        draw_sys_menu();
      }
      break;
    case View::SysRegionPicker: {
      const int backIdx = s_regionPickerCount > 0 ? s_regionPickerCount - 1 : 0;
      if (s_regionPickerSel == backIdx) {
        s_view = View::SysMenu;
        draw_sys_menu();
      } else {
        apply_region_picker_and_maybe_channel();
      }
      break;
    }
    case View::SysRegionChannelPicker: {
      const int backIdx = s_channelPickerCount > 0 ? s_channelPickerCount - 1 : 0;
      if (s_channelPickerSel == backIdx) {
        s_view = View::SysMenu;
        draw_sys_menu();
      } else {
        const int nCh = region::getChannelCount();
        if (s_channelPickerSel >= 0 && s_channelPickerSel < nCh) (void)region::setChannel(s_channelPickerSel);
        s_view = View::SysMenu;
        draw_sys_menu();
      }
      break;
    }
    case View::SysModemScanMenu: {
      if (s_scanMenuCount > 0 && s_scanMenuSel == s_scanMenuCount - 1) {
        s_view = s_scanReturnView;
        if (s_view == View::SysMenu)
          draw_sys_menu();
        else
          menu_nrf_show_boot_screen();
      } else {
        draw_modem_scan_menu();
      }
      break;
    }
    case View::InfoScreen:
      s_view = s_infoReturnView;
      if (s_view == View::NetMenu)
        draw_net_menu();
      else if (s_view == View::SysMenu)
        draw_sys_menu();
      else if (s_view == View::Power)
        draw_power_screen();
      else {
        menu_nrf_show_boot_screen();
      }
      break;
    case View::SysAfterSelftest:
      menu_nrf_show_boot_screen();
      break;
    case View::Power:
      if (ui_nav_mode::isTabMode()) {
        if (s_tabDrillIn) {
          if (s_powerMenuSel == 2) {
            s_tabDrillIn = false;
            s_powerMenuSel = 0;
            draw_power_screen();
          } else {
            s_infoTitle[0] = 0;
            snprintf(s_infoBody, sizeof(s_infoBody), "%s", locale::getForDisplay("power_nrf_note"));
            s_infoReturnView = View::Power;
            s_view = View::InfoScreen;
            nrf_show_fs("", s_infoBody);
#if defined(RIFTLINK_BOARD_HELTEC_T114)
            t114_sync_digest_after_draw();
#endif
          }
        } else {
          s_tabDrillIn = true;
          s_powerMenuSel = 0;
          draw_power_screen();
        }
      } else {
        /* Режим списка: long — «Назад» в главное меню; иначе — то же подтверждение, что в drill. */
        if (s_powerMenuSel < 0 || s_powerMenuSel > 2) s_powerMenuSel = 0;
        if (s_powerMenuSel == 2) {
          s_powerMenuSel = 0;
          s_view = View::MenuHome;
          draw_home_menu();
        } else {
          s_infoTitle[0] = 0;
          snprintf(s_infoBody, sizeof(s_infoBody), "%s", locale::getForDisplay("power_nrf_note"));
          s_infoReturnView = View::Power;
          s_view = View::InfoScreen;
          nrf_show_fs("", s_infoBody);
#if defined(RIFTLINK_BOARD_HELTEC_T114)
          t114_sync_digest_after_draw();
#endif
        }
      }
      break;
    case View::Detail:
      if (ui_nav_mode::isTabMode()) {
        const display_tabs::ContentTab ct = display_tabs::contentForNavTab(s_navTab);
        /* Как displayOnLongPress: на MAIN в табах long — no-op; на MSG/INFO — переход на вкладку MAIN (s_currentScreen=0). */
        if (ct == display_tabs::CT_MAIN) {
          break;
        }
        if (ct == display_tabs::CT_MSG || ct == display_tabs::CT_INFO) {
          s_tabDrillIn = false;
          s_navTab = 0;
          nrf_render_dashboard(s_dashPage);
          break;
        }
        if (ct == display_tabs::CT_GPS) {
          if (!s_tabDrillIn) {
            s_tabDrillIn = true;
            s_gpsMenuIndex = 0;
            draw_detail(display_tabs::CT_GPS);
            break;
          }
          if (!gps::isPresent() || s_gpsMenuIndex == 1) {
            s_tabDrillIn = false;
            s_gpsMenuIndex = 0;
            draw_detail(display_tabs::CT_GPS);
          } else {
            if (gps::isPresent()) gps::toggle();
            draw_detail(display_tabs::CT_GPS);
          }
          break;
        }
        if (ct == display_tabs::CT_NET && !s_tabDrillIn) {
          s_tabDrillIn = true;
          s_netMenuSel = 0;
          s_view = View::NetMenu;
          draw_net_menu();
          break;
        }
        if (ct == display_tabs::CT_SYS && !s_tabDrillIn) {
          s_tabDrillIn = true;
          s_sysMenuSel = 0;
          s_sysInDisplaySubmenu = false;
          s_view = View::SysMenu;
          draw_sys_menu();
          break;
        }
        s_tabDrillIn = false;
        s_view = View::Dashboard;
        nrf_render_dashboard(s_dashPage);
      } else {
        if (s_detailTab == display_tabs::CT_GPS) {
          if (!gps::isPresent() || s_gpsMenuIndex == 1) {
            s_gpsMenuIndex = 0;
            s_view = View::MenuHome;
            draw_home_menu();
          } else {
            if (gps::isPresent()) gps::toggle();
            draw_detail(display_tabs::CT_GPS);
          }
        } else {
          s_view = View::MenuHome;
          draw_home_menu();
        }
      }
      break;
    default:
      break;
  }
}

void menu_nrf_poll_t114_button(bool pressed, uint32_t now_ms) {
  if (pressed && !s_btnPrev) {
    s_btnDownAt = now_ms;
  }
  if (!pressed && s_btnPrev) {
    uint32_t dur = now_ms - s_btnDownAt;
    if (dur >= kDebounceMs) {
      if (dur < kShortMs) {
        on_short_press();
      } else {
        ui_tab_bar_idle::onInput();
        on_long_press();
      }
    }
  }
  s_btnPrev = pressed;
}
#else
void menu_nrf_periodic_refresh(uint32_t) {}
void menu_nrf_tab_idle_tick() {}
void menu_nrf_poll_t114_button(bool, uint32_t) {}
#endif
