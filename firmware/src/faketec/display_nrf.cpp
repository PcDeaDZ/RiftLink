/**
 * Дисплей nRF: Heltec T114 — встроенный ST7789 (SPI1); иначе SSD1306 I2C.
 */

#include "display_nrf.h"
#include "board_pins.h"
#include "ble/ble.h"
#include "nrf_wdt_feed.h"
#include "locale/locale.h"
#include "version.h"

#include <Arduino.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(RIFTLINK_BOARD_HELTEC_T114)
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "bootscreen_oled.h"
#include "cp1251_to_rusfont.h"
#include "ui_t114.h"
#include "ui/display_tabs.h"
#include "ui/ui_display_prefs.h"
#include "gps/gps.h"
#include "neighbors/neighbors.h"
#include "telemetry/telemetry.h"
#include "ui/region_modem_fmt.h"
#include "ui/ui_topbar_model.h"

namespace display_nrf {

namespace {

Adafruit_ST7789 g_tft(&SPI1, TFT_SPI_CS, TFT_SPI_DC, TFT_SPI_RST);

bool g_ok = false;
bool g_last_dirty = false;
uint32_t g_last_poll_ms = 0;
char g_line_from[20] = "";
char g_line_text[48] = "";

/** Режим отрисовки — чтобы не делать fillScreen при каждом обновлении дашборда (как дельта в Meshtastic TFT). */
enum class ScreenKind : uint8_t { None, Boot, Status4, MenuFull, FullscreenText, InitProgress, Selftest, LastMsgPoll };
static ScreenKind g_screen_kind = ScreenKind::None;

/** Кэш списка меню — дельта: две строки при смене выделения; полоса списка при смене скролла (без fillScreen на 240×135). */
static const char* const* s_menu_labels_arr = nullptr;
static const char* s_menu_footer_ptr = nullptr;
static char s_menu_title_snap[64] = "";
static int s_menu_count = -1;
static int s_menu_scroll = -1;
static int s_menu_sel = -1;
static const uint8_t* const* s_menu_icons_arr = nullptr;
static bool s_menu_chrome_strip = false;
static int s_menu_chrome_sel = -999;
static int s_menu_chrome_n = 0;
static int s_menu_cache_tab_top = -1;
static int s_menu_cache_title_h = -1;
static int s_menu_cache_row_h = -1;
static int s_menu_cache_max_rows = -1;
static int s_menu_cache_y_list = -1;
static int s_menu_cache_footer_h = -1;
static bool s_menu_cache_use_icons = false;
static bool s_menu_cache_sys_browse_six = false;
static uint8_t s_menu_cache_text_size = 0;
static int16_t s_menu_cache_label_start_x = 0;
static size_t s_menu_cache_label_clip = 0;

/** Кэш полноэкранного текста: без повторной чёрной заливки при том же title/body (смена языка/двойной вызов). */
static char s_fs_title_snap[64];
static char s_fs_body_snap[480];
static bool s_fs_snap_valid = false;
static bool s_fs_chrome_strip = false;
static int s_fs_chrome_sel = -999;
static int s_fs_chrome_n = 0;

/** Кэш 4 строк дашборда (kDashTextSize): без повторной заливки при том же содержимом (ложные клики / двойные вызовы). */
static char s_status4_lines[4][48];
static bool s_status4_valid = false;
/** Кэш таб-бара (полоска раньше вообще не рисовалась на ST7789). */
static bool s_status4_chrome_strip = false;
static int s_status4_chrome_sel = -999;
static int s_status4_chrome_n = 0;
/** Снимок топбара после последней полной отрисовки (иконка АКБ/зарядка не входит в 4 строки дашборда). */
static ui_topbar::Model s_status4_topbar_cache{};
static bool s_status4_topbar_cache_valid = false;

static bool status4_all_single_row(const char lines[4][48]) {
  for (int i = 0; i < 4; i++) {
    if (strlen(lines[i]) > ui_t114::kDashCharsPerLine) return false;
  }
  return true;
}

static int dash_content_top_px(const StatusScreenChrome* chrome) {
  if (!chrome) return 0;
  if (chrome->draw_tab_row && chrome->tab_count > 0) return (int)ui_t114::kDashTabBarH;
  /* Список / вкладки со скрытой полоской: топбар как на V3 (сигнал, регион, время, батарея). */
  return (int)ui_t114::kT114ChromeStatusTotalH;
}

static bool status4_chrome_matches(const StatusScreenChrome* chrome) {
  const bool strip = chrome && chrome->draw_tab_row;
  const int sel = chrome ? chrome->selected_tab : -1;
  const int n = chrome ? chrome->tab_count : 0;
  return strip == s_status4_chrome_strip && sel == s_status4_chrome_sel && n == s_status4_chrome_n;
}

static void tft_print_clipped(const char* s, size_t maxPrint);

static void draw_t114_signal_bars(int x, int y, int barsCount) {
  const int ts = (int)ui_t114::kDashTextSize;
  for (int i = 0; i < 4; i++) {
    const int h = (2 + i * 2) * ts;
    const int bx = x + i * (4 * ts);
    const int by = y + (8 * ts) - h;
    const int bw = 3 * ts;
    if (i < barsCount)
      g_tft.fillRect(bx, by, bw, h, ST77XX_WHITE);
    else
      g_tft.drawRect(bx, by, bw, h, ST77XX_WHITE);
  }
}

/**
 * Молния зарядки — та же геометрия, что на V3 OLED (`display.cpp`: drawBatteryChargingBoltOled):
 * зигзаг 14×7 в корпусе 19×9, два прохода zig(0)/zig(1) для «толщины»; здесь масштаб ts и ts сдвигов по X.
 */
static void draw_t114_charging_bolt(int x, int y, int ts) {
  constexpr int kBoltW = 14;
  constexpr int kBoltH = 7;
  const int kBatBodyW = 19 * ts;
  const int kBatBodyH = 9 * ts;
  const int ox = x + (kBatBodyW - kBoltW * ts) / 2 - ts;
  const int dy = (kBatBodyH - kBoltH * ts) / 2;
  const auto zig = [&](int dx) {
    g_tft.drawLine((int16_t)(ox + 8 * ts + dx), (int16_t)(y + 1 * ts + dy), (int16_t)(ox + 6 * ts + dx),
        (int16_t)(y + 3 * ts + dy), ST77XX_WHITE);
    g_tft.drawLine((int16_t)(ox + 6 * ts + dx), (int16_t)(y + 3 * ts + dy), (int16_t)(ox + 9 * ts + dx),
        (int16_t)(y + 3 * ts + dy), ST77XX_WHITE);
    g_tft.drawLine((int16_t)(ox + 9 * ts + dx), (int16_t)(y + 3 * ts + dy), (int16_t)(ox + 7 * ts + dx),
        (int16_t)(y + 5 * ts + dy), ST77XX_WHITE);
  };
  for (int dx = 0; dx < ts; dx++) zig(dx);
}

static void draw_t114_battery_icon(int x, int y, int pct, bool charging) {
  const int ts = (int)ui_t114::kDashTextSize;
  const int kW = 19 * ts;
  const int kH = 9 * ts;
  g_tft.drawRect(x, y, kW, kH, ST77XX_WHITE);
  g_tft.fillRect(x + kW, y + (kH - 3 * ts) / 2, 2 * ts, 3 * ts, ST77XX_WHITE);
  if (charging) {
    draw_t114_charging_bolt(x, y, ts);
    /* При зарядке только молния — процент внутри корпуса не помещается без «точки»; SOC на вкладке питания. */
    return;
  }
  /* Процент всегда size 1 — иначе при kDashTextSize=2 не влезает в корпус. */
  char b[8];
  if (pct >= 0)
    snprintf(b, sizeof(b), "%d%%", pct);
  else
    snprintf(b, sizeof(b), "--");
  g_tft.setTextSize(1);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  int16_t x1 = 0, y1 = 0;
  uint16_t tw = 0, th = 0;
  g_tft.getTextBounds(b, 0, 0, &x1, &y1, &tw, &th);
  int16_t tx = x + (kW - (int)tw) / 2 - x1;
  int16_t ty = y + (kH - (int)th) / 2 - y1;
  g_tft.setCursor(tx, ty);
  g_tft.print(b);
}

static int t114_battery_icon_bar_w() {
  const int ts = (int)ui_t114::kDashTextSize;
  return 19 * ts + 2 * ts;
}

static void fill_t114_topbar(ui_topbar::Model& m) {
  int n = neighbors::getCount();
  int avgRssi = neighbors::getAverageRssi();
  m.signalBars = ui_topbar::rssiToBars(n > 0 ? avgRssi : -120);
  m.linkIsBle = true;
  m.linkConnected = ble::isConnected();
  m.regionModem[0] = '\0';
  ui_fmt::regionModemShort(m.regionModem, sizeof(m.regionModem));
  m.hasTime = gps::hasTime();
  if (m.hasTime) {
    m.hour = (uint8_t)(gps::getLocalHour() & 0xff);
    m.minute = (uint8_t)(gps::getLocalMinute() & 0xff);
  } else {
    m.hour = 0;
    m.minute = 0;
  }
  m.batteryPercent = telemetry::batteryPercent();
  m.charging = telemetry::isCharging();
}

static bool t114_topbar_model_equal(const ui_topbar::Model& a, const ui_topbar::Model& b) {
  if (a.signalBars != b.signalBars) return false;
  if (a.linkIsBle != b.linkIsBle) return false;
  if (a.linkConnected != b.linkConnected) return false;
  if (strncmp(a.regionModem, b.regionModem, sizeof(a.regionModem)) != 0) return false;
  if (a.hasTime != b.hasTime) return false;
  if (a.hasTime && (a.hour != b.hour || a.minute != b.minute)) return false;
  if (a.batteryPercent != b.batteryPercent) return false;
  if (a.charging != b.charging) return false;
  return true;
}

/** Зоны статус-бара для частичной перерисовки (не вся полоса 240×28). */
enum T114SbZone : uint32_t {
  T114_SB_LEFT = 1u << 0,   // полосы RSSI
  T114_SB_MID = 1u << 1,    // регион + BLE
  T114_SB_TIME = 1u << 2,   // часы
  T114_SB_BAT = 1u << 3,    // иконка АКБ / молния
  T114_SB_ALL = T114_SB_LEFT | T114_SB_MID | T114_SB_TIME | T114_SB_BAT
};

struct T114SbLayout {
  int w;
  int hb;
  int ts;
  int cw;
  int glyphH;
  int y0;
  int ySig;
  int yText;
  int leftBlockEnd;
  int rightClusterLeft;
  char mid[40];
  size_t midMaxChars;
  int xMid;
  int midAvail;
  int batX;
  int batY;
  int xTime;
  int timeWchars;
};

static void t114_sb_compute_layout(const ui_topbar::Model& tb, T114SbLayout* L) {
  L->y0 = 0;
  L->w = (int)ui_t114::kScreenW;
  L->hb = (int)ui_t114::kDashTabBarH;
  L->ts = (int)ui_t114::kDashTextSize;
  L->cw = 6 * L->ts;
  L->glyphH = 8 * L->ts;
  const int barStackH = 8 * L->ts;
  L->ySig = L->y0 + (L->hb - barStackH) / 2;
  L->yText = L->y0 + (L->hb - L->glyphH) / 2;
  L->leftBlockEnd = 2 + 3 * (4 * L->ts) + 3 * L->ts + 6;
  snprintf(L->mid, sizeof(L->mid), "%s %s", tb.regionModem, tb.linkIsBle ? "BLE" : "WiFi");
  int rightClusterLeft = L->w - 2;
  if (tb.hasTime) {
    char tb_[8];
    snprintf(tb_, sizeof(tb_), "%02d:%02d", (unsigned)tb.hour, (unsigned)tb.minute);
    rightClusterLeft -= (int)strlen(tb_) * L->cw + 4;
  }
  rightClusterLeft -= t114_battery_icon_bar_w();
  rightClusterLeft += 1;
  L->rightClusterLeft = rightClusterLeft;
  L->midAvail = rightClusterLeft - L->leftBlockEnd - 4;
  L->midMaxChars = 1;
  if (L->midAvail >= L->cw) {
    L->midMaxChars = (size_t)(L->midAvail / L->cw);
    if (L->midMaxChars < 1U) L->midMaxChars = 1;
  }
  if (strlen(L->mid) > L->midMaxChars) L->mid[L->midMaxChars] = '\0';
  const int textW = (int)strlen(L->mid) * L->cw;
  L->xMid = L->leftBlockEnd + (L->midAvail - textW) / 2;
  int xRight = L->w - 2;
  xRight -= t114_battery_icon_bar_w();
  xRight += 1;
  L->batX = xRight;
  const int kH = 9 * L->ts;
  L->batY = L->y0 + (L->hb - kH) / 2;
  if (tb.hasTime) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", (unsigned)tb.hour, (unsigned)tb.minute);
    L->timeWchars = (int)strlen(buf);
    L->xTime = L->batX - 4 - L->timeWchars * L->cw;
  } else {
    L->xTime = -1;
    L->timeWchars = 0;
  }
}

static uint32_t t114_status_bar_dirty_mask(const ui_topbar::Model& prev, const ui_topbar::Model& cur) {
  uint32_t m = 0;
  if (prev.signalBars != cur.signalBars) m |= T114_SB_LEFT;
  if (strncmp(prev.regionModem, cur.regionModem, sizeof(prev.regionModem)) != 0 || prev.linkIsBle != cur.linkIsBle)
    m |= T114_SB_MID;
  if (prev.hasTime != cur.hasTime) m |= T114_SB_MID | T114_SB_TIME | T114_SB_BAT;
  else {
    if (cur.hasTime && (prev.hour != cur.hour || prev.minute != cur.minute)) m |= T114_SB_TIME;
  }
  if (prev.batteryPercent != cur.batteryPercent || prev.charging != cur.charging) m |= T114_SB_BAT;
  return m;
}

static void t114_sb_paint_left(const ui_topbar::Model& tb, const T114SbLayout& L) {
  draw_t114_signal_bars(2, L.ySig, tb.signalBars);
}

static void t114_sb_paint_mid(const ui_topbar::Model& tb, const T114SbLayout& L) {
  (void)tb;
  if (L.midAvail < L.cw) return;
  g_tft.setCursor(L.xMid, L.yText);
  tft_print_clipped(L.mid, L.midMaxChars);
}

static void t114_sb_paint_time(const ui_topbar::Model& tb, const T114SbLayout& L) {
  if (!tb.hasTime || L.xTime < 0) return;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", (unsigned)tb.hour, (unsigned)tb.minute);
  g_tft.setCursor(L.xTime, L.yText);
  tft_print_clipped(buf, 5);
}

static void t114_sb_paint_bat(const ui_topbar::Model& tb, const T114SbLayout& L) {
  draw_t114_battery_icon(L.batX, L.batY, tb.batteryPercent, tb.charging);
}

static void t114_sb_draw_separator_lines(int hb, int w) {
  g_tft.drawFastHLine(0, hb - 3, w, ST77XX_WHITE);
  g_tft.drawFastHLine(0, hb - 1, w, ST77XX_WHITE);
}

/** Контент полосы без заливки фона (после fillRect или зон). */
static void t114_status_bar_paint_content(const ui_topbar::Model& tb, const T114SbLayout& L) {
  g_tft.setTextSize((uint8_t)L.ts);
  g_tft.setTextWrap(false);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  t114_sb_paint_left(tb, L);
  t114_sb_paint_mid(tb, L);
  t114_sb_paint_time(tb, L);
  t114_sb_paint_bat(tb, L);
}

static void t114_status_bar_paint_zones(const ui_topbar::Model& tb, const T114SbLayout& L, uint32_t mask) {
  const int yFillMax = L.hb - 4;
  g_tft.setTextSize((uint8_t)L.ts);
  g_tft.setTextWrap(false);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  if (mask & T114_SB_LEFT) {
    g_tft.fillRect(0, 0, L.leftBlockEnd, yFillMax, ST77XX_BLACK);
    t114_sb_paint_left(tb, L);
  }
  if (mask & T114_SB_MID) {
    const int x0 = L.leftBlockEnd;
    const int rw = L.rightClusterLeft - x0;
    if (rw > 0) g_tft.fillRect(x0, 0, rw, yFillMax, ST77XX_BLACK);
    t114_sb_paint_mid(tb, L);
  }
  if (mask & T114_SB_TIME) {
    if (tb.hasTime && L.xTime >= 0) {
      const int tw = L.timeWchars * L.cw;
      g_tft.fillRect(L.xTime - 1, 0, tw + 2, yFillMax, ST77XX_BLACK);
      t114_sb_paint_time(tb, L);
    }
  }
  if (mask & T114_SB_BAT) {
    const int ts = L.ts;
    const int kW = 19 * ts;
    const int kH = 9 * ts;
    const int nibW = 2 * ts;
    g_tft.fillRect(L.batX, L.batY, kW + nibW, kH, ST77XX_BLACK);
    t114_sb_paint_bat(tb, L);
  }
  t114_sb_draw_separator_lines(L.hb, L.w);
}

static void draw_t114_status_bar() {
  ui_topbar::Model tb;
  fill_t114_topbar(tb);
  T114SbLayout L;
  t114_sb_compute_layout(tb, &L);
  g_tft.fillRect(0, 0, L.w, L.hb, ST77XX_BLACK);
  t114_status_bar_paint_content(tb, L);
  t114_sb_draw_separator_lines(L.hb, L.w);
}

static void draw_dash_tab_strip(int selected, int count);
/** Последняя отрисовка полоски вкладок — без повторной заливки при refresh_top_chrome_only (heap/RSSI в дайджесте). */
static int s_tab_strip_cache_sel = -999;
static int s_tab_strip_cache_n = -1;

static void draw_t114_top_chrome(const StatusScreenChrome* chrome) {
  if (!chrome) return;
  if (chrome->draw_tab_row && chrome->tab_count > 0) {
    draw_dash_tab_strip(chrome->selected_tab, chrome->tab_count);
    return;
  }
  draw_t114_status_bar();
}

/** Только полоса kDashTabBarH (статус-бар или вкладки), без заливки контента под ней. */
static void t114_redraw_top_chrome_only(const StatusScreenChrome* chrome) {
  if (!chrome) return;
  if (chrome->draw_tab_row && chrome->tab_count > 0) {
    int sel = chrome->selected_tab;
    int n = chrome->tab_count;
    if (n < 1) n = 1;
    if (sel < 0) sel = 0;
    if (sel >= n) sel = n - 1;
    if (sel == s_tab_strip_cache_sel && n == s_tab_strip_cache_n) {
      return;
    }
    g_tft.startWrite();
    draw_dash_tab_strip(chrome->selected_tab, chrome->tab_count);
    g_tft.endWrite();
    return;
  }
  g_tft.startWrite();
  ui_topbar::Model cur;
  fill_t114_topbar(cur);
  if (!s_status4_topbar_cache_valid) {
    draw_t114_status_bar();
    s_status4_topbar_cache = cur;
    s_status4_topbar_cache_valid = true;
    g_tft.endWrite();
    return;
  }
  const uint32_t mask = t114_status_bar_dirty_mask(s_status4_topbar_cache, cur);
  if (mask == 0) {
    g_tft.endWrite();
    return;
  }
  T114SbLayout L;
  t114_sb_compute_layout(cur, &L);
  if (mask == T114_SB_ALL) {
    g_tft.fillRect(0, 0, L.w, L.hb, ST77XX_BLACK);
    t114_status_bar_paint_content(cur, L);
    t114_sb_draw_separator_lines(L.hb, L.w);
  } else {
    t114_status_bar_paint_zones(cur, L, mask);
  }
  s_status4_topbar_cache = cur;
  s_status4_topbar_cache_valid = true;
  g_tft.endWrite();
}

static void status4_chrome_cache_store(const StatusScreenChrome* chrome) {
  s_status4_chrome_strip = chrome && chrome->draw_tab_row;
  s_status4_chrome_sel = chrome ? chrome->selected_tab : -1;
  s_status4_chrome_n = chrome ? chrome->tab_count : 0;
}

static bool fs_chrome_matches(const StatusScreenChrome* chrome) {
  const bool strip = chrome && chrome->draw_tab_row;
  const int sel = chrome ? chrome->selected_tab : -1;
  const int n = chrome ? chrome->tab_count : 0;
  return strip == s_fs_chrome_strip && sel == s_fs_chrome_sel && n == s_fs_chrome_n;
}

static void fs_chrome_store(const StatusScreenChrome* chrome) {
  s_fs_chrome_strip = chrome && chrome->draw_tab_row;
  s_fs_chrome_sel = chrome ? chrome->selected_tab : -1;
  s_fs_chrome_n = chrome ? chrome->tab_count : 0;
}

static bool menu_chrome_matches_menu(const StatusScreenChrome* chrome) {
  const bool strip = chrome && chrome->draw_tab_row;
  const int sel = chrome ? chrome->selected_tab : -1;
  const int n = chrome ? chrome->tab_count : 0;
  return strip == s_menu_chrome_strip && sel == s_menu_chrome_sel && n == s_menu_chrome_n;
}

static void menu_chrome_store_menu(const StatusScreenChrome* chrome) {
  s_menu_chrome_strip = chrome && chrome->draw_tab_row;
  s_menu_chrome_sel = chrome ? chrome->selected_tab : -1;
  s_menu_chrome_n = chrome ? chrome->tab_count : 0;
}

/** Одна строка списка меню (дельта при смене выделения / скролла без fillScreen). */
static void t114_menu_paint_row(int y, int idx, bool sel, const char* const* labels, const uint8_t* const* icons,
    bool useIcons, int16_t labelStartX, size_t labelClip, uint8_t menuListTextSize, int rowH) {
  g_tft.setTextSize(menuListTextSize);
  if (sel) {
    g_tft.fillRect(0, y, ui_t114::kScreenW, rowH, ST77XX_WHITE);
    g_tft.setTextColor(ST77XX_BLACK, ST77XX_WHITE);
  } else {
    g_tft.fillRect(0, y, ui_t114::kScreenW, rowH, ST77XX_BLACK);
    g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  }
  if (useIcons && icons && icons[idx]) {
    const int iy = (int)y + (rowH - (int)ui_t114::kHomeIconBitmapPx) / 2;
    g_tft.drawBitmap(ui_t114::kMenuIconLeftPx, iy, icons[idx], (int16_t)ui_t114::kHomeIconBitmapPx,
        (int16_t)ui_t114::kHomeIconBitmapPx, sel ? ST77XX_BLACK : ST77XX_WHITE);
  }
  g_tft.setCursor(labelStartX, y + 2);
  if (labels[idx]) tft_print_clipped(labels[idx], labelClip);
}

/** Baseline строки дашборда (слоты 0…3); при таб-баре контент центрируется в оставшейся высоте. */
static int16_t dash_line_y(int line_index, const StatusScreenChrome* chrome) {
  const int top = dash_content_top_px(chrome);
  const int contentH = (int)ui_t114::kScreenH - top;
  const int lineH = (int)ui_t114::kDashLinePx;
  const int blockH = 4 * lineH;
  int marginTop = (contentH - blockH) / 2;
  if (marginTop < 0) marginTop = 0;
  const int y0 = top + marginTop;
  return (int16_t)(y0 + line_index * lineH);
}

static void draw_dash_tab_strip(int selected, int count) {
  if (count < 1) count = 1;
  if (selected < 0) selected = 0;
  if (selected >= count) selected = count - 1;
  const int h = (int)ui_t114::kDashTabBarH;
  const int w = (int)ui_t114::kScreenW;
  g_tft.fillRect(0, 0, w, h, ST77XX_BLACK);
  const int cellW = w / count;
  for (int i = 0; i < count; i++) {
    const int x0 = i * cellW;
    const bool sel = (i == selected);
    g_tft.fillRect(x0, 0, cellW, h, sel ? ST77XX_WHITE : ST77XX_BLACK);
    const uint8_t* icon = display_tabs::iconForNavTab(i);
    const int ix = x0 + (cellW - 8) / 2;
    const int iy = (h - 8) / 2;
    g_tft.drawBitmap(ix, iy, icon, 8, 8, sel ? ST77XX_BLACK : ST77XX_WHITE);
  }
  /* Как display.cpp drawChromeTabsOrIdleRowOled: двойная линия под полоской (там y=11 и 13 под TAB_BAR_H 10). */
  g_tft.drawFastHLine(0, h - 3, w, ST77XX_WHITE);
  g_tft.drawFastHLine(0, h - 1, w, ST77XX_WHITE);
  s_tab_strip_cache_sel = selected;
  s_tab_strip_cache_n = count;
}

/** Кэш show_init_progress (T114): повторные вызовы с тем же totalSteps — только полоса шагов и строка статуса. */
static int s_init_cached_total_steps = -1;
static int s_init_cached_done = -1;
static char s_init_cached_status[96];

static void draw_init_progress_track(int n, int doneCount, int cy, int r, int pitch, int startX) {
  for (int i = 0; i < n - 1; i++) {
    const int cx0 = startX + i * pitch;
    const int cx1 = startX + (i + 1) * pitch;
    g_tft.drawLine(cx0 + r, cy, cx1 - r, cy, ST77XX_WHITE);
  }
  for (int i = 0; i < n; i++) {
    const int cx = startX + i * pitch;
    if (i < doneCount) {
      g_tft.fillCircle(cx, cy, r, ST77XX_WHITE);
    } else if (i == doneCount && doneCount < n) {
      g_tft.drawCircle(cx, cy, r, ST77XX_WHITE);
      const int innerDot = (r >= 4) ? 2 : 1;
      g_tft.fillCircle(cx, cy, innerDot, ST77XX_WHITE);
    } else {
      g_tft.drawCircle(cx, cy, r, ST77XX_WHITE);
    }
  }
}

/** CP1251 → индексы glcdfont (patches/glcdfont.c), как на ESP в display.cpp. */
static void tft_print_cp1251(const char* s) {
  if (!s) return;
  for (const char* p = s; *p; ++p) {
    const unsigned char c = cp1251_to_rusfont((unsigned char)*p);
    g_tft.print((char)c);
  }
}

static void tft_println_cp1251(const char* s) {
  tft_print_cp1251(s);
  g_tft.println();
}

static void tft_print_clipped(const char* s, size_t maxPrint) {
  if (!s) return;
  char conv[256];
  size_t j = 0;
  for (size_t i = 0; s[i] && j < sizeof(conv) - 1; i++) {
    conv[j++] = (char)cp1251_to_rusfont((unsigned char)s[i]);
  }
  conv[j] = 0;
  const size_t n = strlen(conv);
  if (n <= maxPrint) {
    g_tft.print(conv);
    return;
  }
  if (maxPrint <= 2) {
    g_tft.print("..");
    return;
  }
  const size_t keep = maxPrint - 2U;
  char buf[28];
  size_t k = keep;
  if (k > sizeof(buf) - 4U) k = sizeof(buf) - 4U;
  memcpy(buf, conv, k);
  buf[k] = '.';
  buf[k + 1] = '.';
  buf[k + 2] = 0;
  g_tft.print(buf);
}

/** Частичная заливка Status4→Status4: безопасно на всю высоту кадра (таб-бар + центрированный текст). */
static int16_t status4_estimated_clear_height(const char lines[4][48], const StatusScreenChrome* chrome) {
  (void)lines;
  (void)chrome;
  return (int16_t)ui_t114::kScreenH;
}

/** Частичная заливка только при подряд двух Status4; иначе полный кадр (смена меню/формы). */
static void clear_for_screen(ScreenKind next, int16_t status4_dash_strip_h = -1) {
  if (next != ScreenKind::FullscreenText) {
    s_fs_snap_valid = false;
    s_fs_chrome_strip = false;
    s_fs_chrome_sel = -999;
    s_fs_chrome_n = 0;
  }
  if (next != ScreenKind::MenuFull) {
    s_menu_labels_arr = nullptr;
    s_menu_icons_arr = nullptr;
  }
  if (next != ScreenKind::Status4) {
    s_status4_valid = false;
    s_status4_chrome_strip = false;
    s_status4_chrome_sel = -999;
    s_status4_chrome_n = 0;
    s_status4_topbar_cache_valid = false;
  }
  if (next != ScreenKind::InitProgress) {
    s_init_cached_total_steps = -1;
    s_init_cached_done = -1;
    s_init_cached_status[0] = 0;
  }
  const bool dash_refresh = (g_screen_kind == ScreenKind::Status4 && next == ScreenKind::Status4);
  if (dash_refresh) {
    int16_t h = ui_t114::kDashStatusStripDefaultH;
    if (status4_dash_strip_h > 0) {
      h = status4_dash_strip_h;
      if (h > ui_t114::kScreenH) h = ui_t114::kScreenH;
    }
    g_tft.fillRect(0, 0, ui_t114::kScreenW, h, ST77XX_BLACK);
  } else {
    g_tft.fillRect(0, 0, ui_t114::kScreenW, ui_t114::kScreenH, ST77XX_BLACK);
    s_tab_strip_cache_sel = -999;
    s_tab_strip_cache_n = -1;
  }
  g_screen_kind = next;
}

/** Заголовок полноэкранного режима: до двух строк, перенос по словам (длинные «Home > …»). */
static int16_t draw_fullscreen_title_wrapped(int16_t y0, const char* title) {
  if (!title || !title[0]) return y0;
  const int lineH = ui_t114::kFullscreenTitleLineStepPx;
  const size_t maxCol = ui_t114::kFullscreenCharsPerLine;
  int y = (int)y0;
  const char* seg = title;
  size_t rem = strlen(title);
  int lines_out = 0;
  while (rem > 0 && lines_out < ui_t114::kFullscreenTitleMaxLines) {
    while (rem > 0 && *seg == ' ') {
      seg++;
      rem--;
    }
    if (rem == 0) break;
    size_t take = rem;
    if (take > maxCol) {
      size_t br = maxCol;
      while (br > 0 && seg[br - 1] != ' ') br--;
      if (br == 0)
        take = maxCol;
      else
        take = br;
    }
    size_t n = take;
    while (n > 0 && seg[n - 1] == ' ') n--;
    char lineBuf[32];
    if (n > sizeof(lineBuf) - 1U) n = sizeof(lineBuf) - 1U;
    memcpy(lineBuf, seg, n);
    lineBuf[n] = 0;
    for (size_t k = 0; lineBuf[k]; k++) lineBuf[k] = (char)cp1251_to_rusfont((unsigned char)lineBuf[k]);
    g_tft.setCursor(0, y);
    g_tft.print(lineBuf);
    y += lineH;
    lines_out++;
    seg += take;
    rem -= take;
  }
  return (int16_t)y;
}

/** Тело полноэкранного режима: явные \n + перенос по словам под ширину T114. */
static void draw_fullscreen_body_wrapped(int16_t y0, int16_t yMax, const char* body) {
  if (!body || !body[0]) return;
  const int lineH = ui_t114::kFullscreenBodyLineStepPx;
  const size_t maxCol = ui_t114::kFullscreenCharsPerLine;
  int y = (int)y0;
  const char* p = body;
  while (*p && y < (int)yMax) {
    if (*p == '\n') {
      p++;
      y += lineH;
      continue;
    }
    const char* nl = strchr(p, '\n');
    const size_t segLen = nl ? (size_t)(nl - p) : strlen(p);
    const char* seg = p;
    size_t rem = segLen;
    while (rem > 0 && y < (int)yMax) {
      while (rem > 0 && *seg == ' ') {
        seg++;
        rem--;
      }
      if (rem == 0) break;
      size_t take = rem;
      if (take > maxCol) {
        size_t br = maxCol;
        while (br > 0 && seg[br - 1] != ' ') br--;
        if (br == 0)
          take = maxCol;
        else
          take = br;
      }
      size_t n = take;
      while (n > 0 && seg[n - 1] == ' ') n--;
      char lineBuf[32];
      if (n > sizeof(lineBuf) - 1U) n = sizeof(lineBuf) - 1U;
      memcpy(lineBuf, seg, n);
      lineBuf[n] = 0;
      for (size_t k = 0; lineBuf[k]; k++) lineBuf[k] = (char)cp1251_to_rusfont((unsigned char)lineBuf[k]);
      g_tft.setCursor(0, y);
      g_tft.print(lineBuf);
      y += lineH;
      seg += take;
      rem -= take;
    }
    p = nl ? nl + 1 : p + segLen;
  }
}

/** Пиксель монохромного битмапа Adafruit (MSB первый в байте). */
static bool bootscreen_oled_pixel(int16_t sx, int16_t sy) {
  const int16_t w = BOOTSCREEN_OLED_W;
  const int16_t h = BOOTSCREEN_OLED_H;
  const int16_t byteWidth = (w + 7) / 8;
  if (sx < 0 || sx >= w || sy < 0 || sy >= h) return false;
  const uint8_t b = bootscreen_oled[(uint16_t)sy * (uint16_t)byteWidth + (uint16_t)(sx / 8)];
  return (b & (uint8_t)(128 >> (sx & 7))) != 0;
}

/**
 * Логотип с масштабом не выше kBootLogoScale, подгонка под экран без обрезки:
 * totalW = round(128*s), totalH = round(64*s), s = min(kBootLogoScale, W/128, Hзоны/64).
 * Пиксели раскладываются по целочисленной сетке (sx*totalW/128 …) — без клипа.
 */
static void draw_boot_logo_fit_max() {
  const int maxW = (int)ui_t114::kScreenW;
  const int logoAreaH = (int)ui_t114::kScreenH - ui_t114::kBootFooterReservedPx;
  if (logoAreaH < 8 || maxW < 8) return;

  int cap = ui_t114::kBootLogoScale;
  if (cap < 1) cap = 1;
  /* s_f = min(cap, maxW/128, logoAreaH/64) — вещественный масштаб от исходного 128×64 */
  float sF = (float)cap;
  {
    const float sw = (float)maxW / 128.0f;
    const float sh = (float)logoAreaH / 64.0f;
    if (sw < sF) sF = sw;
    if (sh < sF) sF = sh;
  }
  const int totalW = (int)(128.0f * sF + 0.5f);
  const int totalH = (int)(64.0f * sF + 0.5f);
  if (totalW < 1 || totalH < 1) return;

  int bx = (maxW - totalW) / 2;
  if (bx < 0) bx = 0;
  int by = (logoAreaH > totalH) ? ((logoAreaH - totalH) / 2) : 0;
  if (by < (int)ui_t114::kBootLogoMinTopPx &&
      (int)ui_t114::kBootLogoMinTopPx + totalH <= logoAreaH) {
    by = (int)ui_t114::kBootLogoMinTopPx;
  }
  if (by + totalH > logoAreaH) by = logoAreaH - totalH;
  if (by < 0) by = 0;

  for (int sy = 0; sy < (int)BOOTSCREEN_OLED_H; sy++) {
    const int y0 = by + (sy * totalH) / (int)BOOTSCREEN_OLED_H;
    const int y1 = by + ((sy + 1) * totalH) / (int)BOOTSCREEN_OLED_H;
    if (y1 <= y0) continue;
    for (int sx = 0; sx < (int)BOOTSCREEN_OLED_W; sx++) {
      if (!bootscreen_oled_pixel((int16_t)sx, (int16_t)sy)) continue;
      const int x0 = bx + (sx * totalW) / (int)BOOTSCREEN_OLED_W;
      const int x1 = bx + ((sx + 1) * totalW) / (int)BOOTSCREEN_OLED_W;
      if (x1 <= x0) continue;
      g_tft.fillRect((int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0), ST77XX_WHITE);
    }
  }
}

}  // namespace

