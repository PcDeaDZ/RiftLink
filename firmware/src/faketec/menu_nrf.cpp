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
#include "version.h"
#include "x25519_keys/x25519_keys.h"
#include "ui/ui_display_prefs.h"
#include "ui/ui_tab_bar_idle.h"
#include "nrf_ui_parity.h"

#include <Arduino.h>
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

static uint32_t nrf_heap_kb_local() {
  size_t f = xPortGetFreeHeapSize();
#if RIFTLINK_HAS_NRF_SDH
  uint32_t sd = nrf_sdh_get_free_heap_size();
  if (sd > f) f = sd;
#endif
  return (uint32_t)(f / 1024U);
}

/** Те же 4 строки, что на дашборде в режиме списка — для вкладки MAIN в режиме вкладок (как плотный экран V3). */
static void fill_dashboard_lines(uint8_t page, char* l1, size_t z1, char* l2, size_t z2, char* l3, size_t z3, char* l4, size_t z4) {
  if (page > 2) page = 0;
  const uint8_t* nid = node::getId();
  char idshort[12];
  snprintf(idshort, sizeof(idshort), "%02X%02X%02X%02X", nid[0], nid[1], nid[2], nid[3]);
  if (page == 0) {
    snprintf(l1, z1, "%s %s", locale::getForDisplay("boot_line1"), RIFTLINK_VERSION);
    snprintf(l2, z2, "%s %s..", locale::getForDisplay("id"), idshort);
    snprintf(l3, z3, "%s %.2f %s%d", region::getCode(), (double)region::getFreq(), locale::getForDisplay("dash_ch"),
        region::getChannel());
    snprintf(l4, z4, "%s%u %s=%d %s", locale::getForDisplay("lora_sf"), (unsigned)radio::getSpreadingFactor(),
        locale::getForDisplay("dash_n_prefix"), neighbors::getCount(), radio::modemPresetName(radio::getModemPreset()));
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
  NetModemEdit,
  NetRegionEdit,
  InfoScreen,
  SysAfterSelftest,
};

static View s_view = View::Dashboard;
static uint8_t s_dashPage = 0;
static int s_homeSel = 0;
/** Индекс первой видимой строки главного меню (длинные списки на T114). */
static int s_homeScroll = 0;
static int s_navTab = 0;
static bool s_tabDrillIn = false;

void nrf_render_dashboard(uint8_t page) {
  if (!display_nrf::is_ready()) return;
  if (ui_nav_mode::isTabMode()) {
    s_dashPage = page > 2 ? 0 : page;
    render_nav_tab();
    return;
  }
  if (page > 2) page = 0;
  char l1[32], l2[32], l3[40], l4[40];
  fill_dashboard_lines(page, l1, sizeof(l1), l2, sizeof(l2), l3, sizeof(l3), l4, sizeof(l4));
  display_nrf::show_status_screen(l1, l2, l3, l4);
}

static int s_netMenuSel = 0;
static int s_sysMenuSel = 0;
/** Подменю «Дисплей» в SYS — как s_sysInDisplaySubmenu в display.cpp. */
static bool s_sysInDisplaySubmenu = false;
/** Куда вернуться с InfoScreen (скан / заглушка PS). */
static View s_infoReturnView = View::NetMenu;
/** Последний раздел Detail — для перерисовки после смены языка. */
static display_tabs::ContentTab s_detailTab = display_tabs::CT_MAIN;
/** Заголовок и текст для InfoScreen (скан эфира и т.п.). */
static char s_infoTitle[24];
static char s_infoBody[420];
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

static constexpr int kNetMenuItems = 4;
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
      snprintf(buf, bufSz, "PS:%s", locale::getForDisplay("psave"));
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

static void cycle_modem_preset() {
  int p = (int)radio::getModemPreset();
  if (p < 0 || p > 3) p = 0;
  p = (p + 1) % 4;
  (void)radio::requestModemPreset((radio::ModemPreset)p);
}

static void cycle_region() {
  int n = region::getPresetCount();
  if (n <= 0) return;
  const char* cur = region::getCode();
  int idx = 0;
  for (int i = 0; i < n; i++) {
    if (strcmp(region::getPresetCode(i), cur) == 0) {
      idx = i;
      break;
    }
  }
  idx = (idx + 1) % n;
  (void)region::setRegion(region::getPresetCode(idx));
}

static void draw_net_menu() {
  static char buf[4][28];
  static const char* ptrs[4];
  const char* keys[] = {"menu_modem", "select_country", "scan_title", "menu_back"};
  for (int i = 0; i < kNetMenuItems; i++) {
    strncpy(buf[i], locale::getForDisplay(keys[i]), sizeof(buf[i]) - 1);
    buf[i][sizeof(buf[i]) - 1] = 0;
    ptrs[i] = buf[i];
  }
  display_nrf::show_menu_list(locale::getForDisplay("menu_home_lora"), ptrs, kNetMenuItems, s_netMenuSel, 0,
      locale::getForDisplay("ui_hint_home"));
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
  display_nrf::show_menu_list(locale::getForDisplay("menu_home_settings"), ptrs, n, s_sysMenuSel, 0,
      locale::getForDisplay("ui_hint_home"));
}

static void draw_modem_edit_screen() {
  char body[280];
  snprintf(body, sizeof(body), "%s\n%s%u %s%.0f %s%u\n\n%s", radio::modemPresetName(radio::getModemPreset()),
      locale::getForDisplay("lora_sf"), (unsigned)radio::getSpreadingFactor(), locale::getForDisplay("lora_bw"),
      (double)radio::getBandwidth(), locale::getForDisplay("lora_cr"), (unsigned)radio::getCodingRate(),
      locale::getForDisplay("short_long_hint"));
  display_nrf::show_fullscreen_text(locale::getForDisplay("menu_modem"), body);
}

static void draw_region_edit_screen() {
  char body[200];
  snprintf(body, sizeof(body), "%s\n%.2f %s\n%s %d\n\n%s", region::getCode(), (double)region::getFreq(),
      locale::getForDisplay("lora_mhz"), locale::getForDisplay("dash_ch"), region::getChannel(),
      locale::getForDisplay("short_long_hint"));
  display_nrf::show_fullscreen_text(locale::getForDisplay("select_country"), body);
}

static void run_modem_scan_ui(View returnAfter) {
  selftest::ScanResult sr[8];
  int n = selftest::modemScanQuick(sr, 8);
  snprintf(s_infoTitle, sizeof(s_infoTitle), "%s", locale::getForDisplay("scan_title"));
  if (n <= 0) {
    strncpy(s_infoBody, locale::getForDisplay("scan_empty"), sizeof(s_infoBody) - 1);
    s_infoBody[sizeof(s_infoBody) - 1] = 0;
  } else {
    size_t off = 0;
    for (int i = 0; i < n && off < sizeof(s_infoBody) - 32; i++) {
      off += (size_t)snprintf(s_infoBody + off, sizeof(s_infoBody) - off, "%s%u %s%.0f %d%s\n",
          locale::getForDisplay("lora_sf"), (unsigned)sr[i].sf, locale::getForDisplay("lora_bw"), (double)sr[i].bw,
          sr[i].rssi, locale::getForDisplay("lora_dbm"));
    }
  }
  s_infoReturnView = returnAfter;
  s_view = View::InfoScreen;
  display_nrf::show_fullscreen_text(s_infoTitle, s_infoBody);
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
  display_nrf::show_menu_list(locale::getForDisplay("tab_home"), ptrs, use, s_homeSel, s_homeScroll,
      locale::getForDisplay("ui_hint_home"));
  s_homeScroll = display_nrf::menu_list_last_scroll();
}

static void draw_detail(display_tabs::ContentTab ct) {
  char body[480];
  body[0] = 0;
  const char* title = locale::getForDisplay("tab_main");

  switch (ct) {
    case display_tabs::CT_MAIN: {
      title = locale::getForDisplay("menu_home_node");
      const uint8_t* nid = node::getId();
      char idfull[protocol::NODE_ID_LEN * 2 + 1];
      for (int i = 0; i < (int)protocol::NODE_ID_LEN; i++) snprintf(idfull + i * 2, 3, "%02X", nid[i]);
      char nick[20];
      node::getNickname(nick, sizeof(nick));
      snprintf(body, sizeof(body), "%s\n%s\n%s %.2f\n%s%u %s", idfull,
          nick[0] ? nick : locale::getForDisplay("detail_unknown_body"), region::getCode(), (double)region::getFreq(),
          locale::getForDisplay("lora_sf"), (unsigned)radio::getSpreadingFactor(), radio::modemPresetName(radio::getModemPreset()));
      break;
    }
    case display_tabs::CT_MSG: {
      title = locale::getForDisplay("menu_home_msg");
      char from[20], text[48];
      display_nrf::get_last_msg_peek(from, sizeof(from), text, sizeof(text));
      if (from[0] || text[0]) {
        snprintf(body, sizeof(body), "%s\n%s", from[0] ? from : locale::getForDisplay("detail_unknown_body"),
            text[0] ? text : locale::getForDisplay("detail_unknown_body"));
      } else {
        snprintf(body, sizeof(body), "%s", locale::getForDisplay("no_messages"));
      }
      break;
    }
    case display_tabs::CT_INFO: {
      title = locale::getForDisplay("menu_home_peers");
      int nc = neighbors::getCount();
      snprintf(body, sizeof(body), "%s: %d\n", locale::getForDisplay("neighbors"), nc);
      size_t off = strlen(body);
      constexpr int kPeerLines = 8;
      for (int i = 0; i < nc && i < kPeerLines && off < sizeof(body) - 20; i++) {
        char hx[20];
        neighbors::getIdHex(i, hx);
        int r = neighbors::getRssi(i);
        uint8_t pid[protocol::NODE_ID_LEN];
        bool hk = neighbors::getId(i, pid) && x25519_keys::hasKeyFor(pid);
        off += (size_t)snprintf(body + off, sizeof(body) - off, "%s %ddBm %s\n", hx, r,
            hk ? locale::getForDisplay("peer_key_yes") : locale::getForDisplay("peer_key_no"));
      }
      break;
    }
    case display_tabs::CT_GPS: {
      title = locale::getForDisplay("tab_gps");
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
      title = locale::getForDisplay("detail_unknown_title");
      snprintf(body, sizeof(body), "%s", locale::getForDisplay("detail_unknown_body"));
      break;
  }
  display_nrf::show_fullscreen_text(title, body);
}

static void draw_power_screen() {
  uint16_t mv = telemetry::readBatteryMv();
  int pct = telemetry::batteryPercent();
  char body[200];
  snprintf(body, sizeof(body), "%s\n%umV\n", locale::getForDisplay("battery"), (unsigned)mv);
  if (pct >= 0) {
    size_t nb = strlen(body);
    snprintf(body + nb, sizeof(body) - nb, "%d%%\n", pct);
  }
  size_t n2 = strlen(body);
  snprintf(body + n2, sizeof(body) - n2, "%s", locale::getForDisplay("power_nrf_note"));
  display_nrf::show_fullscreen_text(locale::getForDisplay("menu_home_power"), body);
}

static void advance_nav_tab() {
  const int n = display_tabs::getNavTabCount();
  if (display_tabs::contentForNavTab(s_navTab) == display_tabs::CT_NET) s_tabDrillIn = false;
  if (display_tabs::contentForNavTab(s_navTab) == display_tabs::CT_SYS) s_tabDrillIn = false;
  s_navTab = (s_navTab + 1) % n;
}

static void draw_net_tab_summary() {
  char body[200];
  snprintf(body, sizeof(body), "%s\n%s", radio::modemPresetName(radio::getModemPreset()), region::getCode());
  display_nrf::show_fullscreen_text(locale::getForDisplay("menu_home_lora"), body);
  s_view = View::Detail;
  s_detailTab = display_tabs::CT_NET;
}

static void draw_sys_tab_summary() {
  char body[200];
  snprintf(body, sizeof(body), "%s\n%s\n%s", locale::getForDisplay("select_lang"), locale::getForDisplay("menu_selftest"),
      locale::getForDisplay(ui_nav_mode::isTabMode() ? "nav_mode_list" : "nav_mode_tabs"));
  display_nrf::show_fullscreen_text(locale::getForDisplay("menu_home_settings"), body);
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
      /* Вкладка «Нода»: тот же 4-строчный статус, что и дашборд в режиме списка (не пустой fullscreen). */
      s_detailTab = ct;
      s_view = View::Dashboard;
      {
        char l1[32], l2[32], l3[40], l4[40];
        fill_dashboard_lines(s_dashPage, l1, sizeof(l1), l2, sizeof(l2), l3, sizeof(l3), l4, sizeof(l4));
        display_nrf::show_status_screen(l1, l2, l3, l4);
      }
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
  draw_detail(ct);
}

void menu_nrf_init() {
  s_view = View::Dashboard;
  s_dashPage = 0;
  s_homeSel = 0;
  s_homeScroll = 0;
  s_navTab = 0;
  s_tabDrillIn = false;
  s_netMenuSel = 0;
  s_sysMenuSel = 0;
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
    case View::NetModemEdit:
      draw_modem_edit_screen();
      break;
    case View::NetRegionEdit:
      draw_region_edit_screen();
      break;
    case View::InfoScreen:
      snprintf(s_infoTitle, sizeof(s_infoTitle), "%s", locale::getForDisplay("scan_title"));
      display_nrf::show_fullscreen_text(s_infoTitle, s_infoBody);
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
}

void menu_nrf_open_menu() {
  if (!display_nrf::is_ready()) return;
  s_view = View::MenuHome;
  s_homeSel = 0;
  s_homeScroll = 0;
  s_netMenuSel = 0;
  s_sysMenuSel = 0;
  s_sysInDisplaySubmenu = false;
  draw_home_menu();
}

void menu_nrf_dashboard_next_page() {
  if (ui_nav_mode::isTabMode()) {
    if (display_tabs::contentForNavTab(s_navTab) == display_tabs::CT_MAIN) {
      s_dashPage = (uint8_t)((s_dashPage + 1) % 3);
    } else {
      advance_nav_tab();
    }
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

void menu_nrf_tab_idle_tick() {
  ui_tab_bar_idle::tick(s_tabDrillIn);
  static bool s_prevStrip = true;
  if (!ui_nav_mode::isTabMode()) {
    s_prevStrip = true;
    return;
  }
  const bool sh = ui_tab_bar_idle::tabStripVisible();
  if (sh != s_prevStrip && s_view == View::Dashboard) nrf_render_dashboard(s_dashPage);
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
        if (display_tabs::contentForNavTab(s_navTab) == display_tabs::CT_MAIN) {
          s_dashPage = (uint8_t)((s_dashPage + 1) % 3);
        } else {
          advance_nav_tab();
        }
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
    case View::NetModemEdit:
      cycle_modem_preset();
      draw_modem_edit_screen();
      break;
    case View::NetRegionEdit:
      cycle_region();
      draw_region_edit_screen();
      break;
    case View::InfoScreen:
      s_view = s_infoReturnView;
      if (s_view == View::NetMenu)
        draw_net_menu();
      else if (s_view == View::SysMenu)
        draw_sys_menu();
      else {
        s_view = View::Dashboard;
        nrf_render_dashboard(s_dashPage);
      }
      break;
    case View::SysAfterSelftest:
      s_view = View::SysMenu;
      draw_sys_menu();
      break;
    case View::Detail:
    case View::Power:
      if (ui_nav_mode::isTabMode()) {
        advance_nav_tab();
        s_view = View::Dashboard;
        nrf_render_dashboard(s_dashPage);
      } else {
        s_view = View::MenuHome;
        draw_home_menu();
      }
      break;
    default:
      break;
  }
}

static void on_long_press() {
  if (!display_nrf::is_ready()) return;
  switch (s_view) {
    case View::Dashboard:
      if (ui_nav_mode::isTabMode()) {
        const display_tabs::ContentTab ct = display_tabs::contentForNavTab(s_navTab);
        if (ct == display_tabs::CT_MAIN || ct == display_tabs::CT_MSG || ct == display_tabs::CT_INFO ||
            ct == display_tabs::CT_GPS) {
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
      if (s_netMenuSel == 3) {
        if (ui_nav_mode::isTabMode()) {
          s_tabDrillIn = false;
          s_view = View::Dashboard;
          nrf_render_dashboard(s_dashPage);
        } else {
          s_view = View::MenuHome;
          draw_home_menu();
        }
      } else if (s_netMenuSel == 0) {
        s_view = View::NetModemEdit;
        draw_modem_edit_screen();
      } else if (s_netMenuSel == 1) {
        s_view = View::NetRegionEdit;
        draw_region_edit_screen();
      } else {
        run_modem_scan_ui(View::NetMenu);
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
          snprintf(s_infoTitle, sizeof(s_infoTitle), "%s", locale::getForDisplay("psave"));
          snprintf(s_infoBody, sizeof(s_infoBody), "%s", locale::getForDisplay("power_nrf_note"));
          s_infoReturnView = View::SysMenu;
          s_view = View::InfoScreen;
          display_nrf::show_fullscreen_text(s_infoTitle, s_infoBody);
        } else if (e == 3) {
          s_view = View::NetRegionEdit;
          draw_region_edit_screen();
        } else if (e == 0) {
          s_view = View::NetModemEdit;
          draw_modem_edit_screen();
        } else if (e == 1) {
          run_modem_scan_ui(View::SysMenu);
        } else if (e == 4) {
          run_selftest_ui();
        }
      }
      break;
    case View::NetModemEdit:
      s_view = View::NetMenu;
      draw_net_menu();
      break;
    case View::NetRegionEdit:
      s_view = View::NetMenu;
      draw_net_menu();
      break;
    case View::InfoScreen:
      s_view = s_infoReturnView;
      if (s_view == View::NetMenu)
        draw_net_menu();
      else if (s_view == View::SysMenu)
        draw_sys_menu();
      else {
        s_view = View::Dashboard;
        nrf_render_dashboard(s_dashPage);
      }
      break;
    case View::SysAfterSelftest:
      s_view = View::Dashboard;
      nrf_render_dashboard(s_dashPage);
      break;
    case View::Detail:
    case View::Power:
      if (ui_nav_mode::isTabMode()) {
        const display_tabs::ContentTab ct = display_tabs::contentForNavTab(s_navTab);
        if (ct == display_tabs::CT_MAIN || ct == display_tabs::CT_MSG || ct == display_tabs::CT_INFO ||
            ct == display_tabs::CT_GPS) {
          break;
        }
        if (ct == display_tabs::CT_NET && !s_tabDrillIn && s_view == View::Detail) {
          s_tabDrillIn = true;
          s_netMenuSel = 0;
          s_view = View::NetMenu;
          draw_net_menu();
          break;
        }
        if (ct == display_tabs::CT_SYS && !s_tabDrillIn && s_view == View::Detail) {
          s_tabDrillIn = true;
          s_sysMenuSel = 0;
          s_sysInDisplaySubmenu = false;
          s_view = View::SysMenu;
          draw_sys_menu();
          break;
        }
        if (ct == display_tabs::CT_POWER && s_view == View::Power) {
          break;
        }
        s_tabDrillIn = false;
        s_view = View::Dashboard;
        nrf_render_dashboard(s_dashPage);
      } else {
        s_view = View::MenuHome;
        draw_home_menu();
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
      ui_tab_bar_idle::onInput();
      if (dur < kShortMs)
        on_short_press();
      else
        on_long_press();
    }
  }
  s_btnPrev = pressed;
}
#else
void menu_nrf_tab_idle_tick() {}
void menu_nrf_poll_t114_button(bool, uint32_t) {}
#endif
