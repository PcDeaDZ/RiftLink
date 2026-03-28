/**
 * Меню nRF: зеркало display_tabs (Heltec V3/V4) — главный список и экраны разделов.
 */

#include "menu_nrf.h"
#include "display_nrf.h"
#include "ui/display_tabs.h"
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

static uint32_t nrf_heap_kb_local() {
  size_t f = xPortGetFreeHeapSize();
#if RIFTLINK_HAS_NRF_SDH
  uint32_t sd = nrf_sdh_get_free_heap_size();
  if (sd > f) f = sd;
#endif
  return (uint32_t)(f / 1024U);
}

void nrf_render_dashboard(uint8_t page) {
  if (!display_nrf::is_ready()) return;
  if (page > 2) page = 0;
  char l1[32], l2[32], l3[40], l4[40];
  const uint8_t* nid = node::getId();
  char idshort[12];
  snprintf(idshort, sizeof(idshort), "%02X%02X%02X%02X", nid[0], nid[1], nid[2], nid[3]);
  if (page == 0) {
    snprintf(l1, sizeof(l1), "RiftLink %s", RIFTLINK_VERSION);
    snprintf(l2, sizeof(l2), "%s %s..", locale::getForDisplay("id"), idshort);
    snprintf(l3, sizeof(l3), "%s %.2f %s%d", region::getCode(), (double)region::getFreq(), locale::getForDisplay("dash_ch"),
        region::getChannel());
    snprintf(l4, sizeof(l4), "SF%u %s=%d %s", (unsigned)radio::getSpreadingFactor(), locale::getForDisplay("dash_n_prefix"),
        neighbors::getCount(), radio::modemPresetName(radio::getModemPreset()));
  } else if (page == 1) {
    snprintf(l1, sizeof(l1), "%s", locale::getForDisplay("dash_mesh"));
    snprintf(l2, sizeof(l2), "%s=%d", locale::getForDisplay("dash_n_prefix"), neighbors::getCount());
    snprintf(l3, sizeof(l3), "%s",
        ble::isConnected() ? locale::getForDisplay("dash_ble_connected") : locale::getForDisplay("dash_ble_adv"));
    if (neighbors::getCount() <= 0) {
      snprintf(l4, sizeof(l4), "%s --", locale::getForDisplay("dash_min_rssi"));
    } else {
      int mr = neighbors::getMinRssi();
      snprintf(l4, sizeof(l4), "%s %d", locale::getForDisplay("dash_min_rssi"), mr < 0 ? mr : -120);
    }
  } else {
    uint16_t mv = telemetry::readBatteryMv();
    snprintf(l1, sizeof(l1), "%s", locale::getForDisplay("dash_power_title"));
    snprintf(l2, sizeof(l2), "%s %umV", locale::getForDisplay("battery"), (unsigned)mv);
    snprintf(l3, sizeof(l3), "%s %u kB", locale::getForDisplay("dash_heap"), (unsigned)nrf_heap_kb_local());
    snprintf(l4, sizeof(l4), "%s %lus", locale::getForDisplay("dash_uptime"), (unsigned long)(millis() / 1000UL));
  }
  display_nrf::show_status_screen(l1, l2, l3, l4);
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
static int s_netMenuSel = 0;
static int s_sysMenuSel = 0;
/** Последний раздел Detail — для перерисовки после смены языка. */
static display_tabs::ContentTab s_detailTab = display_tabs::CT_MAIN;
/** Заголовок и текст для InfoScreen (скан эфира и т.п.). */
static char s_infoTitle[24];
static char s_infoBody[420];
static constexpr uint32_t kShortMs = 520;
static constexpr uint32_t kDebounceMs = 40;

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
static constexpr int kSysMenuItems = 3;

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

static void toggle_language() {
  (void)locale::setLang(locale::getLang() == 0 ? 1 : 0);
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
  display_nrf::show_menu_list(locale::getForDisplay("menu_home_lora"), ptrs, kNetMenuItems, s_netMenuSel, 0);
}

static void draw_sys_menu() {
  static char buf[3][28];
  static const char* ptrs[3];
  const char* keys[] = {"select_lang", "menu_selftest", "menu_back"};
  for (int i = 0; i < kSysMenuItems; i++) {
    strncpy(buf[i], locale::getForDisplay(keys[i]), sizeof(buf[i]) - 1);
    buf[i][sizeof(buf[i]) - 1] = 0;
    ptrs[i] = buf[i];
  }
  display_nrf::show_menu_list(locale::getForDisplay("menu_home_settings"), ptrs, kSysMenuItems, s_sysMenuSel, 0);
}

static void draw_modem_edit_screen() {
  char body[280];
  snprintf(body, sizeof(body), "%s\nSF%u BW%.0f CR%u\n\n%s", radio::modemPresetName(radio::getModemPreset()),
      (unsigned)radio::getSpreadingFactor(), (double)radio::getBandwidth(), (unsigned)radio::getCodingRate(),
      locale::getForDisplay("short_long_hint"));
  display_nrf::show_fullscreen_text(locale::getForDisplay("menu_modem"), body);
}

static void draw_region_edit_screen() {
  char body[200];
  snprintf(body, sizeof(body), "%s\n%.2f MHz\nch %d\n\n%s", region::getCode(), (double)region::getFreq(), region::getChannel(),
      locale::getForDisplay("short_long_hint"));
  display_nrf::show_fullscreen_text(locale::getForDisplay("select_country"), body);
}

static void run_modem_scan_ui() {
  selftest::ScanResult sr[8];
  int n = selftest::modemScanQuick(sr, 8);
  snprintf(s_infoTitle, sizeof(s_infoTitle), "%s", locale::getForDisplay("scan_title"));
  if (n <= 0) {
    strncpy(s_infoBody, locale::getForDisplay("scan_empty"), sizeof(s_infoBody) - 1);
    s_infoBody[sizeof(s_infoBody) - 1] = 0;
  } else {
    size_t off = 0;
    for (int i = 0; i < n && off < sizeof(s_infoBody) - 32; i++) {
      off += (size_t)snprintf(s_infoBody + off, sizeof(s_infoBody) - off, "SF%u bw%.0f %ddBm\n", (unsigned)sr[i].sf,
          (double)sr[i].bw, sr[i].rssi);
    }
  }
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
  display_nrf::show_menu_list(locale::getForDisplay("tab_home"), ptrs, use, s_homeSel, 0);
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
      snprintf(body, sizeof(body), "%s\n%s\n%s %.2f\nSF%u %s", idfull, nick[0] ? nick : "-", region::getCode(),
          (double)region::getFreq(), (unsigned)radio::getSpreadingFactor(), radio::modemPresetName(radio::getModemPreset()));
      break;
    }
    case display_tabs::CT_MSG: {
      title = locale::getForDisplay("menu_home_msg");
      char from[20], text[48];
      display_nrf::get_last_msg_peek(from, sizeof(from), text, sizeof(text));
      if (from[0] || text[0]) {
        snprintf(body, sizeof(body), "%s\n%s", from[0] ? from : "-", text[0] ? text : "-");
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
      for (int i = 0; i < nc && i < 8 && off < sizeof(body) - 20; i++) {
        char hx[20];
        neighbors::getIdHex(i, hx);
        int r = neighbors::getRssi(i);
        uint8_t pid[protocol::NODE_ID_LEN];
        bool hk = neighbors::getId(i, pid) && x25519_keys::hasKeyFor(pid);
        off += (size_t)snprintf(body + off, sizeof(body) - off, "%s %ddBm %s\n", hx, r, hk ? "K" : "-");
      }
      break;
    }
    case display_tabs::CT_GPS: {
      title = locale::getForDisplay("tab_gps");
      snprintf(body, sizeof(body), "%s\n%s\n%.5f %.5f", gps::isPresent() ? "HW" : locale::getForDisplay("gps_not_present"),
          gps::hasFix() ? locale::getForDisplay("gps_fix") : locale::getForDisplay("gps_no_fix"), (double)gps::getLat(),
          (double)gps::getLon());
      break;
    }
    default:
      title = "?";
      snprintf(body, sizeof(body), "-");
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
  s_netMenuSel = 0;
  s_sysMenuSel = 0;
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
      draw_detail(s_detailTab);
      break;
    case View::SysAfterSelftest:
      break;
    default:
      break;
  }
}