void apply_rotation_from_prefs() {
  const uint8_t r = ui_display_prefs::getFlip180() ? 3u : ui_t114::kGfxRotation;
  g_tft.setRotation(r);
  /* После setRotation буфер/дельты невалидны — один полный кадр как при смене экрана (до init g_ok ещё false). */
  if (g_ok) clear_for_screen(ScreenKind::None);
}

bool init() {
  g_ok = false;
  // Heltec T114: VTFT_CTRL включает питание матрицы (Meshtastic: LOW = on). Иначе подсветка/шина могут «моргать», картинки нет.
  pinMode(TFT_VTFT_CTRL, OUTPUT);
  digitalWrite(TFT_VTFT_CTRL, TFT_VTFT_PWR_ON);
  // Чуть дольше, чем 20 ms: стабилизация питания матрицы до SPI (Meshtastic поднимает VTFT в DISPLAYON после init драйвера).
  delay(50);
  pinMode(TFT_BL, OUTPUT);
  // Подсветка после init (как в туториалах Adafruit): не показывать мусор на шине до первого чёрного кадра.
  digitalWrite(TFT_BL, TFT_BL_ON == LOW ? HIGH : LOW);
  delay(5);
  SPI1.setPins(TFT_SPI_MISO, TFT_SPI_SCK, TFT_SPI_MOSI);
  SPI1.begin();
  // 1.14" ST7789: init(135,240) — ветка панели в Adafruit; логический кадр — kScreenW×kScreenH после setRotation.
  // SPI_MODE0 — как LovyanGFX cfg.spi_mode = 0 в Meshtastic TFTDisplay (ST7789).
  g_tft.init(ui_t114::kPanelW, ui_t114::kPanelH, SPI_MODE0);
  apply_rotation_from_prefs();
  g_tft.setSPISpeed(ui_t114::kSpiWriteHz);
  // Белый на чёрном — перерисовка символов без «просветов»; ниже кадры батчим startWrite/endWrite (меньше артефактов на ST7789).
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  /** Нейтральный размер до первого экрана (дашборд/вкладки — kDashTextSize). */
  g_tft.setTextSize(ui_t114::kDashTextSize);
  g_tft.setTextWrap(false);
  /** Без cp437(true) Adafruit_GFX сдвигает коды >=176 — ломает кириллицу в rusfont (как OLED в display.cpp). */
  g_tft.cp437(true);
  g_tft.startWrite();
  g_tft.fillRect(0, 0, ui_t114::kScreenW, ui_t114::kScreenH, ST77XX_BLACK);
  g_tft.endWrite();
  digitalWrite(TFT_BL, TFT_BL_ON);
  g_tft.setCursor(0, 0);
  g_screen_kind = ScreenKind::None;
  g_ok = true;
  ui_t114::init();
  return true;
}

bool is_ready() {
  return g_ok;
}

void show_boot(const char* line1, const char* line2) {
  if (!g_ok) return;
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(2);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::Boot);
  g_tft.setCursor(0, 0);
  if (line1) tft_println_cp1251(line1);
  if (line2) tft_println_cp1251(line2);
  g_tft.endWrite();
}

void show_boot_screen() {
  if (!g_ok) return;
  g_tft.startWrite();
  clear_for_screen(ScreenKind::Boot);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  draw_boot_logo_fit_max();
  g_tft.setTextSize(ui_t114::kBootVersionTextSize);
  char ver[20];
  snprintf(ver, sizeof(ver), "v%s", RIFTLINK_VERSION);
  int16_t x1, y1;
  uint16_t tw, th;
  g_tft.getTextBounds(ver, 0, 0, &x1, &y1, &tw, &th);
  /* Курсор — baseline: нижний край bbox = verY + y1 + h (см. Adafruit_GFX). */
  const int16_t verY =
      (int16_t)((int)ui_t114::kScreenH - (int)ui_t114::kBootVersionBottomPx - (int)y1 - (int)th);
  const int16_t verX = (int16_t)ui_t114::kBootVersionLeftPx;
  g_tft.setCursor(verX, verY);
  g_tft.print(ver);
  g_tft.endWrite();
  g_screen_kind = ScreenKind::Boot;
}