bool menu_nrf_owns_display() {
  return s_view != View::Dashboard;
}

void menu_nrf_open_menu() {
  if (!display_nrf::is_ready()) return;
  s_view = View::MenuHome;
  s_homeSel = 0;
  s_netMenuSel = 0;
  s_sysMenuSel = 0;
  draw_home_menu();
}

void menu_nrf_dashboard_next_page() {
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
  nrf_render_dashboard(page);
}

#if defined(RIFTLINK_BOARD_HELTEC_T114)
static void on_short_press() {
  if (!display_nrf::is_ready()) return;
  switch (s_view) {
    case View::Dashboard:
      s_dashPage = (uint8_t)((s_dashPage + 1) % 3);
      nrf_render_dashboard(s_dashPage);
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
    case View::SysMenu:
      s_sysMenuSel = (s_sysMenuSel + 1) % kSysMenuItems;
      draw_sys_menu();
      break;
    case View::NetModemEdit:
      cycle_modem_preset();
      draw_modem_edit_screen();
      break;
    case View::NetRegionEdit:
      cycle_region();
      draw_region_edit_screen();
      break;
    case View::InfoScreen:
      s_view = View::NetMenu;
      draw_net_menu();
      break;
    case View::SysAfterSelftest:
      s_view = View::SysMenu;
      draw_sys_menu();
      break;
    case View::Detail:
    case View::Power:
      s_view = View::MenuHome;
      draw_home_menu();
      break;
    default:
      break;
  }
}

static void on_long_press() {
  if (!display_nrf::is_ready()) return;
  switch (s_view) {
    case View::Dashboard:
      s_view = View::MenuHome;
      s_homeSel = 0;
      draw_home_menu();
      break;
    case View::MenuHome:
      open_detail_for_selection();
      break;
    case View::NetMenu:
      if (s_netMenuSel == 3) {
        s_view = View::MenuHome;
        draw_home_menu();
      } else if (s_netMenuSel == 0) {
        s_view = View::NetModemEdit;
        draw_modem_edit_screen();
      } else if (s_netMenuSel == 1) {
        s_view = View::NetRegionEdit;
        draw_region_edit_screen();
      } else {
        run_modem_scan_ui();
      }
      break;
    case View::SysMenu:
      if (s_sysMenuSel == 2) {
        s_view = View::MenuHome;
        draw_home_menu();
      } else if (s_sysMenuSel == 0) {
        toggle_language();
        draw_sys_menu();
      } else {
        run_selftest_ui();
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
      s_view = View::NetMenu;
      draw_net_menu();
      break;
    case View::SysAfterSelftest:
      s_view = View::Dashboard;
      nrf_render_dashboard(s_dashPage);
      break;
    case View::Detail:
    case View::Power:
      s_view = View::Dashboard;
      nrf_render_dashboard(s_dashPage);
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
      if (dur < kShortMs)
        on_short_press();
      else
        on_long_press();
    }
  }
  s_btnPrev = pressed;
}
#else
void menu_nrf_poll_t114_button(bool, uint32_t) {}
#endif