void show_init_progress(int doneCount, int totalSteps, const char* statusLine) {
  if (!g_ok) return;
  if (totalSteps < 1) totalSteps = 1;
  if (doneCount < 0) doneCount = 0;
  if (doneCount > totalSteps) doneCount = totalSteps;

  const int cy = ui_t114::kInitProgressTrackY;
  const int r = ui_t114::kInitProgressTrackR;
  const int n = totalSteps;
  int pitch = n > 1 ? (ui_t114::kScreenW - ui_t114::kInitProgressTrackMarginX) / (n - 1) : 0;
  if (pitch < ui_t114::kInitProgressPitchMin) pitch = ui_t114::kInitProgressPitchMin;
  if (pitch > ui_t114::kInitProgressPitchMax) pitch = ui_t114::kInitProgressPitchMax;
  const int startX = (int)ui_t114::kScreenW / 2 - ((n - 1) * pitch) / 2;

  const char* st = statusLine ? statusLine : "";
  const bool can_delta = (g_screen_kind == ScreenKind::InitProgress && s_init_cached_total_steps == n);
  const bool done_changed = (doneCount != s_init_cached_done);
  char status_prev[sizeof(s_init_cached_status)];
  strncpy(status_prev, s_init_cached_status, sizeof(status_prev));
  status_prev[sizeof(status_prev) - 1] = 0;
  const bool status_changed = (strcmp(status_prev, st) != 0);

  if (can_delta && !done_changed && !status_changed) {
    return;
  }

  if (can_delta && (done_changed || status_changed)) {
    g_tft.startWrite();
    g_tft.setTextSize(ui_t114::kInitProgressTextSize);
    g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    if (done_changed) {
      const int bandH = 2 * r + 12;
      const int bandY = cy - r - 6;
      g_tft.fillRect(0, bandY, ui_t114::kScreenW, bandH, ST77XX_BLACK);
      draw_init_progress_track(n, doneCount, cy, r, pitch, startX);
      s_init_cached_done = doneCount;
    }
    if (status_changed) {
      g_tft.fillRect(0, ui_t114::kInitProgressStatusY, ui_t114::kScreenW,
                     (int16_t)ui_t114::kInitProgressStatusBandH, ST77XX_BLACK);
      if (st[0]) {
        g_tft.setCursor(0, ui_t114::kInitProgressStatusY);
        tft_print_clipped(st, ui_t114::kInitProgressCharsPerLine);
      }
      strncpy(s_init_cached_status, st, sizeof(s_init_cached_status) - 1);
      s_init_cached_status[sizeof(s_init_cached_status) - 1] = 0;
    }
    g_tft.endWrite();
    return;
  }

  g_tft.startWrite();
  clear_for_screen(ScreenKind::InitProgress);
  g_tft.setTextSize(ui_t114::kInitProgressTextSize);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setCursor(0, ui_t114::kInitProgressTitleY);
  tft_print_cp1251(locale::getForDisplay("init_title"));
  draw_init_progress_track(n, doneCount, cy, r, pitch, startX);
  if (st[0]) {
    g_tft.setCursor(0, ui_t114::kInitProgressStatusY);
    tft_print_clipped(st, ui_t114::kInitProgressCharsPerLine);
  }
  g_tft.setCursor(0, ui_t114::kInitProgressHintBaselineY);
  tft_print_clipped(locale::getForDisplay("init_hint"), ui_t114::kInitProgressCharsPerLine);
  g_tft.endWrite();
  s_init_cached_total_steps = n;
  s_init_cached_done = doneCount;
  strncpy(s_init_cached_status, st, sizeof(s_init_cached_status) - 1);
  s_init_cached_status[sizeof(s_init_cached_status) - 1] = 0;
}

void show_warning_blocking(const char* line1, const char* line2, uint32_t durationMs) {
  if (!g_ok) return;
  char body[220];
  snprintf(body, sizeof(body), "%s\n%s", line1 ? line1 : "", line2 ? line2 : "");
  show_fullscreen_text(locale::getForDisplay("warn_title"), body);
  const uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < durationMs) {
    ble::update();
    riftlink_wdt_feed();
    delay(50);
  }
}

void show_selftest_summary(bool radioOk, bool antennaOk, uint16_t batteryMv, uint32_t heapFree) {
  if (!g_ok) return;
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(ui_t114::kDashTextSize);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::Selftest);
  g_tft.setCursor(0, 0);
  tft_println_cp1251(locale::getForDisplay("menu_selftest"));
  tft_println_cp1251(radioOk ? locale::getForDisplay("radio_ok") : locale::getForDisplay("radio_fail"));
  tft_println_cp1251(antennaOk ? locale::getForDisplay("selftest_ant_ok") : locale::getForDisplay("selftest_ant_warn"));
  tft_print_cp1251(locale::getForDisplay("battery"));
  g_tft.printf(" %umV\n", (unsigned)batteryMv);
  tft_print_cp1251(locale::getForDisplay("selftest_heap"));
  g_tft.printf(" %u\n", (unsigned)heapFree);
  g_tft.endWrite();
}

void queue_last_msg(const char* fromHex, const char* text) {
  if (!g_ok) return;
  char nextFrom[20] = "";
  char nextText[48] = "";
  if (fromHex) {
    strncpy(nextFrom, fromHex, sizeof(nextFrom) - 1);
    nextFrom[sizeof(nextFrom) - 1] = 0;
  }
  if (text) {
    strncpy(nextText, text, sizeof(nextText) - 1);
    nextText[sizeof(nextText) - 1] = 0;
  }
  if (strcmp(nextFrom, g_line_from) == 0 && strcmp(nextText, g_line_text) == 0) return;
  memcpy(g_line_from, nextFrom, sizeof(g_line_from));
  memcpy(g_line_text, nextText, sizeof(g_line_text));
  g_last_dirty = true;
}

void show_status_screen(const char* line1, const char* line2, const char* line3, const char* line4,
    const StatusScreenChrome* chrome) {
  if (!g_ok) return;
  const char* src[4] = {line1, line2, line3, line4};
  char next4[4][48];
  for (int i = 0; i < 4; i++) {
    if (src[i]) {
      strncpy(next4[i], src[i], sizeof(next4[i]) - 1);
      next4[i][sizeof(next4[i]) - 1] = 0;
    } else {
      next4[i][0] = 0;
    }
  }
  bool s4_same = false;
  bool s4_top_dirty = false;
  if (g_screen_kind == ScreenKind::Status4 && s_status4_valid) {
    s4_same = true;
    for (int i = 0; i < 4; i++) {
      if (strcmp(next4[i], s_status4_lines[i]) != 0) {
        s4_same = false;
        break;
      }
    }
    /* 4 строки и chrome совпали, но топбар (батарея/молния/время/BLE) живёт отдельно — иначе ранний выход без перерисовки. */
    if (chrome && !chrome->draw_tab_row) {
      ui_topbar::Model cur{};
      fill_t114_topbar(cur);
      s4_top_dirty = !s_status4_topbar_cache_valid || !t114_topbar_model_equal(cur, s_status4_topbar_cache);
    }
    if (s4_same && status4_chrome_matches(chrome) && !s4_top_dirty) return;
  }

  /* Только полоса статуса: строки дашборда те же, изменилась телеметрия в топбаре — без clear и без 4 строк. */
  if (g_screen_kind == ScreenKind::Status4 && s_status4_valid && s4_same && status4_chrome_matches(chrome) &&
      s4_top_dirty && chrome && !chrome->draw_tab_row) {
    t114_redraw_top_chrome_only(chrome);
    return;
  }

  /* При топбаре со временем дельта по строкам не обновляет часы — полный кадр. */
  const bool skipStatus4Delta = chrome && !chrome->draw_tab_row;
  // Дельта строк: только если таб-хром совпал (смена вкладки = полный кадр с новой полоской).
  if (!skipStatus4Delta && g_screen_kind == ScreenKind::Status4 && s_status4_valid && status4_chrome_matches(chrome) &&
      status4_all_single_row(next4) && status4_all_single_row(s_status4_lines)) {
    g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    g_tft.setTextSize(ui_t114::kDashTextSize);
    g_tft.setTextWrap(false);
    g_tft.startWrite();
    for (int i = 0; i < 4; i++) {
      if (strcmp(next4[i], s_status4_lines[i]) == 0) continue;
      const int16_t y = dash_line_y(i, chrome);
      g_tft.fillRect(0, y, ui_t114::kScreenW, ui_t114::kDashLinePx, ST77XX_BLACK);
      g_tft.setCursor((int16_t)ui_t114::kDashLeftMargin, y);
      tft_print_clipped(next4[i], ui_t114::kDashCharsPerLine);
    }
    g_tft.endWrite();
    memcpy(s_status4_lines, next4, sizeof(next4));
    s_status4_valid = true;
    status4_chrome_cache_store(chrome);
    g_screen_kind = ScreenKind::Status4;
    return;
  }

  int16_t dash_strip_h = -1;
  if (g_screen_kind == ScreenKind::Status4) {
    dash_strip_h = status4_estimated_clear_height(next4, chrome);
  }

  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(ui_t114::kDashTextSize);
  g_tft.setTextWrap(false);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::Status4, dash_strip_h);
  draw_t114_top_chrome(chrome);
  if (!chrome || !chrome->draw_tab_row) {
    fill_t114_topbar(s_status4_topbar_cache);
    s_status4_topbar_cache_valid = true;
  }
  for (int i = 0; i < 4; i++) {
    g_tft.setCursor((int16_t)ui_t114::kDashLeftMargin, dash_line_y(i, chrome));
    tft_print_clipped(next4[i], ui_t114::kDashCharsPerLine);
  }
  g_tft.endWrite();
  memcpy(s_status4_lines, next4, sizeof(next4));
  s_status4_valid = true;
  status4_chrome_cache_store(chrome);
}

/** Не вызывается из main loop: полный fillScreen здесь перезаписывал дашборд и давал моргание на T114 ST7789. */
void poll() {
  if (!g_ok || !g_last_dirty) return;
  uint32_t now = millis();
  // Реже полный кадр — при активном mesh меньше вспышек чёрного.
  if ((uint32_t)(now - g_last_poll_ms) < 750) return;
  g_last_poll_ms = now;

  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(ui_t114::kDashTextSize);
  g_tft.setTextWrap(false);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::LastMsgPoll);
  int row = 0;
  g_tft.setCursor((int16_t)ui_t114::kDashLeftMargin, dash_line_y(row++, nullptr));
  tft_print_clipped(locale::getForDisplay("last_msg_title"), ui_t114::kDashCharsPerLine);
  if (g_line_from[0]) {
    g_tft.setCursor((int16_t)ui_t114::kDashLeftMargin, dash_line_y(row++, nullptr));
    tft_print_clipped(g_line_from, ui_t114::kDashCharsPerLine);
  }
  if (g_line_text[0]) {
    g_tft.setCursor((int16_t)ui_t114::kDashLeftMargin, dash_line_y(row, nullptr));
    tft_print_clipped(g_line_text, ui_t114::kDashCharsPerLine);
  }
  g_tft.endWrite();
  g_last_dirty = false;
}

void show_net_drill(const char* screenTitle, const char* modeLine, const char* advLine, const char* pinLine, int selectedRow,
    const StatusScreenChrome* chrome) {
  if (!g_ok) return;
  if (selectedRow < 0) selectedRow = 0;
  if (selectedRow > 1) selectedRow = 1;
  const char* backStr = locale::getForDisplay("menu_back");
  const int tabTop = dash_content_top_px(chrome);
  const bool tabStripUi = chrome && chrome->draw_tab_row && chrome->tab_count > 0;
  const bool showTitle = (screenTitle && screenTitle[0]);
  const int titleH = showTitle ? ui_t114::kMenuTitleBarPx : 0;
  const int rowH = ui_t114::kMenuRowPx;

  g_tft.setTextWrap(false);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::MenuFull);
  draw_t114_top_chrome(chrome);
  g_tft.setTextSize(ui_t114::kDashTextSize);
  if (showTitle) {
    g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    g_tft.setCursor(ui_t114::kMarginX, (int16_t)(tabTop + ui_t114::kMarginY));
    tft_print_clipped(screenTitle, ui_t114::kMenuTitlePrintChars);
    g_tft.drawFastHLine(0, tabTop + titleH - 1, ui_t114::kScreenW, ST77XX_WHITE);
  }
  int y = tabTop + titleH + (tabStripUi ? (int)ui_t114::kMarginY : 0);

  auto draw_sel_row = [&](const char* txt, bool sel) {
    if (sel) {
      g_tft.fillRect(0, y, ui_t114::kScreenW, rowH, ST77XX_WHITE);
      g_tft.setTextColor(ST77XX_BLACK, ST77XX_WHITE);
    } else {
      g_tft.fillRect(0, y, ui_t114::kScreenW, rowH, ST77XX_BLACK);
      g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    }
    g_tft.setCursor(2, y + 2);
    if (txt) tft_print_clipped(txt, ui_t114::kMenuLabelPrintChars);
    y += rowH;
  };
  auto draw_plain_row = [&](const char* txt) {
    g_tft.fillRect(0, y, ui_t114::kScreenW, rowH, ST77XX_BLACK);
    g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    g_tft.setCursor(2, y + 2);
    if (txt) tft_print_clipped(txt, ui_t114::kMenuLabelPrintChars);
    y += rowH;
  };

  draw_sel_row(modeLine, selectedRow == 0);
  draw_plain_row(advLine);
  draw_plain_row(pinLine);
  draw_sel_row(backStr, selectedRow == 1);

  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.endWrite();

  s_menu_labels_arr = nullptr;
  s_menu_icons_arr = nullptr;
  s_menu_footer_ptr = nullptr;
  s_menu_count = -1;
  s_menu_scroll = 0;
  s_menu_sel = selectedRow;
  s_menu_title_snap[0] = 0;
  g_screen_kind = ScreenKind::MenuFull;
}

void show_menu_list(const char* title, const char* const* labels, int count, int selected, int scroll,
    const char* footerHint, const uint8_t* const* icons, const StatusScreenChrome* chrome) {
  if (!g_ok || !labels || count < 1) return;
  const bool useIcons = (icons != nullptr);
  const int16_t labelStartX = useIcons ? (int16_t)ui_t114::kMenuTextStartXWithIcon : 2;
  const bool noHighlight = (selected < 0);
  if (!noHighlight) {
    if (selected >= count) selected = count - 1;
  }
  if (scroll < 0) scroll = 0;
  const int tabTop = dash_content_top_px(chrome);
  const bool tabStripUi = chrome && chrome->draw_tab_row && chrome->tab_count > 0;
  const int footerH = (footerHint && footerHint[0]) ? ui_t114::kMenuFooterHintH : 0;
  const bool hasTitle = (title && title[0]);
  /* Пустой title: без полосы под заголовком (как drawContentPower на ESP — только строки меню). */
  const int titleH = tabStripUi ? 0 : (hasTitle ? ui_t114::kMenuTitleBarPx : 0);
  /** Как sysTabBrowse на OLED: 6 пунктов подряд, size 1 — иначе не влезают под полоску вкладок. */
  const bool sysBrowseSix = noHighlight && useIcons && count == 6;
  const uint8_t menuListTextSize = sysBrowseSix ? (uint8_t)1 : (uint8_t)ui_t114::kDashTextSize;
  const int rowH = sysBrowseSix ? ui_t114::kMenuSysBrowseRowPx : (int)ui_t114::kMenuRowPx;
  size_t labelClip =
      useIcons ? ui_t114::kMenuLabelPrintCharsWithIcon : ui_t114::kMenuLabelPrintChars;
  if (sysBrowseSix) {
    labelClip = (size_t)((ui_t114::kScreenW - (int)labelStartX) / (6 * (int)menuListTextSize));
  }
  int maxRows = ((int)ui_t114::kScreenH - tabTop - titleH - footerH) / rowH;
  if (maxRows < 1) maxRows = 1;

  int scrollAdj = scroll;
  if (!noHighlight) {
    if (scrollAdj > selected) scrollAdj = selected;
    if (scrollAdj < selected - maxRows + 1) scrollAdj = selected - maxRows + 1;
  }
  if (scrollAdj < 0) scrollAdj = 0;
  const int maxScroll = count - maxRows;
  if (maxScroll > 0 && scrollAdj > maxScroll) scrollAdj = maxScroll;

  const int yListStart = tabTop + titleH + ((tabStripUi || !hasTitle) ? (int)ui_t114::kMarginY : 0);

  char titleCmp[64];
  if (tabStripUi) {
    titleCmp[0] = 0;
  } else if (hasTitle && title) {
    strncpy(titleCmp, title, sizeof(titleCmp) - 1);
    titleCmp[sizeof(titleCmp) - 1] = 0;
  } else {
    titleCmp[0] = 0;
  }

  const bool menuSame = g_screen_kind == ScreenKind::MenuFull && labels == s_menu_labels_arr && count == s_menu_count &&
      footerHint == s_menu_footer_ptr && icons == s_menu_icons_arr && menu_chrome_matches_menu(chrome) &&
      tabTop == s_menu_cache_tab_top && titleH == s_menu_cache_title_h && rowH == s_menu_cache_row_h &&
      maxRows == s_menu_cache_max_rows && yListStart == s_menu_cache_y_list && footerH == s_menu_cache_footer_h &&
      useIcons == s_menu_cache_use_icons && sysBrowseSix == s_menu_cache_sys_browse_six &&
      menuListTextSize == s_menu_cache_text_size && labelStartX == s_menu_cache_label_start_x &&
      labelClip == s_menu_cache_label_clip;
  const bool titleSnapOk = (strcmp(titleCmp, s_menu_title_snap) == 0);

  if (menuSame && titleSnapOk) {
    if (scrollAdj == s_menu_scroll && (noHighlight || selected == s_menu_sel)) {
      return;
    }
    /* Только смена выделения, тот же viewport — две строки без fillScreen. */
    if (!noHighlight && scrollAdj == s_menu_scroll && selected != s_menu_sel && selected >= 0 && s_menu_sel >= 0) {
      const int oldVis = s_menu_sel - scrollAdj;
      const int newVis = selected - scrollAdj;
      if (oldVis >= 0 && oldVis < maxRows && newVis >= 0 && newVis < maxRows) {
        g_tft.setTextWrap(false);
        g_tft.startWrite();
        const int y0 = yListStart + oldVis * rowH;
        const int y1 = yListStart + newVis * rowH;
        t114_menu_paint_row(y0, s_menu_sel, false, labels, icons, useIcons, labelStartX, labelClip, menuListTextSize, rowH);
        t114_menu_paint_row(y1, selected, true, labels, icons, useIcons, labelStartX, labelClip, menuListTextSize, rowH);
        g_tft.setTextSize(ui_t114::kDashTextSize);
        g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        g_tft.endWrite();
        s_menu_sel = selected;
        return;
      }
    }
    /* Смена скролла (или выделение сдвинуло окно) — только полоса списка + футер при необходимости. */
    if (scrollAdj != s_menu_scroll) {
      g_tft.setTextWrap(false);
      g_tft.startWrite();
      const int clearY = yListStart;
      const int clearH =
          (footerH > 0) ? (ui_t114::kMenuFooterHintY - clearY) : ((int)ui_t114::kScreenH - clearY);
      g_tft.fillRect(0, clearY, ui_t114::kScreenW, clearH, ST77XX_BLACK);
      g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      int y = yListStart;
      for (int row = 0; row < maxRows; row++) {
        const int idx = scrollAdj + row;
        if (idx >= count) break;
        const bool sel = !noHighlight && (idx == selected);
        t114_menu_paint_row(y, idx, sel, labels, icons, useIcons, labelStartX, labelClip, menuListTextSize, rowH);
        y += rowH;
      }
      if (footerH > 0 && footerHint) {
        g_tft.setTextSize(ui_t114::kDashTextSize);
        g_tft.fillRect(0, ui_t114::kMenuFooterHintY, ui_t114::kScreenW, footerH, ST77XX_BLACK);
        g_tft.setCursor(0, ui_t114::kMenuFooterHintY);
        tft_print_cp1251(footerHint);
      }
      g_tft.setTextSize(ui_t114::kDashTextSize);
      g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      g_tft.endWrite();
      s_menu_scroll = scrollAdj;
      s_menu_sel = noHighlight ? -1 : selected;
      return;
    }
  }

  g_tft.setTextWrap(false);
  g_tft.startWrite();
  clear_for_screen(ScreenKind::MenuFull);
  draw_t114_top_chrome(chrome);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  if (!tabStripUi && hasTitle) {
    g_tft.setTextSize(ui_t114::kDashTextSize);
    g_tft.setCursor(ui_t114::kMarginX, (int16_t)(tabTop + ui_t114::kMarginY));
    tft_print_clipped(title, ui_t114::kMenuTitlePrintChars);
    g_tft.drawFastHLine(0, tabTop + titleH - 1, ui_t114::kScreenW, ST77XX_WHITE);
  }

  int y = yListStart;
  for (int row = 0; row < maxRows; row++) {
    const int idx = scrollAdj + row;
    if (idx >= count) break;
    const bool sel = !noHighlight && (idx == selected);
    t114_menu_paint_row(y, idx, sel, labels, icons, useIcons, labelStartX, labelClip, menuListTextSize, rowH);
    y += rowH;
  }

  if (footerH > 0 && footerHint) {
    g_tft.setTextSize(ui_t114::kDashTextSize);
    g_tft.fillRect(0, ui_t114::kMenuFooterHintY, ui_t114::kScreenW, footerH, ST77XX_BLACK);
    g_tft.setCursor(0, ui_t114::kMenuFooterHintY);
    tft_print_cp1251(footerHint);
  }
  g_tft.setTextSize(ui_t114::kDashTextSize);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.endWrite();

  if (tabStripUi) {
    s_menu_title_snap[0] = 0;
  } else if (hasTitle) {
    strncpy(s_menu_title_snap, title, sizeof(s_menu_title_snap) - 1);
    s_menu_title_snap[sizeof(s_menu_title_snap) - 1] = 0;
  } else {
    s_menu_title_snap[0] = 0;
  }
  s_menu_labels_arr = labels;
  s_menu_icons_arr = icons;
  s_menu_footer_ptr = footerHint;
  s_menu_count = count;
  s_menu_scroll = scrollAdj;
  s_menu_sel = noHighlight ? -1 : selected;
  menu_chrome_store_menu(chrome);
  s_menu_cache_tab_top = tabTop;
  s_menu_cache_title_h = titleH;
  s_menu_cache_row_h = rowH;
  s_menu_cache_max_rows = maxRows;
  s_menu_cache_y_list = yListStart;
  s_menu_cache_footer_h = footerH;
  s_menu_cache_use_icons = useIcons;
  s_menu_cache_sys_browse_six = sysBrowseSix;
  s_menu_cache_text_size = menuListTextSize;
  s_menu_cache_label_start_x = labelStartX;
  s_menu_cache_label_clip = labelClip;
  g_screen_kind = ScreenKind::MenuFull;
}

void show_home_menu_strip(const char* title, const char* const* labels, const uint8_t* const* icons, int count, int selected,
    int scroll, const char* footerHint, const StatusScreenChrome* chrome) {
  show_menu_list(title, labels, count, selected, scroll, footerHint, icons, chrome);
}

void show_fullscreen_text(const char* title, const char* body, const StatusScreenChrome* chrome) {
  if (!g_ok) return;
  char tbuf[64];
  char bbuf[480];
  if (title) {
    strncpy(tbuf, title, sizeof(tbuf) - 1);
    tbuf[sizeof(tbuf) - 1] = 0;
  } else {
    tbuf[0] = 0;
  }
  if (body) {
    strncpy(bbuf, body, sizeof(bbuf) - 1);
    bbuf[sizeof(bbuf) - 1] = 0;
  } else {
    bbuf[0] = 0;
  }
  const bool tabStripUi = chrome && chrome->draw_tab_row && chrome->tab_count > 0;
  char titleSnap[64];
  if (tabStripUi) {
    titleSnap[0] = 0;
  } else {
    strncpy(titleSnap, tbuf, sizeof(titleSnap) - 1);
    titleSnap[sizeof(titleSnap) - 1] = 0;
  }

  if (s_fs_snap_valid && strcmp(titleSnap, s_fs_title_snap) == 0 && strcmp(bbuf, s_fs_body_snap) == 0 &&
      fs_chrome_matches(chrome)) {
    /* Как Status4: текст тот же, но топбар (молния/BLE/время) живёт своей жизнью — иначе залипание иконки. */
    bool topbar_dirty = false;
    if (chrome && !chrome->draw_tab_row) {
      ui_topbar::Model cur{};
      fill_t114_topbar(cur);
      topbar_dirty = !s_status4_topbar_cache_valid || !t114_topbar_model_equal(cur, s_status4_topbar_cache);
    }
    if (!topbar_dirty) return;
    if (chrome && !chrome->draw_tab_row) {
      t114_redraw_top_chrome_only(chrome);
      return;
    }
  }

  const int tabTop = dash_content_top_px(chrome);
  /* Уже FullscreenText с той же полоской вкладок (число вкладок то же): смена вкладки/тела — без чёрной вспышки на весь TFT. */
  const bool fsTabStripPartial =
      g_screen_kind == ScreenKind::FullscreenText && tabStripUi && chrome && s_fs_snap_valid && s_fs_chrome_strip &&
      (int)chrome->tab_count == s_fs_chrome_n;
  if (fsTabStripPartial) {
    g_tft.startWrite();
    draw_t114_top_chrome(chrome);
    g_tft.fillRect(0, tabTop, (int)ui_t114::kScreenW, (int)ui_t114::kScreenH - tabTop, ST77XX_BLACK);
    g_tft.setTextSize(ui_t114::kDashTextSize);
    g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    const int16_t body_y_max = (int16_t)(ui_t114::kScreenH - ui_t114::kFullscreenBottomMarginPx);
    const int16_t body_y0 = (int16_t)(tabTop + ui_t114::kMarginY);
    if (bbuf[0]) draw_fullscreen_body_wrapped(body_y0, body_y_max, bbuf);
    strncpy(s_fs_title_snap, titleSnap, sizeof(s_fs_title_snap) - 1);
    s_fs_title_snap[sizeof(s_fs_title_snap) - 1] = 0;
    strncpy(s_fs_body_snap, bbuf, sizeof(s_fs_body_snap) - 1);
    s_fs_body_snap[sizeof(s_fs_body_snap) - 1] = 0;
    s_fs_snap_valid = true;
    fs_chrome_store(chrome);
    g_tft.endWrite();
    return;
  }

  g_tft.startWrite();
  clear_for_screen(ScreenKind::FullscreenText);
  draw_t114_top_chrome(chrome);
  if (!chrome || !chrome->draw_tab_row) {
    fill_t114_topbar(s_status4_topbar_cache);
    s_status4_topbar_cache_valid = true;
  }
  g_tft.setTextSize(ui_t114::kDashTextSize);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);

  int16_t body_y0;
  int16_t body_y_max = (int16_t)(ui_t114::kScreenH - ui_t114::kFullscreenBottomMarginPx);
  if (tabStripUi) {
    /* Полоска вкладок уже задаёт раздел — без заголовка «Сеть/Сообщения…» и без второй линии. */
    body_y0 = (int16_t)(tabTop + ui_t114::kMarginY);
  } else if (tbuf[0]) {
    const int16_t y_after = draw_fullscreen_title_wrapped((int16_t)(tabTop + ui_t114::kMarginY), tbuf);
    const int16_t y_sep = (int16_t)(y_after + 2);
    g_tft.drawFastHLine(0, y_sep, ui_t114::kScreenW, ST77XX_WHITE);
    body_y0 = (int16_t)(y_sep + 4);
  } else {
    /* Без заголовка линию не рисуем — иначе под топбаром «висячая» полоска без текста над ней. */
    body_y0 = (int16_t)(tabTop + ui_t114::kMarginY);
  }
  if (bbuf[0]) draw_fullscreen_body_wrapped(body_y0, body_y_max, bbuf);

  strncpy(s_fs_title_snap, titleSnap, sizeof(s_fs_title_snap) - 1);
  s_fs_title_snap[sizeof(s_fs_title_snap) - 1] = 0;
  strncpy(s_fs_body_snap, bbuf, sizeof(s_fs_body_snap) - 1);
  s_fs_body_snap[sizeof(s_fs_body_snap) - 1] = 0;
  s_fs_snap_valid = true;
  fs_chrome_store(chrome);
  g_tft.endWrite();
}

void get_last_msg_peek(char* fromBuf, size_t fromLen, char* textBuf, size_t textLen) {
  if (fromBuf && fromLen) {
    strncpy(fromBuf, g_line_from, fromLen - 1);
    fromBuf[fromLen - 1] = 0;
  }
  if (textBuf && textLen) {
    strncpy(textBuf, g_line_text, textLen - 1);
    textBuf[textLen - 1] = 0;
  }
}

int menu_list_last_scroll() {
  return s_menu_scroll >= 0 ? s_menu_scroll : 0;
}

void refresh_top_chrome_only(const StatusScreenChrome* chrome) {
  t114_redraw_top_chrome_only(chrome);
}

}  // namespace display_nrf

#else

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

namespace display_nrf {

namespace {

constexpr uint8_t kScreenW = 128;
constexpr uint8_t kScreenH = 64;
constexpr int8_t kOledRst = -1;

Adafruit_SSD1306 g_disp(kScreenW, kScreenH, &Wire, kOledRst);

bool g_ok = false;
bool g_last_dirty = false;
uint32_t g_last_poll_ms = 0;
char g_line_from[20] = "";
char g_line_text[48] = "";

}  // namespace

bool init() {
  g_ok = false;
  Wire.setPins((uint8_t)PIN_I2C_SDA, (uint8_t)PIN_I2C_SCL);
  Wire.begin();
  Wire.setClock(100000);
  delay(50);
  if (!g_disp.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    Wire.end();
    return false;
  }
  Wire.setClock(400000);
  g_disp.clearDisplay();
  g_disp.setTextSize(1);
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.setCursor(0, 0);
  g_disp.display();
  g_ok = true;
  return true;
}

bool is_ready() {
  return g_ok;
}

void show_boot(const char* line1, const char* line2) {
  if (!g_ok) return;
  g_disp.clearDisplay();
  g_disp.setTextSize(1);
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.setCursor(0, 0);
  if (line1) g_disp.println(line1);
  if (line2) g_disp.println(line2);
  g_disp.display();
}

void show_init_progress(int doneCount, int totalSteps, const char* statusLine) {
  if (!g_ok) return;
  if (totalSteps < 1) totalSteps = 1;
  if (doneCount < 0) doneCount = 0;
  if (doneCount > totalSteps) doneCount = totalSteps;
  g_disp.clearDisplay();
  g_disp.setTextSize(1);
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.setCursor(0, 0);
  g_disp.print(locale::getForDisplay("init_title"));
  const int cy = 22;
  const int r = 2;
  const int n = totalSteps;
  int pitch = n > 1 ? (kScreenW - 16) / (n - 1) : 0;
  if (pitch < 8) pitch = 8;
  if (pitch > 18) pitch = 18;
  const int startX = (int)kScreenW / 2 - ((n - 1) * pitch) / 2;
  for (int i = 0; i < n - 1; i++) {
    const int cx0 = startX + i * pitch;
    const int cx1 = startX + (i + 1) * pitch;
    g_disp.drawLine(cx0 + r, cy, cx1 - r, cy, SSD1306_WHITE);
  }
  for (int i = 0; i < n; i++) {
    const int cx = startX + i * pitch;
    if (i < doneCount) {
      g_disp.fillCircle(cx, cy, r, SSD1306_WHITE);
    } else if (i == doneCount && doneCount < totalSteps) {
      g_disp.drawCircle(cx, cy, r, SSD1306_WHITE);
      g_disp.fillCircle(cx, cy, 1, SSD1306_WHITE);
    } else {
      g_disp.drawCircle(cx, cy, r, SSD1306_WHITE);
    }
  }
  if (statusLine && statusLine[0]) {
    g_disp.setCursor(0, 36);
    g_disp.print(statusLine);
  }
  g_disp.setCursor(0, 52);
  g_disp.print(locale::getForDisplay("init_hint"));
  g_disp.display();
}

void show_warning_blocking(const char* line1, const char* line2, uint32_t durationMs) {
  if (!g_ok) return;
  char body[220];
  snprintf(body, sizeof(body), "%s\n%s", line1 ? line1 : "", line2 ? line2 : "");
  show_fullscreen_text(locale::getForDisplay("warn_title"), body);
  const uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < durationMs) {
    ble::update();
    riftlink_wdt_feed();
    delay(50);
  }
}

void show_selftest_summary(bool radioOk, bool antennaOk, uint16_t batteryMv, uint32_t heapFree) {
  if (!g_ok) return;
  g_disp.clearDisplay();
  g_disp.setCursor(0, 0);
  g_disp.println(locale::getForDisplay("menu_selftest"));
  g_disp.printf("%s\n", radioOk ? locale::getForDisplay("radio_ok") : locale::getForDisplay("radio_fail"));
  g_disp.printf("%s\n", antennaOk ? locale::getForDisplay("selftest_ant_ok") : locale::getForDisplay("selftest_ant_warn"));
  g_disp.printf("%s %umV\n", locale::getForDisplay("battery"), (unsigned)batteryMv);
  g_disp.printf("%s %u\n", locale::getForDisplay("selftest_heap"), (unsigned)heapFree);
  g_disp.display();
}

void show_status_screen(const char* line1, const char* line2, const char* line3, const char* line4,
    const StatusScreenChrome* chrome) {
  (void)chrome;
  if (!g_ok) return;
  g_disp.clearDisplay();
  g_disp.setCursor(0, 0);
  g_disp.setTextSize(1);
  g_disp.setTextColor(SSD1306_WHITE);
  if (line1) g_disp.println(line1);
  if (line2) g_disp.println(line2);
  if (line3) g_disp.println(line3);
  if (line4) g_disp.println(line4);
  g_disp.display();
}

void queue_last_msg(const char* fromHex, const char* text) {
  if (!g_ok) return;
  g_line_from[0] = 0;
  g_line_text[0] = 0;
  if (fromHex) {
    strncpy(g_line_from, fromHex, sizeof(g_line_from) - 1);
    g_line_from[sizeof(g_line_from) - 1] = 0;
  }
  if (text) {
    strncpy(g_line_text, text, sizeof(g_line_text) - 1);
    g_line_text[sizeof(g_line_text) - 1] = 0;
  }
  g_last_dirty = true;
}

void poll() {
  if (!g_ok || !g_last_dirty) return;
  uint32_t now = millis();
  if ((uint32_t)(now - g_last_poll_ms) < 400) return;
  g_last_poll_ms = now;

  g_disp.clearDisplay();
  g_disp.setCursor(0, 0);
  g_disp.println(locale::getForDisplay("last_msg_title"));
  if (g_line_from[0]) {
    g_disp.println(g_line_from);
  }
  if (g_line_text[0]) {
    g_disp.println(g_line_text);
  }
  g_disp.display();
  g_last_dirty = false;
}

static constexpr int kMenuRowOled = 9;

void show_net_drill(const char* screenTitle, const char* modeLine, const char* advLine, const char* pinLine, int selectedRow,
    const StatusScreenChrome* chrome) {
  (void)chrome;
  if (!g_ok) return;
  if (selectedRow < 0) selectedRow = 0;
  if (selectedRow > 1) selectedRow = 1;
  const char* backStr = locale::getForDisplay("menu_back");
  const bool showTitle = (screenTitle && screenTitle[0]);
  const int titleY = showTitle ? 9 : 0;
  const int rowH = kMenuRowOled;

  g_disp.clearDisplay();
  g_disp.setTextSize(1);
  g_disp.setCursor(0, 0);
  if (showTitle) {
    g_disp.setTextColor(SSD1306_WHITE);
    g_disp.println(screenTitle);
  }
  int y = titleY;
  auto draw_sel = [&](const char* txt, bool sel) {
    if (sel) g_disp.fillRect(0, y, kScreenW, rowH, SSD1306_WHITE);
    else g_disp.fillRect(0, y, kScreenW, rowH, SSD1306_BLACK);
    g_disp.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
    g_disp.setCursor(1, y + 1);
    if (txt) g_disp.print(txt);
    y += rowH;
  };
  auto draw_plain = [&](const char* txt) {
    g_disp.fillRect(0, y, kScreenW, rowH, SSD1306_BLACK);
    g_disp.setTextColor(SSD1306_WHITE);
    g_disp.setCursor(1, y + 1);
    if (txt) g_disp.print(txt);
    y += rowH;
  };
  draw_sel(modeLine, selectedRow == 0);
  draw_plain(advLine);
  draw_plain(pinLine);
  draw_sel(backStr, selectedRow == 1);
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.display();
}

void show_menu_list(const char* title, const char* const* labels, int count, int selected, int scroll, const char* footerHint,
    const uint8_t* const* icons, const StatusScreenChrome* chrome) {
  (void)chrome;
  (void)icons;
  if (!g_ok || !labels || count < 1) return;
  const bool noHighlight = (selected < 0);
  if (!noHighlight) {
    if (selected >= count) selected = count - 1;
  }
  if (scroll < 0) scroll = 0;
  const int footerH = (footerHint && footerHint[0]) ? 8 : 0;
  const int maxRows = (kScreenH - 10 - footerH) / kMenuRowOled;
  if (!noHighlight) {
    if (scroll > selected) scroll = selected;
    if (scroll < selected - maxRows + 1) scroll = selected - maxRows + 1;
  }
  if (scroll < 0) scroll = 0;

  g_disp.clearDisplay();
  g_disp.setTextSize(1);
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.setCursor(0, 0);
  if (title && title[0]) g_disp.println(title);
  int y = (title && title[0]) ? 10 : 0;
  for (int row = 0; row < maxRows; row++) {
    int idx = scroll + row;
    if (idx >= count) break;
    const bool sel = !noHighlight && (idx == selected);
    if (sel) g_disp.fillRect(0, y, kScreenW, kMenuRowOled, SSD1306_WHITE);
    g_disp.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
    g_disp.setCursor(1, y + 1);
    if (labels[idx]) g_disp.print(labels[idx]);
    y += kMenuRowOled;
  }
  if (footerH > 0 && footerHint) {
    g_disp.setTextColor(SSD1306_WHITE);
    g_disp.setCursor(0, (int16_t)(kScreenH - 8));
    g_disp.print(footerHint);
  }
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.display();
}

void show_home_menu_strip(const char* title, const char* const* labels, const uint8_t* const* icons, int count,
    int selected, int scroll, const char* footerHint, const StatusScreenChrome* chrome) {
  show_menu_list(title, labels, count, selected, scroll, footerHint, icons, chrome);
}

void show_fullscreen_text(const char* title, const char* body, const StatusScreenChrome* chrome) {
  (void)chrome;
  if (!g_ok) return;
  g_disp.clearDisplay();
  g_disp.setTextSize(1);
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.setCursor(0, 0);
  if (title) g_disp.println(title);
  int y = 10;
  if (body && body[0]) {
    const char* p = body;
    char line[22];
    while (*p && y < 56) {
      size_t n = 0;
      while (*p && *p != '\n' && n < sizeof(line) - 1) line[n++] = *p++;
      line[n] = 0;
      if (*p == '\n') p++;
      g_disp.setCursor(0, y);
      g_disp.print(line);
      y += 8;
    }
  }
  g_disp.display();
}

void get_last_msg_peek(char* fromBuf, size_t fromLen, char* textBuf, size_t textLen) {
  if (fromBuf && fromLen) {
    strncpy(fromBuf, g_line_from, fromLen - 1);
    fromBuf[fromLen - 1] = 0;
  }
  if (textBuf && textLen) {
    strncpy(textBuf, g_line_text, textLen - 1);
    textBuf[textLen - 1] = 0;
  }
}

int menu_list_last_scroll() {
  return 0;
}

void refresh_top_chrome_only(const StatusScreenChrome* chrome) {
  (void)chrome;
}

void apply_rotation_from_prefs() {}

}  // namespace display_nrf

#endif
