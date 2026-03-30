/**
 * RiftLink Display — LilyGO T-Lora Pager, ST7796 SPI (480×222), LovyanGFX
 * Общая SPI с SX1262 — перед отрисовкой standby LoRa + mutex (как E-Ink).
 */

#include "display.h"
#include "display_tabs.h"
#include "region_modem_fmt.h"
#include "ui_section_titles.h"
#include "ui_scroll.h"
#include "ui_menu_exec.h"
#include "ui_icons.h"
#include "ui_msg_scroll.h"
#include "ui_layout_profile.h"
#include "ui_topbar_fill.h"
#include "ui_typography.h"
#include "ui_content_scroll.h"
#include "ui_nav_mode.h"
#include "ui_display_prefs.h"
#include "ui_tab_bar_idle.h"
#include "locale/locale.h"
#include "selftest/selftest.h"
#include "node/node.h"
#include "gps/gps.h"
#include "region/region.h"
#include "neighbors/neighbors.h"
#include "wifi/wifi.h"
#include "telemetry/telemetry.h"
#include "radio/radio.h"
#include "radio_mode/radio_mode.h"
#include "powersave/powersave.h"
#include "ble/ble.h"
#include "version.h"
#include <LovyanGFX.hpp>
#include "utf8rus.h"
#include "cp1251_to_rusfont.h"
#include <cstring>
#include <cstdio>
#include <atomic>

#define BUTTON_PIN 7
/** Энкодер (wiki): A=40, B=41 — навигация по главному меню */
#define ENC_A 40
#define ENC_B 41
#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 222
#define SPI_SCK 35
#define SPI_MISO 33
#define SPI_MOSI 34
#define TFT_CS 38
#define TFT_DC 37
#define TFT_BL 42

#define CONTENT_Y 34
#define CONTENT_H (SCREEN_HEIGHT - CONTENT_Y - 6)
#define ICON_W 8
#define ICON_H 8
static constexpr auto kProfTpager = ui_layout::profileTpager480x222();
/** Шаг строки списка меню (popup, контент подменю с тем же шагом) — из ui_layout_profile */
static constexpr int LINE_H = kProfTpager.menuListRowHeight;
static constexpr int MENU_LIST_TEXT_OFF_TP = kProfTpager.menuListTextOffsetY;
static constexpr int MENU_LIST_ICON_OFF_TP = kProfTpager.menuListIconTopOffsetY;
/** Главное меню: шаг строк чуть больше высоты текста, чтобы пункты не слипались */
#define HOME_MENU_ROW_STEP_TPAGER_7 (LINE_H + 2)
#define HOME_MENU_ROW_STEP_TPAGER_6 (LINE_H + 4)
#define HOME_MENU_ROW_STEP_TPAGER_5 (LINE_H + 6)
#define MAX_LINE_CHARS 52
#define CONTENT_X 8
/** Высота блока статус-бара до двойной линии (см. drawSubScreenChrome). */
#define TPAGER_STATUS_H 30
/** Нижняя линия разделителя под статусом = TPAGER_STATUS_H + 2. */
#define TPAGER_STATUS_SEP_BOTTOM (TPAGER_STATUS_H + 2)
/**
 * Зазор от нижней линии разделителя до базовой линии ника (px): меньше — пустота;
 * слишком мало — снова касание разделителя (cp1251 выше «номинальных» 8px).
 */
#define NODE_MAIN_MIN_BASELINE_BELOW_SEP 16
#define NODE_MAIN_MIN_BASELINE_Y (TPAGER_STATUS_SEP_BOTTOM + NODE_MAIN_MIN_BASELINE_BELOW_SEP)
/** Доп. отступ к submenuListY0 (итог не ниже NODE_MAIN_MIN_BASELINE_Y). */
#define NODE_MAIN_LIST_TOP_PAD_PX 2
/** Отступ над строкой «Назад» на «Узел» (px). */
#define NODE_BACK_ROW_GAP_PX 8
/** Полоса выбора в списках (главное меню = popup): как в displayShowPopupMenu */
#define MENU_SEL_X 2
#define MENU_SEL_W (SCREEN_WIDTH - 4)
#define POPUP_MODE_CANCEL 0
#define POPUP_MODE_PICKER 1
#define SUBMENU_TITLE_H_TPAGER 20

static int s_layoutTabShiftY = 0;
static bool s_tabDrillIn = false;
static int s_powerMenuIndex = 0;
static int s_gpsMenuIndex = 0;

static int displayShowPopupMenu(const char* title, const char* items[], int count, int initialSel = 0, int mode = POPUP_MODE_CANCEL,
    const uint8_t* const* rowIcons = nullptr, bool lastRowNoIcon = false, bool noRowIcons = false);
static void drawSubScreenChrome();
static void drawTabBarTpager();
static int submenuListY0Tpager(bool hasTitle);
static void drawSubmenuTitleBarTpager(const char* title);
static void drawSubmenuFooterSepTpager();
static void prepareTabLayoutShiftTpager();
static void drawChromeTabsOrIdleRowTpager();

static constexpr uint32_t COL_BG = 0x000000u;
static constexpr uint32_t COL_FG = 0xFFFFu;
static constexpr uint32_t COL_DIM = 0x8410u;

#define SHORT_PRESS_MS  350
#define LONG_PRESS_MS   500
#define MIN_PRESS_MS    50
#define DISPLAY_SLEEP_MS 30000

/* submenuListY0 в профиле; в режиме вкладок полоска иконок в зоне топбара, s_layoutTabShiftY = 0 */
static_assert(ui_layout::profileTpager480x222().submenuListY0 == (CONTENT_Y + SUBMENU_TITLE_H_TPAGER + 6), "ui_layout vs T-Pager");
static_assert(kProfTpager.menuListRowHeight == 16, "ui_layout menuListRowHeight T-Pager");

class LGFX_TPager : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel;
  lgfx::Bus_SPI _bus;
public:
  LGFX_TPager() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = SPI_SCK;
      cfg.pin_mosi = SPI_MOSI;
      cfg.pin_miso = SPI_MISO;
      cfg.pin_dc = TFT_DC;
      _bus.config(cfg);
    }
    _panel.setBus(&_bus);
    {
      auto cfg = _panel.config();
      cfg.pin_cs = TFT_CS;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.memory_width = 480;
      cfg.memory_height = 320;
      cfg.panel_width = 480;
      cfg.panel_height = 222;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.readable = true;
      cfg.invert = false;
#if defined(TPAGER_ST7796_RGB_ORDER) && TPAGER_ST7796_RGB_ORDER
      cfg.rgb_order = true;   // IPS: при неверных цветах попробуйте без этого флага в platformio.ini
#else
      cfg.rgb_order = false;
#endif
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

static LGFX_TPager gfx;
static bool s_gfxReady = false;

static int s_currentScreen = 0;
static int s_homeMenuIndex = 0;
static int s_homeMenuScrollOff = 0;
/** Вкладка «Режим»: 0 = переключить BLE/WiFi, 1 = Назад (главное меню). */
static int s_netMenuIndex = 0;
/** Вкладка «Настройки»: 0 «Дисплей», 1 PS, 2 регион, 3 модем, 4 скан, 5 тест, 6 «Назад». */
static int s_sysMenuIndex = 0;
static int s_sysScrollOff = 0;
static bool s_sysInDisplaySubmenu = false;
static size_t s_msgScrollStart = 0;
static uint32_t s_lastScreenUpdate = 0;
static bool s_needRedrawInfo = false;
static bool s_needRedrawMsg = false;
static bool s_lastButton = false;
static uint32_t s_pressStart = 0;
static char s_lastMsgFrom[17] = {0};
static char s_lastMsgText[64] = {0};
static bool s_displaySleeping = false;
static bool s_wakeRequested = false;
static uint32_t s_lastActivityTime = 0;
static bool s_buttonPolledExternally = false;
static volatile bool s_menuActive = false;
static bool s_showingBootScreen = false;
static int s_tpagerMainScrollPx = 0;
static int s_tpagerMainMaxScrollPx = 0;
static std::atomic<int32_t> s_encAccum{0};
static volatile uint8_t s_encLastState = 0;
static const int8_t kEncQuadTable[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

static inline display_tabs::ContentTab contentTabAtIndex(int screen) {
  return ui_nav_mode::isTabMode() ? display_tabs::contentForNavTab(screen) : display_tabs::contentForTab(screen);
}
static inline int tabCountForUi() {
  return ui_nav_mode::isTabMode() ? display_tabs::getNavTabCount() : display_tabs::getTabCount();
}

void IRAM_ATTR tpagerEncoderIsr() {
  uint8_t a = (uint8_t)digitalRead(ENC_A);
  uint8_t b = (uint8_t)digitalRead(ENC_B);
  uint8_t curr = (uint8_t)((a << 1) | b);
  uint8_t idx = (uint8_t)((s_encLastState << 2) | curr);
  s_encLastState = curr;
  int8_t d = kEncQuadTable[idx & 15];
  if (d != 0) {
    s_encAccum.fetch_add(d, std::memory_order_relaxed);
  }
}

template<typename Fn>
static void syncDraw(Fn fn) {
  if (!radio::takeMutex(pdMS_TO_TICKS(5000))) return;
  radio::standbyChipUnderMutex();
  fn();
  radio::releaseMutex();
}

static void drawTruncRaw(int x, int y, const char* s, int maxLen) {
  if (!s_gfxReady) return;
  char buf[MAX_LINE_CHARS + 4];
  int i = 0;
  while (s[i] && i < maxLen && i < (int)sizeof(buf) - 1) {
    buf[i] = (char)cp1251_to_rusfont((unsigned char)s[i]);
    i++;
  }
  buf[i] = '\0';
  gfx.setCursor(x, y);
  gfx.print(buf);
}

static void drawTruncUtf8(int x, int y, const char* s, int maxLen) {
  if (!s_gfxReady) return;
  char buf[MAX_LINE_CHARS + 4];
  const char* u = utf8rus(s);
  int i = 0;
  while (u[i] && i < maxLen && i < (int)sizeof(buf) - 1) {
    buf[i] = (char)cp1251_to_rusfont((unsigned char)u[i]);
    i++;
  }
  buf[i] = '\0';
  gfx.setCursor(x, y);
  gfx.print(buf);
}

static void drawContentLine(int line, const char* s, bool useUtf8 = false) {
  int y = submenuListY0Tpager(true) + line * LINE_H;
  if (useUtf8) drawTruncUtf8(CONTENT_X, y, s, MAX_LINE_CHARS);
  else drawTruncRaw(CONTENT_X, y, s, MAX_LINE_CHARS);
}

void displayInit() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  s_encLastState = (uint8_t)(((uint8_t)digitalRead(ENC_A) << 1) | (uint8_t)digitalRead(ENC_B));
  attachInterrupt(digitalPinToInterrupt(ENC_A), tpagerEncoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), tpagerEncoderIsr, CHANGE);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  syncDraw([]() {
    gfx.init();
    gfx.setRotation(ui_display_prefs::getFlip180() ? 2 : 0);
    gfx.fillScreen(COL_BG);
    gfx.setTextColor(COL_FG, COL_BG);
    gfx.setTextSize(1);
  });
  s_gfxReady = true;
  s_lastActivityTime = millis();
}

void displayApplyRotationFromPrefs() {
  if (!s_gfxReady) return;
  syncDraw([]() { gfx.setRotation(ui_display_prefs::getFlip180() ? 2 : 0); });
}

void displaySleep() {
  if (!s_gfxReady || s_displaySleeping) return;
  digitalWrite(TFT_BL, LOW);
  s_displaySleeping = true;
}

void displayWake() {
  if (!s_gfxReady || !s_displaySleeping) return;
  digitalWrite(TFT_BL, HIGH);
  s_displaySleeping = false;
  s_lastActivityTime = millis();
}

void displayWakeRequest() { s_wakeRequested = true; }

bool displayIsSleeping() { return s_displaySleeping; }

bool displayIsMenuActive() { return s_menuActive; }

void displayClear() {
  s_showingBootScreen = false;
  if (!s_gfxReady) return;
  syncDraw([]() { gfx.fillScreen(COL_BG); });
}

void displayText(int x, int y, const char* text) {
  if (!s_gfxReady) return;
  char buf[64];
  int i = 0;
  while (text[i] && i < 63) {
    buf[i] = (char)cp1251_to_rusfont((unsigned char)text[i]);
    i++;
  }
  buf[i] = '\0';
  syncDraw([&]() {
    gfx.setTextColor(COL_FG, COL_BG);
    gfx.setCursor(x, y);
    gfx.print(buf);
  });
}

void displayShow() { /* LovyanGFX: немедленная отрисовка */ }

void displaySetTextSize(uint8_t s) {
  if (s_gfxReady) gfx.setTextSize(s);
}

void displayShowInitProgress(int doneCount, int totalSteps, const char* statusLine) {
  if (!s_gfxReady) return;
  s_showingBootScreen = false;
  if (totalSteps < 1) totalSteps = 1;
  if (doneCount < 0) doneCount = 0;
  if (doneCount > totalSteps) doneCount = totalSteps;
  const int titleY = 72;
  const int cy = 102;
  const int r = 5;
  const int n = totalSteps;
  const int pitch = n > 1 ? (SCREEN_WIDTH - 48) / (n - 1) : 0;
  const int pitchClamped = pitch < 32 ? 32 : (pitch > 56 ? 56 : pitch);
  const int startX = SCREEN_WIDTH / 2 - ((n - 1) * pitchClamped) / 2;
  syncDraw([&]() {
    drawSubScreenChrome();
    gfx.setTextColor(COL_FG, COL_BG);
    gfx.setTextSize((uint8_t)ui_typography::bodyTextSizeTpager());
    drawTruncRaw(80, titleY, locale::getForDisplay("init_title"), 40);
    for (int i = 0; i < n - 1; i++) {
      const int cx0 = startX + i * pitchClamped;
      const int cx1 = startX + (i + 1) * pitchClamped;
      gfx.drawLine(cx0 + r, cy, cx1 - r, cy, COL_FG);
    }
    for (int i = 0; i < n; i++) {
      const int cx = startX + i * pitchClamped;
      if (i < doneCount) {
        gfx.fillCircle(cx, cy, r, COL_FG);
      } else if (i == doneCount && doneCount < totalSteps) {
        gfx.drawCircle(cx, cy, r, COL_FG);
        gfx.fillCircle(cx, cy, 2, COL_FG);
      } else {
        gfx.drawCircle(cx, cy, r, COL_FG);
      }
    }
    if (statusLine && statusLine[0])
      drawTruncRaw(80, titleY + 44, statusLine, 40);
    drawTruncRaw(80, titleY + 84, locale::getForDisplay("init_hint"), 40);
  });
}

void displayShowBootScreen() {
  if (!s_gfxReady) return;
  s_showingBootScreen = true;
  syncDraw([]() {
    gfx.fillScreen(COL_BG);
    gfx.setTextColor(COL_FG, COL_BG);
    gfx.setTextSize((uint8_t)ui_typography::bootTitleTextSizeTpager());
    gfx.setCursor(120, 80);
    gfx.print("RiftLink");
    gfx.setTextSize((uint8_t)ui_typography::bodyTextSizeTpager());
    char ver[24];
    snprintf(ver, sizeof(ver), "v%s", RIFTLINK_VERSION);
    gfx.setCursor(200, 120);
    gfx.print(ver);
  });
}

#define BTN_ACTIVE_LOW 1
#define BTN_PRESSED (digitalRead(BUTTON_PIN) == (BTN_ACTIVE_LOW ? LOW : HIGH))

enum PressType { PRESS_NONE = 0, PRESS_SHORT = 1, PRESS_LONG = 2 };

static int waitButtonPressWithType(uint32_t timeoutMs) {
  uint32_t start = millis();
  uint32_t pressStart = 0;
  bool wasPressed = false;
  while (millis() - start < timeoutMs) {
    if (BTN_PRESSED) {
      if (!wasPressed) pressStart = millis();
      wasPressed = true;
    } else if (wasPressed) {
      uint32_t hold = millis() - pressStart;
      if (hold >= LONG_PRESS_MS) return PRESS_LONG;
      if (hold >= MIN_PRESS_MS) return PRESS_SHORT;
      wasPressed = false;
    }
    delay(20);
  }
  return PRESS_NONE;
}

bool displayShowLanguagePicker() {
  if (!s_gfxReady) return false;
  delay(200);
  int pickLang = locale::getLang();
  const char* items[] = {
    locale::getForDisplay("lang_en"),
    locale::getForDisplay("lang_ru"),
    locale::getForDisplay("menu_back"),
  };
  int sel = displayShowPopupMenu(
      "",
      items, 3,
      pickLang == LANG_EN ? 0 : 1,
      POPUP_MODE_CANCEL,
      nullptr,
      true,
      true);
  if (sel < 0) return true;
  if (sel == 2) return true;
  locale::setLang(sel == 0 ? LANG_EN : LANG_RU);
  return true;
}

bool displayShowRegionPicker() {
  if (!s_gfxReady) return false;
  delay(200);
  int nPresets = region::getPresetCount();
  if (nPresets <= 0) return false;
  int pickIdx = 0;
  for (int i = 0; i < nPresets; i++) {
    if (strcasecmp(region::getPresetCode(i), region::getCode()) == 0) {
      pickIdx = i;
      break;
    }
  }
  const char* codes[25];
  if (nPresets > 24) nPresets = 24;
  for (int i = 0; i < nPresets; i++) codes[i] = region::getPresetCode(i);
  codes[nPresets] = locale::getForDisplay("menu_back");
  int sel = displayShowPopupMenu("", codes, nPresets + 1, pickIdx, POPUP_MODE_CANCEL, nullptr, true, true);
  if (sel < 0) return true;
  if (sel == nPresets) return true;
  region::setRegion(region::getPresetCode(sel));
  {
    const int nCh = region::getChannelCount();
    if (nCh > 0) {
      int curCh = region::getChannel();
      if (curCh < 0) curCh = 0;
      if (curCh >= nCh) curCh = nCh - 1;
      char chBuf[5][24];
      const char* chItems[6];
      for (int i = 0; i < nCh; i++) {
        snprintf(chBuf[i], sizeof(chBuf[0]), "%.1f MHz", (double)region::getChannelMHz(i));
        chItems[i] = chBuf[i];
      }
      chItems[nCh] = locale::getForDisplay("menu_back");
      int chSel = displayShowPopupMenu("", chItems, nCh + 1, curCh, POPUP_MODE_CANCEL, nullptr, true, true);
      if (chSel >= 0 && chSel < nCh) region::setChannel(chSel);
    }
  }
  return true;
}

static void displayShowModemPicker() {
  if (!s_gfxReady) return;
  delay(200);
  int pickIdx = (int)radio::getModemPreset();
  if (pickIdx < 0 || pickIdx > 4) pickIdx = 1;
  const char* names[] = {"Speed", "Normal", "Range", "MaxRange", "Custom"};
  const char* desc[]  = {"SF7 BW250", "SF7 BW125", "SF10 BW125", "SF12 BW125", ""};
  char m0[28], m1[28], m2[28], m3[28], m4[28];
  snprintf(m0, sizeof(m0), "%s %s", names[0], desc[0]);
  snprintf(m1, sizeof(m1), "%s %s", names[1], desc[1]);
  snprintf(m2, sizeof(m2), "%s %s", names[2], desc[2]);
  snprintf(m3, sizeof(m3), "%s %s", names[3], desc[3]);
  {
    char b[24];
    snprintf(b, sizeof(b), "SF%u BW%.0f CR%u",
        radio::getSpreadingFactor(), radio::getBandwidth(), radio::getCodingRate());
    snprintf(m4, sizeof(m4), "%s %s", names[4], b);
  }
  const char* items[] = {
    m0, m1, m2, m3, m4,
    locale::getForDisplay("menu_back"),
  };
  int sel = displayShowPopupMenu("", items, 6, pickIdx, POPUP_MODE_CANCEL, nullptr, true, true);
  if (sel < 0) return;
  if (sel == 5) return;
  if (sel < 4) radio::requestModemPreset((radio::ModemPreset)sel);
}

static void displayRunModemScan() {
  if (!s_gfxReady) return;
  prepareTabLayoutShiftTpager();
  const int listY = submenuListY0Tpager(false);
  syncDraw([&]() {
    drawChromeTabsOrIdleRowTpager();
    gfx.setTextColor(COL_FG, COL_BG);
    drawTruncRaw(8, listY, locale::getForDisplay("scanning"), 40);
    drawTruncRaw(8, listY + LINE_H, "~36s ...", 40);
  });

  static selftest::ScanResult res[6];
  int found = selftest::modemScan(res, 6);
  s_lastActivityTime = millis();

  const char* back = locale::getForDisplay("menu_back");
  if (found == 0) {
    const char* items[] = {
      locale::getForDisplay("scan_empty"),
      back,
    };
    for (;;) {
      int sel = displayShowPopupMenu("", items, 2, 1, POPUP_MODE_CANCEL, nullptr, true, true);
      if (sel < 0 || sel == 1) return;
    }
  }
  char rows[4][32];
  const int nShow = (found > 4) ? 4 : found;
  for (int i = 0; i < nShow; i++) {
    snprintf(rows[i], sizeof(rows[0]), "SF%u BW%.0f %ddBm", res[i].sf, res[i].bw, res[i].rssi);
  }
  const char* items[7];
  items[0] = locale::getForDisplay("scan_found");
  for (int i = 0; i < nShow; i++) items[1 + i] = rows[i];
  items[1 + nShow] = back;
  const int count = nShow + 2;
  for (;;) {
    int sel = displayShowPopupMenu("", items, count, count - 1, POPUP_MODE_CANCEL, nullptr, true, true);
    if (sel < 0 || sel == count - 1) return;
  }
}

/* Шире корпуса OLED (19), чтобы «100%» и центр молнии 14px совпадали с шириной корпуса. */
static constexpr int kBatBodyW = 23;
/** Чуть выше 8×8 глифа в строке BLE/WiFi (+1 px), один контур. */
static constexpr int kBatBodyH = 9;
static constexpr int kBatNubW = 2;
static constexpr int kBatteryIconBarW = kBatBodyW + kBatNubW;

static void drawBatteryChargingBoltGfx(int x, int y) {
  const int ox = x + (kBatBodyW - 14) / 2 - 1;
  const int dy = (kBatBodyH - 7) / 2;
  const auto zig = [&](int dx) {
    gfx.drawLine(ox + 8 + dx, y + 1 + dy, ox + 6 + dx, y + 3 + dy, COL_FG);
    gfx.drawLine(ox + 6 + dx, y + 3 + dy, ox + 9 + dx, y + 3 + dy, COL_FG);
    gfx.drawLine(ox + 9 + dx, y + 3 + dy, ox + 7 + dx, y + 5 + dy, COL_FG);
  };
  zig(0);
  zig(1);
}

static void drawBatteryIcon(int x, int y, int pct, bool charging) {
  gfx.drawRect(x, y, kBatBodyW, kBatBodyH, COL_FG);
  gfx.fillRect(x + kBatBodyW, y + (kBatBodyH - 3) / 2, kBatNubW, 3, COL_FG);
  if (charging) {
    drawBatteryChargingBoltGfx(x, y);
    return;
  }
  char b[8];
  if (pct >= 0) {
    snprintf(b, sizeof(b), "%d%%", pct);
  } else {
    snprintf(b, sizeof(b), "--");
  }
  gfx.setTextSize(1);
  gfx.setTextColor(COL_FG);
  const int tw = gfx.textWidth(b);
  const int th = gfx.fontHeight();
  const int16_t tx = x + (kBatBodyW - tw) / 2;
  const int16_t ty = y + (kBatBodyH - th) / 2;
  gfx.setCursor(tx, ty);
  gfx.print(b);
}

static void drawSignalBars(int x, int y, int barsCount) {
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;
    int bx = x + i * 4;
    int by = y + 8 - h;
    if (i < barsCount) gfx.fillRect(bx, by, 3, h, COL_FG);
    else gfx.drawRect(bx, by, 3, h, COL_FG);
  }
}

/** ySig=8 — верхний статус и замена полоски вкладок при автоскрытии (та же зона, что drawTabBarTpager). */
static void drawStatusBarTpagerAt(int ySig) {
  ui_topbar::Model tb;
  ui_topbar::fill(tb);
  char buf[32];
  const int pct = tb.batteryPercent;
  const bool chg = tb.charging;

  int rightClusterLeft = SCREEN_WIDTH - 2;
  if (tb.hasTime) {
    snprintf(buf, sizeof(buf), "%02d:%02d", tb.hour, tb.minute);
    rightClusterLeft -= (int)strlen(buf) * 6 + 4;
  }
  rightClusterLeft -= kBatteryIconBarW;
  rightClusterLeft += 1;

  drawSignalBars(8, ySig, tb.signalBars);
  const int leftBlockEnd = 30;
  char mid[48];
  snprintf(mid, sizeof(mid), "%s %s", tb.regionModem, tb.linkIsBle ? "BLE" : "WiFi");
  {
    const int avail = rightClusterLeft - leftBlockEnd - 4;
    if (avail >= 6) {
      int maxChars = avail / 6;
      if (maxChars < 1) maxChars = 1;
      const size_t len = strlen(mid);
      if ((int)len > maxChars) {
        mid[maxChars] = '\0';
      }
      const int textW = (int)strlen(mid) * 6;
      const int xMid = leftBlockEnd + (avail - textW) / 2;
      drawTruncRaw(xMid, ySig + 2, mid, maxChars);
    }
  }

  int xRight = SCREEN_WIDTH - 2;
  xRight -= kBatteryIconBarW;
  xRight += 1;
  const int batX = xRight;
  if (tb.hasTime) {
    snprintf(buf, sizeof(buf), "%02d:%02d", tb.hour, tb.minute);
    const int tw = (int)strlen(buf) * 6;
    const int xTime = batX - 6 - tw;
    drawTruncRaw(xTime, ySig + 2, buf, 6);
  }
  drawBatteryIcon(batX, ySig - 1, pct, chg);
}

static void drawStatusBarTpager() {
  drawStatusBarTpagerAt(8);
}

/** drawUpperTopbar: false при скрытых вкладках — только двойная линия; статус в зоне полоски вкладок. */
static void drawSubScreenChromeTpager(bool drawUpperTopbar) {
  if (!s_gfxReady) return;
  gfx.fillScreen(COL_BG);
  gfx.setTextSize((uint8_t)ui_typography::bodyTextSizeTpager());
  gfx.setTextColor(COL_FG, COL_BG);
  if (drawUpperTopbar) drawStatusBarTpager();
  const int statusH = 30;
  gfx.drawLine(0, statusH, SCREEN_WIDTH - 1, statusH, COL_FG);
  gfx.drawLine(0, statusH + 2, SCREEN_WIDTH - 1, statusH + 2, COL_FG);
}

static void drawSubScreenChrome() {
  drawSubScreenChromeTpager(true);
}

static void drawTabBarTpager() {
  if (!s_gfxReady) return;
  const int y0 = 0;
  const int h = TPAGER_STATUS_H;
  const int n = display_tabs::getNavTabCount();
  if (n < 1) return;
  const int activeIdx = display_tabs::clampNavTabIndex(s_currentScreen);
  if (activeIdx != s_currentScreen) s_currentScreen = activeIdx;
  const int cellW = SCREEN_WIDTH / n;
  const int rem = SCREEN_WIDTH - n * cellW;
  const display_tabs::ContentTab activeCt = display_tabs::contentForNavTab(activeIdx);
  for (int i = 0; i < n; i++) {
    const int x0 = i * cellW;
    const int w = cellW + (i == n - 1 ? rem : 0);
    const uint8_t* icon = display_tabs::iconForNavTab(i);
    const int ix = x0 + (w - ICON_W) / 2;
    const int iy = y0 + (h - ICON_H) / 2;
    const bool sel = (display_tabs::contentForNavTab(i) == activeCt);
    if (sel) {
      gfx.fillRect(x0, y0, w, h, COL_FG);
      gfx.drawBitmap(ix, iy, icon, ICON_W, ICON_H, COL_BG);
      gfx.drawFastHLine(x0, y0 + h - 1, w, COL_BG);
    } else {
      gfx.drawBitmap(ix, iy, icon, ICON_W, ICON_H, COL_FG);
    }
  }
}

static void prepareTabLayoutShiftTpager() {
  /* Вкладки в зоне топбара; отдельной полосы под статусом нет. */
  s_layoutTabShiftY = 0;
}

static void drawChromeTabsOrIdleRowTpager() {
  if (!ui_nav_mode::isTabMode()) {
    drawSubScreenChrome();
    return;
  }
  if (ui_tab_bar_idle::tabStripVisible()) {
    if (!s_gfxReady) return;
    gfx.fillScreen(COL_BG);
    gfx.setTextSize((uint8_t)ui_typography::bodyTextSizeTpager());
    gfx.setTextColor(COL_FG, COL_BG);
    drawTabBarTpager();
    const int statusH = TPAGER_STATUS_H;
    gfx.drawLine(0, statusH, SCREEN_WIDTH - 1, statusH, COL_FG);
    gfx.drawLine(0, statusH + 2, SCREEN_WIDTH - 1, statusH + 2, COL_FG);
  } else {
    drawSubScreenChromeTpager(false);
    drawStatusBarTpagerAt(8);
  }
}

static int submenuListY0Tpager(bool hasTitle) {
  const int base = hasTitle ? (CONTENT_Y + SUBMENU_TITLE_H_TPAGER + 6) : (CONTENT_Y + 6);
  return base + s_layoutTabShiftY;
}

static void drawSubmenuTitleBarTpager(const char* title) {
  if (!s_gfxReady || !title || !title[0]) return;
  const int top = CONTENT_Y + s_layoutTabShiftY;
  gfx.fillRect(2, top, SCREEN_WIDTH - 4, SUBMENU_TITLE_H_TPAGER, COL_DIM);
  gfx.setTextColor(COL_FG, COL_DIM);
  drawTruncRaw(8, top + 6, title, 40);
  gfx.setTextColor(COL_FG, COL_BG);
}

static void drawSubmenuFooterSepTpager() {
  if (!s_gfxReady) return;
  gfx.drawFastHLine(8, SCREEN_HEIGHT - 22, SCREEN_WIDTH - 16, COL_DIM);
}

static const char* homeMenuLabelForSlot(int slot) {
  switch (display_tabs::homeMenuContentAt(slot)) {
    case display_tabs::CT_MAIN: return locale::getForDisplay("menu_home_node");
    case display_tabs::CT_MSG: return locale::getForDisplay("menu_home_msg");
    case display_tabs::CT_INFO: return locale::getForDisplay("menu_home_peers");
    case display_tabs::CT_GPS: return locale::getForDisplay("tab_gps");
    case display_tabs::CT_NET: return locale::getForDisplay("menu_home_lora");
    case display_tabs::CT_SYS: return locale::getForDisplay("menu_home_settings");
    case display_tabs::CT_POWER: return locale::getForDisplay("menu_home_power");
    default: return "?";
  }
}

static void drawHomeScreen() {
  if (!s_gfxReady) return;
  drawSubScreenChrome();
  const int menuY0 = CONTENT_Y + 6;
  const int nItems = display_tabs::homeMenuCount();
  const int rowStep = (nItems >= 7) ? HOME_MENU_ROW_STEP_TPAGER_7
                                    : ((nItems >= 6) ? HOME_MENU_ROW_STEP_TPAGER_6 : HOME_MENU_ROW_STEP_TPAGER_5);
  const int bottomPad = 8;
  int showMax = (SCREEN_HEIGHT - menuY0 - bottomPad) / rowStep;
  if (showMax < 1) showMax = 1;
  if (showMax > nItems) showMax = nItems;
  int scrollRef = s_homeMenuScrollOff;
  ui_scroll::syncListWindow(s_homeMenuIndex, nItems, showMax, scrollRef);
  s_homeMenuScrollOff = scrollRef;

  if (ui_scroll::canScrollUp(s_homeMenuScrollOff)) {
    gfx.fillTriangle(SCREEN_WIDTH - 10, menuY0 - 2, SCREEN_WIDTH - 18, menuY0 + 6, SCREEN_WIDTH - 2, menuY0 + 6, COL_FG);
  }
  for (int vis = 0; vis < showMax; vis++) {
    const int i = s_homeMenuScrollOff + vis;
    const uint8_t* icon = display_tabs::iconForContent(display_tabs::homeMenuContentAt(i));
    int y = menuY0 + vis * rowStep;
    const int innerOff = (rowStep > 8) ? (rowStep - 8) / 2 : 0;
    if (i == s_homeMenuIndex) {
      gfx.fillRect(MENU_SEL_X, y, MENU_SEL_W, rowStep, COL_FG);
      gfx.setTextColor(COL_BG, COL_FG);
      gfx.drawBitmap(16, y + innerOff, icon, ICON_W, ICON_H, COL_BG);
    } else {
      gfx.setTextColor(COL_FG, COL_BG);
      gfx.drawBitmap(16, y + innerOff, icon, ICON_W, ICON_H, COL_FG);
    }
    drawTruncRaw(16 + ICON_W + 12, y + innerOff, homeMenuLabelForSlot(i), MAX_LINE_CHARS - 4);
  }
  if (ui_scroll::canScrollDown(s_homeMenuScrollOff, nItems, showMax)) {
    const int triY = menuY0 + (showMax - 1) * rowStep + rowStep - 4;
    gfx.fillTriangle(SCREEN_WIDTH - 10, triY, SCREEN_WIDTH - 18, triY - 4, SCREEN_WIDTH - 2, triY - 4, COL_FG);
  }
  gfx.setTextColor(COL_FG, COL_BG);
}

static void drawContentMain() {
  if (!s_gfxReady) return;
  char buf[48];
  const uint8_t nodeSz = (uint8_t)ui_typography::nodeTabTextSizeTpager();
  gfx.setTextSize(nodeSz);

  const int listY0Raw = submenuListY0Tpager(false) + NODE_MAIN_LIST_TOP_PAD_PX;
  const int minY = NODE_MAIN_MIN_BASELINE_Y + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < minY) ? minY : listY0Raw;
  const int lh = ui_typography::nodeMsgLineStepTpager();
  const uint8_t* nid = node::getId();
  char idHex[20];
  snprintf(idHex, sizeof(idHex), "%02X%02X%02X%02X%02X%02X%02X%02X",
      nid[0], nid[1], nid[2], nid[3], nid[4], nid[5], nid[6], nid[7]);

  char nick[33];
  node::getNickname(nick, sizeof(nick));

  const bool tabNoBack = ui_nav_mode::isTabMode();
  const int numRows = 1 + (nick[0] ? 1 : 0) + 1 + (tabNoBack ? 0 : 1);

  int ry[5];
  int ri = 0;
  ry[ri++] = listY0;
  if (nick[0]) ry[ri++] = listY0 + lh;
  ry[ri++] = listY0 + lh * (nick[0] ? 2 : 1);
  if (!tabNoBack) {
    ry[ri++] = ry[ri - 1] + lh;
    ry[numRows - 1] += NODE_BACK_ROW_GAP_PX;
  }

  const int contentTop = listY0 - 1;
  const int contentBottom = ry[numRows - 1] + lh;
  const int viewportBottom = SCREEN_HEIGHT - 8;
  const int viewportH = viewportBottom - contentTop;
  const int totalH = contentBottom - contentTop;
  const int sMaxChrome = listY0 - (NODE_MAIN_MIN_BASELINE_Y + s_layoutTabShiftY);
  s_tpagerMainMaxScrollPx = ui_content_scroll::maxScrollForOverflow(totalH, viewportH);
  s_tpagerMainScrollPx = ui_content_scroll::clampScroll(s_tpagerMainScrollPx, s_tpagerMainMaxScrollPx);
  {
    int rowTop = ry[numRows - 1];
    int s_lo = rowTop + lh - viewportBottom;
    if (s_lo < 0) s_lo = 0;
    int s_hi = rowTop - contentTop;
    if (s_hi > s_tpagerMainMaxScrollPx) s_hi = s_tpagerMainMaxScrollPx;
    if (sMaxChrome >= 0 && s_hi > sMaxChrome) s_hi = sMaxChrome;
    if (s_lo <= s_hi) {
      if (s_tpagerMainScrollPx < s_lo) s_tpagerMainScrollPx = s_lo;
      else if (s_tpagerMainScrollPx > s_hi) s_tpagerMainScrollPx = s_hi;
    }
  }
  if (sMaxChrome >= 0 && s_tpagerMainScrollPx > sMaxChrome) s_tpagerMainScrollPx = sMaxChrome;
  const int s = s_tpagerMainScrollPx;

  if (nick[0]) {
    gfx.setTextColor(COL_FG, COL_BG);
    drawTruncUtf8(CONTENT_X, listY0 - s, nick, MAX_LINE_CHARS - 2);
    const int yId = listY0 + lh - s;
    gfx.setTextColor(COL_FG, COL_BG);
    snprintf(buf, sizeof(buf), "%s %s", locale::getForDisplay("id"), idHex);
    drawTruncRaw(CONTENT_X, yId, buf, MAX_LINE_CHARS);
  } else {
    gfx.setTextColor(COL_FG, COL_BG);
    drawTruncRaw(CONTENT_X, listY0 - s, idHex, MAX_LINE_CHARS);
  }

  int n = neighbors::getCount();
  int avgRssi = neighbors::getAverageRssi();
  snprintf(buf, sizeof(buf), "%s: %d", locale::getForDisplay("neighbors"), n);
  const int yNbDraw = listY0 + lh * (nick[0] ? 2 : 1) - s;
  gfx.setTextColor(COL_FG, COL_BG);
  drawTruncRaw(CONTENT_X, yNbDraw, buf, MAX_LINE_CHARS);
  if (n > 0) drawSignalBars(SCREEN_WIDTH - 40, yNbDraw, ui_topbar::rssiToBars(avgRssi));

  if (!tabNoBack) {
    const int yBack = ry[numRows - 1] - s;
    gfx.fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, COL_FG);
    gfx.setTextColor(COL_BG, COL_FG);
    drawTruncUtf8(CONTENT_X, yBack, locale::getForDisplay("menu_back"), MAX_LINE_CHARS);
    gfx.setTextColor(COL_FG, COL_BG);
  }

  gfx.setTextSize((uint8_t)ui_typography::bodyTextSizeTpager());
}

static void drawContentInfo() {
  if (!s_gfxReady) return;
  gfx.setTextSize((uint8_t)ui_typography::bodyTextSizeTpager());
  const int listY0Raw = submenuListY0Tpager(false) + NODE_MAIN_LIST_TOP_PAD_PX;
  const int minY = NODE_MAIN_MIN_BASELINE_Y + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < minY) ? minY : listY0Raw;
  const int lh = LINE_H;
  const bool tabNoBack = ui_nav_mode::isTabMode();
  const int bottomPad = tabNoBack ? 2 : 6;
  const int yBack = SCREEN_HEIGHT - bottomPad - lh - 1;
  int maxNumLines = (yBack - listY0 - NODE_BACK_ROW_GAP_PX) / lh;
  if (maxNumLines < 1) maxNumLines = 1;

  char buf[32];
  int n = neighbors::getCount();
  gfx.setTextColor(COL_FG, COL_BG);
  snprintf(buf, sizeof(buf), "%s: %d", locale::getForDisplay("neighbors"), n);
  drawTruncRaw(CONTENT_X, listY0, buf, MAX_LINE_CHARS);

  const int rowsAfterP = maxNumLines - 1;
  int maxShow = 0;
  int extraRow = 0;
  if (rowsAfterP > 0) {
    if (n <= rowsAfterP) {
      maxShow = n;
    } else {
      maxShow = (rowsAfterP >= 1) ? (rowsAfterP - 1) : 0;
      extraRow = 1;
    }
  }
  for (int i = 0; i < maxShow; i++) {
    char hex[17];
    neighbors::getIdHex(i, hex);
    int rssi = neighbors::getRssi(i);
    snprintf(buf, sizeof(buf), "%c%c%c%c %ddBm", hex[0], hex[1], hex[2], hex[3], rssi);
    drawTruncRaw(CONTENT_X, listY0 + (1 + i) * lh, buf, 24);
    drawSignalBars(SCREEN_WIDTH - 40, listY0 + (1 + i) * lh, ui_topbar::rssiToBars(rssi));
  }
  if (extraRow) {
    snprintf(buf, sizeof(buf), "+%d more", n - maxShow);
    drawTruncRaw(CONTENT_X, listY0 + (1 + maxShow) * lh, buf, MAX_LINE_CHARS);
  }

  if (!tabNoBack) {
    gfx.fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, COL_FG);
    gfx.setTextColor(COL_BG, COL_FG);
    drawTruncUtf8(CONTENT_X, yBack, locale::getForDisplay("menu_back"), MAX_LINE_CHARS);
    gfx.setTextColor(COL_FG, COL_BG);
  }
}

static void drawContentNet() {
  if (!s_gfxReady) return;
  gfx.setTextSize((uint8_t)ui_typography::bodyTextSizeTpager());
  const int lh = LINE_H;
  const int bottomPad = 6;
  const int gapMid = ui_nav_mode::isTabMode() ? 1 : 4;
  const int yBack = SCREEN_HEIGHT - bottomPad - lh - 1;
  const int listY0Raw = submenuListY0Tpager(false) + NODE_MAIN_LIST_TOP_PAD_PX;
  const int minY = NODE_MAIN_MIN_BASELINE_Y + s_layoutTabShiftY;
  const int yModeRow = (listY0Raw < minY) ? minY : listY0Raw;
  const int yStat0 = yModeRow + lh + gapMid;
  const bool haveMid = (yStat0 + 2 * lh <= yBack - 2);
  const int yStatCompact = yModeRow + lh + 1;
  const bool haveMidCompact = ui_nav_mode::isTabMode() && !haveMid && (yStatCompact + 2 * lh <= yBack - 2);
  const int yDrawStat = haveMid ? yStat0 : (haveMidCompact ? yStatCompact : yStat0);

  const bool isBle = (radio_mode::current() == radio_mode::BLE);
  char buf[40];
  const bool netSel = !ui_nav_mode::isTabMode() || s_tabDrillIn;
  gfx.setTextColor(COL_FG, COL_BG);

  const char* modeLine = locale::getForDisplay(isBle ? "net_mode_line_ble" : "net_mode_line_wifi");
  if (netSel && s_netMenuIndex == 0) {
    gfx.fillRect(0, yModeRow - 2, SCREEN_WIDTH, lh + 1, COL_FG);
    gfx.setTextColor(COL_BG, COL_FG);
  } else {
    gfx.setTextColor(COL_FG, COL_BG);
  }
  drawTruncRaw(CONTENT_X, yModeRow, modeLine, MAX_LINE_CHARS);
  gfx.setTextColor(COL_FG, COL_BG);

  if (haveMid || haveMidCompact) {
    if (isBle) {
      char advName[24];
      ble::getAdvertisingName(advName, sizeof(advName));
      drawTruncRaw(CONTENT_X, yDrawStat + 0 * lh, advName, MAX_LINE_CHARS);
      snprintf(buf, sizeof(buf), "%s %06u", locale::getForDisplay("pin"), (unsigned)ble::getPasskey());
      drawTruncRaw(CONTENT_X, yDrawStat + 1 * lh, buf, MAX_LINE_CHARS);
    } else {
      char ssid[32] = {0};
      char ip[20] = {0};
      if (wifi::isStaConnecting()) {
        drawTruncRaw(CONTENT_X, yDrawStat + 0 * lh, locale::getForDisplay("net_wifi_connecting"), MAX_LINE_CHARS);
        if (wifi::hasCredentials()) {
          wifi::getSavedSsid(ssid, sizeof(ssid));
        }
        if (!ssid[0]) {
          ssid[0] = '-';
          ssid[1] = '\0';
        }
        drawTruncUtf8(CONTENT_X, yDrawStat + 1 * lh, ssid, MAX_LINE_CHARS);
      } else if (wifi::isConnected()) {
        wifi::getStatus(ssid, sizeof(ssid), ip, sizeof(ip));
        if (!ssid[0]) {
          ssid[0] = '-';
          ssid[1] = '\0';
        }
        drawTruncUtf8(CONTENT_X, yDrawStat + 0 * lh, ssid, MAX_LINE_CHARS);
        drawTruncUtf8(CONTENT_X, yDrawStat + 1 * lh, ip[0] ? ip : "-", MAX_LINE_CHARS);
      } else {
        if (wifi::hasCredentials()) {
          wifi::getSavedSsid(ssid, sizeof(ssid));
        }
        if (!ssid[0]) {
          ssid[0] = '-';
          ssid[1] = '\0';
        }
        drawTruncUtf8(CONTENT_X, yDrawStat + 0 * lh, ssid, MAX_LINE_CHARS);
        drawTruncRaw(CONTENT_X, yDrawStat + 1 * lh, "-", MAX_LINE_CHARS);
      }
    }
  }

  if (!ui_nav_mode::isTabMode() || s_tabDrillIn) {
    if (netSel && s_netMenuIndex == 1) {
      gfx.fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, COL_FG);
      gfx.setTextColor(COL_BG, COL_FG);
    } else {
      gfx.fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, COL_BG);
      gfx.setTextColor(COL_FG, COL_BG);
    }
    drawTruncUtf8(CONTENT_X, yBack, locale::getForDisplay("menu_back"), MAX_LINE_CHARS);
    gfx.setTextColor(COL_FG, COL_BG);
  }
}

static int sysDisplaySubMenuCountTp() {
  return ui_nav_mode::isTabMode() ? 5 : 4;
}
static int sysDisplaySubBackIdxTp() { return sysDisplaySubMenuCountTp() - 1; }
static void clampSysDisplaySubMenuIndexTp() {
  int c = sysDisplaySubMenuCountTp();
  if (s_sysMenuIndex >= c) s_sysMenuIndex = c - 1;
}

static void displaySubFillLabelTpager(int idx, char* buf, size_t bufSz) {
  const bool tabs = ui_nav_mode::isTabMode();
  const int backIdx = sysDisplaySubBackIdxTp();
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

static void sysMenuFillLabelTpager(int idx, char* buf, size_t bufSz) {
  switch (idx) {
    case 0: strncpy(buf, locale::getForDisplay("menu_display_submenu"), bufSz); break;
    case 1:
      snprintf(buf, bufSz, "PS:%s", powersave::isEnabled() ? "ON->OFF" : "OFF->ON");
      break;
    case 2: strncpy(buf, locale::getForDisplay("region"), bufSz); break;
    case 3: strncpy(buf, locale::getForDisplay("menu_modem"), bufSz); break;
    case 4: strncpy(buf, locale::getForDisplay("scan_title"), bufSz); break;
    case 5: strncpy(buf, locale::getForDisplay("menu_selftest"), bufSz); break;
    default: buf[0] = '\0'; break;
  }
  buf[bufSz - 1] = '\0';
}

static int sysMainMenuIndexToExecSelTp(int idx) {
  switch (idx) {
    case 1: return 2;
    case 2: return 3;
    case 3: return 0;
    case 4: return 1;
    case 5: return 4;
    default: return -1;
  }
}

static void drawContentSys() {
  if (!s_gfxReady) return;
  gfx.setTextSize((uint8_t)ui_typography::bodyTextSizeTpager());
  const int lh = LINE_H;
  const int rowStep = lh + 2;
  const int bottomPad = 4;
  const int listY0Raw = submenuListY0Tpager(false) + NODE_MAIN_LIST_TOP_PAD_PX;
  const int minY = NODE_MAIN_MIN_BASELINE_Y + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < minY) ? minY : listY0Raw;
  int showMax = (SCREEN_HEIGHT - listY0 - bottomPad) / rowStep;
  if (showMax < 1) showMax = 1;

  char buf[40];
  gfx.setTextColor(COL_FG, COL_BG);

  if (s_sysInDisplaySubmenu) {
    const int kItems = sysDisplaySubMenuCountTp();
    const int backIdx = sysDisplaySubBackIdxTp();
    if (showMax > kItems) showMax = kItems;
    int scrollRef = s_sysScrollOff;
    ui_scroll::syncListWindow(s_sysMenuIndex, kItems, showMax, scrollRef);
    s_sysScrollOff = scrollRef;
    if (s_sysScrollOff > 0) {
      gfx.fillTriangle(SCREEN_WIDTH - 10, listY0 - 2, SCREEN_WIDTH - 18, listY0 + 6, SCREEN_WIDTH - 2, listY0 + 6, COL_FG);
    }
    for (int vis = 0; vis < showMax; vis++) {
      int idx = s_sysScrollOff + vis;
      if (idx >= kItems) break;
      int y = listY0 + vis * rowStep;
      const bool rowSel = (s_sysMenuIndex == idx);
      if (idx == backIdx) {
        const char* backLbl = locale::getForDisplay("menu_back");
        if (rowSel) {
          gfx.fillRect(0, y - 2, SCREEN_WIDTH, rowStep + 1, COL_FG);
          gfx.setTextColor(COL_BG, COL_FG);
        } else {
          gfx.setTextColor(COL_FG, COL_BG);
        }
        drawTruncUtf8(CONTENT_X, y, backLbl, MAX_LINE_CHARS - 4);
      } else {
        displaySubFillLabelTpager(idx, buf, sizeof(buf));
        if (rowSel) {
          gfx.fillRect(0, y - 2, SCREEN_WIDTH, rowStep + 1, COL_FG);
          gfx.setTextColor(COL_BG, COL_FG);
        } else {
          gfx.setTextColor(COL_FG, COL_BG);
        }
        drawTruncUtf8(CONTENT_X, y, buf, MAX_LINE_CHARS - 4);
      }
    }
    gfx.setTextColor(COL_FG, COL_BG);
    if (s_sysScrollOff + showMax < kItems) {
      const int triY = listY0 + (showMax - 1) * rowStep + rowStep - 4;
      gfx.fillTriangle(SCREEN_WIDTH - 10, triY, SCREEN_WIDTH - 18, triY - 4, SCREEN_WIDTH - 2, triY - 4, COL_FG);
    }
    return;
  }

  constexpr int kSysListItems = 7;
  if (showMax > kSysListItems) showMax = kSysListItems;
  const bool sysTabBrowse = ui_nav_mode::isTabMode() && !s_tabDrillIn;
  const int sysListCount = sysTabBrowse ? (kSysListItems - 1) : kSysListItems;
  int scrollRef = sysTabBrowse ? 0 : s_sysScrollOff;
  ui_scroll::syncListWindow(sysTabBrowse ? 0 : s_sysMenuIndex, sysListCount, showMax, scrollRef);
  s_sysScrollOff = scrollRef;

  if (s_sysScrollOff > 0) {
    gfx.fillTriangle(SCREEN_WIDTH - 10, listY0 - 2, SCREEN_WIDTH - 18, listY0 + 6, SCREEN_WIDTH - 2, listY0 + 6, COL_FG);
  }
  for (int vis = 0; vis < showMax; vis++) {
    int idx = s_sysScrollOff + vis;
    if (idx >= sysListCount) break;
    int y = listY0 + vis * rowStep;
    const bool rowSel = !sysTabBrowse && (s_sysMenuIndex == idx);
    if (idx == 6) {
      const char* backLbl = locale::getForDisplay("menu_back");
      if (rowSel) {
        gfx.fillRect(0, y - 2, SCREEN_WIDTH, rowStep + 1, COL_FG);
        gfx.setTextColor(COL_BG, COL_FG);
      } else {
        gfx.setTextColor(COL_FG, COL_BG);
      }
      drawTruncUtf8(CONTENT_X, y, backLbl, MAX_LINE_CHARS - 4);
    } else {
      sysMenuFillLabelTpager(idx, buf, sizeof(buf));
      if (rowSel) {
        gfx.fillRect(0, y - 2, SCREEN_WIDTH, rowStep + 1, COL_FG);
        gfx.setTextColor(COL_BG, COL_FG);
        gfx.drawBitmap(16, y + 2, ui_icons::sysMenuIcon(idx), ICON_W, ICON_H, COL_BG);
      } else {
        gfx.setTextColor(COL_FG, COL_BG);
        gfx.drawBitmap(16, y + 2, ui_icons::sysMenuIcon(idx), ICON_W, ICON_H, COL_FG);
      }
      drawTruncRaw(16 + ICON_W + 12, y, buf, MAX_LINE_CHARS - 4);
    }
  }
  gfx.setTextColor(COL_FG, COL_BG);
  if (s_sysScrollOff + showMax < sysListCount) {
    const int triY = listY0 + (showMax - 1) * rowStep + rowStep - 4;
    gfx.fillTriangle(SCREEN_WIDTH - 10, triY, SCREEN_WIDTH - 18, triY - 4, SCREEN_WIDTH - 2, triY - 4, COL_FG);
  }
}

static void drawContentMsg() {
  if (!s_gfxReady) return;
  gfx.setTextSize((uint8_t)ui_typography::bodyTextSizeTpager());
  const int listY0Raw = submenuListY0Tpager(false) + NODE_MAIN_LIST_TOP_PAD_PX;
  const int minY = NODE_MAIN_MIN_BASELINE_Y + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < minY) ? minY : listY0Raw;
  const int lh = ui_typography::msgBodyLineStepTpager();
  gfx.setTextColor(COL_FG, COL_BG);
  {
    char hdr[MAX_LINE_CHARS + 1];
    snprintf(hdr, sizeof(hdr), "%s %s", locale::getForDisplay("from"), s_lastMsgFrom[0] ? s_lastMsgFrom : "-");
    drawTruncUtf8(CONTENT_X, listY0 + 0 * lh, hdr, MAX_LINE_CHARS);
  }

  const char* txt = s_lastMsgText[0] ? s_lastMsgText : locale::getForDisplay("no_messages");
  const bool utf8Msg = (s_lastMsgText[0] != 0);
  const size_t len = strlen(txt);
  size_t pos = s_msgScrollStart;
  if (pos >= len) pos = 0;

  if (pos > 0 && len > 0) {
    gfx.fillTriangle(SCREEN_WIDTH - 10, listY0 + 1 * lh - 2, SCREEN_WIDTH - 18, listY0 + 1 * lh + 6, SCREEN_WIDTH - 2, listY0 + 1 * lh + 6, COL_FG);
  }
  for (int row = 0; row < 3; row++) {
    char buf[MAX_LINE_CHARS + 1];
    size_t n = 0;
    while (n < (size_t)MAX_LINE_CHARS && pos + n < len) {
      if (txt[pos + n] == '\n') break;
      buf[n++] = txt[pos + n];
    }
    buf[n] = '\0';
    pos += n;
    if (pos < len && txt[pos] == '\n') pos++;
    if (utf8Msg) drawTruncUtf8(CONTENT_X, listY0 + (1 + row) * lh, buf, MAX_LINE_CHARS);
    else drawTruncRaw(CONTENT_X, listY0 + (1 + row) * lh, buf, MAX_LINE_CHARS);
  }
  if (ui_msg_scroll::hasOverflowPastLines(txt, len, s_msgScrollStart, MAX_LINE_CHARS, 3)) {
    gfx.fillTriangle(SCREEN_WIDTH - 10, listY0 + 3 * lh + lh - 4, SCREEN_WIDTH - 18, listY0 + 3 * lh - 4, SCREEN_WIDTH - 2, listY0 + 3 * lh - 4, COL_FG);
  }

  if (!ui_nav_mode::isTabMode()) {
    const int yBack = listY0 + 4 * lh + NODE_BACK_ROW_GAP_PX;
    gfx.fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, COL_FG);
    gfx.setTextColor(COL_BG, COL_FG);
    drawTruncUtf8(CONTENT_X, yBack, locale::getForDisplay("menu_back"), MAX_LINE_CHARS);
    gfx.setTextColor(COL_FG, COL_BG);
  }
}

static void drawGpsBackFooterTpager(bool highlight) {
  if (!s_gfxReady) return;
  const int lh = LINE_H;
  const int bottomPad = 6;
  const int yBack = SCREEN_HEIGHT - bottomPad - lh - 1;
  if (highlight) {
    gfx.fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, COL_FG);
    gfx.setTextColor(COL_BG, COL_FG);
  } else {
    gfx.fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, COL_BG);
    gfx.setTextColor(COL_FG, COL_BG);
  }
  drawTruncUtf8(CONTENT_X, yBack, locale::getForDisplay("menu_back"), MAX_LINE_CHARS);
  gfx.setTextColor(COL_FG, COL_BG);
}

static void drawContentGps() {
  if (!s_gfxReady) return;
  char buf[40];
  gfx.setTextSize((uint8_t)ui_typography::bodyTextSizeTpager());
  const int lh = LINE_H;
  const int listY0Raw = submenuListY0Tpager(false) + NODE_MAIN_LIST_TOP_PAD_PX;
  const int minY = NODE_MAIN_MIN_BASELINE_Y + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < minY) ? minY : listY0Raw;
  const int bottomPad = 6;
  const int yBack = SCREEN_HEIGHT - bottomPad - lh - 1;
  const bool tabDrill = ui_nav_mode::isTabMode() && s_tabDrillIn;
  const bool gpsSel = !ui_nav_mode::isTabMode() || s_tabDrillIn;
  const bool showGpsToggleRow = gps::isPresent() && (tabDrill || !ui_nav_mode::isTabMode());
  const bool showBackFooter = !ui_nav_mode::isTabMode() || tabDrill;
  const int yInfoEnd = showBackFooter ? yBack : (SCREEN_HEIGHT - 2);
  int y = listY0;

  if (showGpsToggleRow) {
    const bool on = gps::isEnabled();
    snprintf(buf, sizeof(buf), "%s", locale::getForDisplay(on ? "gps_toggle_on_off" : "gps_toggle_off_on"));
    if (s_gpsMenuIndex == 0) {
      gfx.fillRect(0, y - 2, SCREEN_WIDTH, lh + 1, COL_FG);
      gfx.setTextColor(COL_BG, COL_FG);
    } else {
      gfx.setTextColor(COL_FG, COL_BG);
    }
    drawTruncUtf8(CONTENT_X, y, buf, MAX_LINE_CHARS);
    gfx.setTextColor(COL_FG, COL_BG);
    y += lh;
  }

  if (!gps::isPresent() && gps::hasPhoneSync()) {
    if (y + lh <= yInfoEnd + 1) {
      drawTruncUtf8(CONTENT_X, y, locale::getForDisplay("gps_phone"), MAX_LINE_CHARS);
      y += lh;
    }
    if (showBackFooter) {
      const bool hiBack = gpsSel && !gps::isPresent() && (s_gpsMenuIndex == 0);
      drawGpsBackFooterTpager(hiBack);
    }
    return;
  }
  if (!gps::isPresent()) {
    if (y + lh <= yInfoEnd + 1) {
      drawTruncUtf8(CONTENT_X, y, locale::getForDisplay("gps_not_present"), MAX_LINE_CHARS);
      y += lh;
    }
    if (y + lh <= yInfoEnd + 1) {
      drawTruncRaw(CONTENT_X, y, "BLE: gps rx,tx,en", MAX_LINE_CHARS);
      y += lh;
    }
    if (showBackFooter) {
      const bool hiBack = gpsSel && !gps::isPresent() && (s_gpsMenuIndex == 0);
      drawGpsBackFooterTpager(hiBack);
    }
    return;
  }

  if (!showGpsToggleRow && y + lh <= yInfoEnd + 1) {
    if (gpsSel && gps::isPresent() && s_gpsMenuIndex == 0) {
      gfx.fillRect(0, y - 2, SCREEN_WIDTH, lh + 1, COL_FG);
      gfx.setTextColor(COL_BG, COL_FG);
    } else {
      gfx.setTextColor(COL_FG, COL_BG);
    }
    drawTruncUtf8(CONTENT_X, y, gps::isEnabled() ? locale::getForDisplay("gps_on") : locale::getForDisplay("gps_off"),
        MAX_LINE_CHARS);
    gfx.setTextColor(COL_FG, COL_BG);
    y += lh;
  }

  const int yMid0 = y;
  int lineY = yMid0;
  char coordLine[48];

  if (gps::isEnabled()) {
    if (!gps::hasFix()) {
      if (lineY + lh <= yInfoEnd + 1) {
        drawTruncUtf8(CONTENT_X, lineY, locale::getForDisplay("gps_search"), MAX_LINE_CHARS);
      }
    } else {
      uint32_t sat = gps::getSatellites();
      float course = gps::getCourseDeg();
      const char* card = gps::getCourseCardinal();
      if (course >= 0 && card && card[0])
        snprintf(buf, sizeof(buf), "%u sat %0.0f %s", (unsigned)sat, course, card);
      else
        snprintf(buf, sizeof(buf), "%u sat", (unsigned)sat);
      if (lineY + lh <= yInfoEnd + 1) {
        drawTruncRaw(CONTENT_X, lineY, buf, MAX_LINE_CHARS);
        lineY += lh;
      }
      snprintf(coordLine, sizeof(coordLine), "%.5f %.5f", (double)gps::getLat(), (double)gps::getLon());
      if (lineY + lh <= yInfoEnd + 1) drawTruncRaw(CONTENT_X, lineY, coordLine, MAX_LINE_CHARS);
    }
  } else if (gps::hasFix()) {
    snprintf(coordLine, sizeof(coordLine), "%.5f %.5f", (double)gps::getLat(), (double)gps::getLon());
    if (lineY + lh <= yInfoEnd + 1) drawTruncRaw(CONTENT_X, lineY, coordLine, MAX_LINE_CHARS);
  }
  if (showBackFooter) {
    const bool hiBack = gpsSel && gps::isPresent() && (s_gpsMenuIndex == 1);
    drawGpsBackFooterTpager(hiBack);
  }
}

static void drawContentPower() {
  if (!s_gfxReady) return;
  gfx.setTextSize((uint8_t)ui_typography::bodyTextSizeTpager());
  const int listY0Raw = submenuListY0Tpager(false) + NODE_MAIN_LIST_TOP_PAD_PX;
  const int minY = NODE_MAIN_MIN_BASELINE_Y + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < minY) ? minY : listY0Raw;
  const int lh = LINE_H;
  const int nLines = (!ui_nav_mode::isTabMode() || s_tabDrillIn) ? 3 : 2;
  const char* items[3] = {
    locale::getForDisplay("menu_power_off"),
    locale::getForDisplay("menu_power_sleep"),
    locale::getForDisplay("menu_back"),
  };
  for (int i = 0; i < nLines; i++) {
    const int y = listY0 + i * lh;
    const bool sel = ui_nav_mode::isTabMode() && s_tabDrillIn && (s_powerMenuIndex == i);
    if (sel) {
      gfx.fillRect(MENU_SEL_X, y, MENU_SEL_W, lh, COL_FG);
      gfx.setTextColor(COL_BG, COL_FG);
    } else {
      gfx.setTextColor(COL_FG, COL_BG);
    }
    drawTruncUtf8(CONTENT_X, y, items[i], MAX_LINE_CHARS);
  }
}

static void drawScreen(int tab) {
  if (ui_nav_mode::isTabMode()) {
    tab = display_tabs::clampNavTabIndex(tab);
    s_currentScreen = tab;
  }
  syncDraw([&]() {
    prepareTabLayoutShiftTpager();
    if (ui_nav_mode::isTabMode()) {
      drawChromeTabsOrIdleRowTpager();
      display_tabs::ContentTab ct = display_tabs::contentForNavTab(tab);
      if (ct != display_tabs::CT_MAIN && ct != display_tabs::CT_MSG && ct != display_tabs::CT_INFO &&
          ct != display_tabs::CT_NET && ct != display_tabs::CT_SYS && ct != display_tabs::CT_GPS && ct != display_tabs::CT_POWER) {
        drawSubmenuTitleBarTpager(ui_section::sectionTitleForContent(ct));
      }
      switch (ct) {
        case display_tabs::CT_MAIN: drawContentMain(); break;
        case display_tabs::CT_MSG:  drawContentMsg(); break;
        case display_tabs::CT_INFO: drawContentInfo(); break;
        case display_tabs::CT_NET:  drawContentNet(); break;
        case display_tabs::CT_SYS:  drawContentSys(); break;
        case display_tabs::CT_GPS:  drawContentGps(); break;
        case display_tabs::CT_POWER: drawContentPower(); break;
        default: break;
      }
      return;
    }
    if (display_tabs::contentForTab(tab) == display_tabs::CT_HOME) {
      drawHomeScreen();
    } else {
      drawSubScreenChrome();
      {
        display_tabs::ContentTab ct = display_tabs::contentForTab(tab);
        if (ct != display_tabs::CT_MAIN && ct != display_tabs::CT_MSG && ct != display_tabs::CT_INFO &&
            ct != display_tabs::CT_NET && ct != display_tabs::CT_SYS && ct != display_tabs::CT_GPS) {
          drawSubmenuTitleBarTpager(ui_section::sectionTitleForContent(ct));
        }
      }
      switch (display_tabs::contentForTab(tab)) {
        case display_tabs::CT_MAIN: drawContentMain(); break;
        case display_tabs::CT_MSG:  drawContentMsg(); break;
        case display_tabs::CT_INFO: drawContentInfo(); break;
        case display_tabs::CT_NET:  drawContentNet(); break;
        case display_tabs::CT_SYS:  drawContentSys(); break;
        case display_tabs::CT_GPS:  drawContentGps(); break;
        default: break;
      }
    }
  });
}

void displaySetLastMsg(const char* fromHex, const char* text) {
  displayWakeRequest();
  s_lastActivityTime = millis();
  if (fromHex) {
    strncpy(s_lastMsgFrom, fromHex, 16);
    s_lastMsgFrom[16] = '\0';
  }
  if (text) {
    strncpy(s_lastMsgText, text, 63);
    s_lastMsgText[63] = '\0';
  }
  s_msgScrollStart = 0;
  if (contentTabAtIndex(s_currentScreen) == display_tabs::CT_MSG) s_needRedrawMsg = true;
}

void displayShowScreen(int screen) {
  int nTabs = tabCountForUi();
  int next = (screen < nTabs) ? screen : (nTabs - 1);
  int prev = s_currentScreen;
  if (!ui_nav_mode::isTabMode() && next == 0 && prev != 0) {
    s_homeMenuIndex = 0;
    s_homeMenuScrollOff = 0;
  }
  if (!ui_nav_mode::isTabMode() && next == 0 && s_homeMenuIndex >= display_tabs::homeMenuCount()) s_homeMenuIndex = 0;
  if (next != prev && contentTabAtIndex(next) == display_tabs::CT_NET) s_netMenuIndex = 0;
  const bool sysToSys = (next != prev) && contentTabAtIndex(prev) == display_tabs::CT_SYS &&
      contentTabAtIndex(next) == display_tabs::CT_SYS;
  if (next != prev && contentTabAtIndex(next) == display_tabs::CT_SYS && !sysToSys) {
    s_sysMenuIndex = 0;
    s_sysScrollOff = 0;
  }
  s_currentScreen = next;
  if (next != prev && sysToSys && !s_sysInDisplaySubmenu && s_sysMenuIndex == 6) {
    s_sysMenuIndex = 0;
    s_sysScrollOff = 0;
  }
  if (next != prev) {
    s_tpagerMainScrollPx = 0;
    if (!sysToSys) {
      s_tabDrillIn = false;
      s_sysInDisplaySubmenu = false;
    }
    s_powerMenuIndex = 0;
    s_gpsMenuIndex = 0;
  }
  s_lastActivityTime = millis();
  drawScreen(s_currentScreen);
}

void displayShowScreenForceFull(int screen) {
  displayShowScreen(screen);
}

int displayGetCurrentScreen() { return s_currentScreen; }

static void hook_modem_tp() { displayShowModemPicker(); }
static void hook_scan_tp() { displayRunModemScan(); }
static void hook_region_tp() { displayShowRegionPicker(); }
static void hook_lang_tp() { displayShowLanguagePicker(); }
static void hook_selftest_tp() { selftest::run(nullptr); }

static const UiDisplayHooks s_ui_hooks_tp = {hook_modem_tp, hook_scan_tp, hook_region_tp, hook_lang_tp, hook_selftest_tp};

int displayHandleShortPress() {
  if (!ui_nav_mode::isTabMode() && display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_HOME) {
    s_homeMenuIndex = (s_homeMenuIndex + 1) % display_tabs::homeMenuCount();
    return s_currentScreen;
  }
  if (ui_nav_mode::isTabMode()) {
    s_currentScreen = display_tabs::clampNavTabIndex(s_currentScreen);
    display_tabs::ContentTab ct = contentTabAtIndex(s_currentScreen);
    if (ct == display_tabs::CT_SYS && s_sysInDisplaySubmenu) {
      s_sysMenuIndex = (s_sysMenuIndex + 1) % sysDisplaySubMenuCountTp();
      return s_currentScreen;
    }
    if (!s_tabDrillIn) {
      const int n = display_tabs::getNavTabCount();
      s_currentScreen = (s_currentScreen + 1) % n;
      return s_currentScreen;
    }
    if (ct == display_tabs::CT_NET) {
      s_netMenuIndex = (s_netMenuIndex + 1) % 2;
      return s_currentScreen;
    }
    if (ct == display_tabs::CT_GPS && s_tabDrillIn) {
      if (gps::isPresent()) {
        s_gpsMenuIndex = (s_gpsMenuIndex + 1) % 2;
      }
      return s_currentScreen;
    }
    if (ct == display_tabs::CT_SYS) {
      s_sysMenuIndex = (s_sysMenuIndex + 1) % 7;
      return s_currentScreen;
    }
    if (ct == display_tabs::CT_POWER && s_tabDrillIn) {
      if (s_powerMenuIndex == 2) {
        s_tabDrillIn = false;
        s_powerMenuIndex = 0;
        return s_currentScreen;
      }
      s_powerMenuIndex = (s_powerMenuIndex + 1) % 3;
      return s_currentScreen;
    }
    return s_currentScreen;
  }
  if (display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_NET) {
    s_netMenuIndex = (s_netMenuIndex + 1) % 2;
    return s_currentScreen;
  }
  if (display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_SYS) {
    if (s_sysInDisplaySubmenu) {
      s_sysMenuIndex = (s_sysMenuIndex + 1) % sysDisplaySubMenuCountTp();
    } else {
      s_sysMenuIndex = (s_sysMenuIndex + 1) % 7;
    }
    return s_currentScreen;
  }
  if (display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_GPS) {
    if (gps::isPresent()) {
      s_gpsMenuIndex = (s_gpsMenuIndex + 1) % 2;
    }
    return s_currentScreen;
  }
  if (display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_MAIN ||
      display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_MSG ||
      display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_INFO) {
    return s_currentScreen;
  }
  s_homeMenuIndex = 0;
  s_currentScreen = 0;
  return s_currentScreen;
}

static int displayShowPopupMenu(const char* title, const char* items[], int count, int initialSel, int mode,
    const uint8_t* const* rowIcons, bool lastRowNoIcon, bool noRowIcons) {
  (void)mode;
  if (!s_gfxReady || count <= 0) return -1;
  delay(200);
  prepareTabLayoutShiftTpager();
  if (initialSel < 0) initialSel = 0;
  if (initialSel >= count) initialSel = count - 1;
  int selected = initialSel;
  int scrollOff = 0;
  const bool hasTitle = (title && title[0]);
  const int menuY0 = submenuListY0Tpager(hasTitle);
  const int rowStep = kProfTpager.menuListRowHeight;
  const int txOff = MENU_LIST_TEXT_OFF_TP;
  const int icOff = MENU_LIST_ICON_OFF_TP;
  const int maxVisible = (SCREEN_HEIGHT - 18 - menuY0) / rowStep;
  const int showMax = (maxVisible >= 1) ? maxVisible : 1;

  while (1) {
    s_lastActivityTime = millis();
    ui_scroll::syncListWindow(selected, count, showMax, scrollOff);

    syncDraw([&]() {
      prepareTabLayoutShiftTpager();
      drawChromeTabsOrIdleRowTpager();
      if (hasTitle) drawSubmenuTitleBarTpager(title);
      gfx.setTextSize((uint8_t)ui_typography::bodyTextSizeTpager());
      int show = count - scrollOff;
      if (show > showMax) show = showMax;
      for (int i = 0; i < show; i++) {
        int idx = scrollOff + i;
        int y = menuY0 + i * rowStep;
        const bool noIcon = noRowIcons || (lastRowNoIcon && (idx == count - 1));
        if (noIcon) {
          if (idx == selected) {
            gfx.fillRect(MENU_SEL_X, y, MENU_SEL_W, rowStep, COL_FG);
            gfx.setTextColor(COL_BG, COL_FG);
          } else {
            gfx.setTextColor(COL_FG, COL_BG);
          }
          drawTruncRaw(16, y + txOff, items[idx], MAX_LINE_CHARS - 2);
          continue;
        }
        const uint8_t* bullet = rowIcons ? rowIcons[idx] : display_tabs::ICON_MAIN;
        if (idx == selected) {
          gfx.fillRect(MENU_SEL_X, y, MENU_SEL_W, rowStep, COL_FG);
          gfx.setTextColor(COL_BG, COL_FG);
          gfx.drawBitmap(16, y + icOff, bullet, ICON_W, ICON_H, COL_BG);
        } else {
          gfx.setTextColor(COL_FG, COL_BG);
          gfx.drawBitmap(16, y + icOff, bullet, ICON_W, ICON_H, COL_FG);
        }
        drawTruncRaw(16 + ICON_W + 12, y + txOff, items[idx], MAX_LINE_CHARS - 4);
      }
      gfx.setTextColor(COL_FG, COL_BG);
      if (scrollOff > 0)
        gfx.fillTriangle(SCREEN_WIDTH - 10, menuY0 - 2, SCREEN_WIDTH - 18, menuY0 + 6, SCREEN_WIDTH - 2, menuY0 + 6, COL_FG);
      if (scrollOff + showMax < count)
        gfx.fillTriangle(SCREEN_WIDTH - 10, SCREEN_HEIGHT - 22, SCREEN_WIDTH - 18, SCREEN_HEIGHT - 30, SCREEN_WIDTH - 2, SCREEN_HEIGHT - 30, COL_FG);
    });

    for (;;) {
      int pt = waitButtonPressWithType(60000);
      if (pt == PRESS_SHORT) {
        selected = (selected + 1) % count;
        break;
      }
      if (pt == PRESS_LONG) return selected;
    }
  }
}

/** Меню питания: как popup без заголовка, по центру по вертикали; только SHORT/LONG, без таймаута. */
static int displayShowCenteredPowerMenuTpager() {
  const char* items[] = {
    locale::getForDisplay("menu_power_off"),
    locale::getForDisplay("menu_power_sleep"),
    locale::getForDisplay("menu_back"),
  };
  const int count = 3;
  const int txOff = MENU_LIST_TEXT_OFF_TP;
  prepareTabLayoutShiftTpager();
  int rowStep;
  int menuY0;
  if (ui_nav_mode::isTabMode()) {
    rowStep = LINE_H;
    const int listY0Raw = submenuListY0Tpager(false) + NODE_MAIN_LIST_TOP_PAD_PX;
    const int minY = NODE_MAIN_MIN_BASELINE_Y + s_layoutTabShiftY;
    menuY0 = (listY0Raw < minY) ? minY : listY0Raw;
  } else {
    rowStep = kProfTpager.menuListRowHeight;
    const int totalH = count * rowStep;
    menuY0 = (SCREEN_HEIGHT - totalH) / 2;
    const int minY = CONTENT_Y + 4 + s_layoutTabShiftY;
    if (menuY0 < minY) menuY0 = minY;
  }
  delay(200);
  int selected = 0;
  if (ui_nav_mode::isTabMode() && s_powerMenuIndex >= 0 && s_powerMenuIndex < count) selected = s_powerMenuIndex;

  for (;;) {
    syncDraw([&]() {
      prepareTabLayoutShiftTpager();
      if (ui_nav_mode::isTabMode()) {
        const int listY0Raw = submenuListY0Tpager(false) + NODE_MAIN_LIST_TOP_PAD_PX;
        const int minY = NODE_MAIN_MIN_BASELINE_Y + s_layoutTabShiftY;
        menuY0 = (listY0Raw < minY) ? minY : listY0Raw;
      }
      drawChromeTabsOrIdleRowTpager();
      gfx.setTextSize((uint8_t)ui_typography::bodyTextSizeTpager());
      for (int i = 0; i < count; i++) {
        int y = menuY0 + i * rowStep;
        if (i == selected) {
          gfx.fillRect(MENU_SEL_X, y, MENU_SEL_W, rowStep, COL_FG);
          gfx.setTextColor(COL_BG, COL_FG);
        } else {
          gfx.setTextColor(COL_FG, COL_BG);
        }
        drawTruncRaw(16, y + txOff, items[i], MAX_LINE_CHARS - 2);
      }
      gfx.setTextColor(COL_FG, COL_BG);
    });

    for (;;) {
      int pt = waitButtonPressWithType(60000);
      if (pt == PRESS_SHORT) {
        selected = (selected + 1) % count;
        break;
      }
      if (pt == PRESS_LONG) return selected;
    }
  }
}

static void displayShowPowerMenu() {
  int sel = displayShowCenteredPowerMenuTpager();
  if (sel == 0) powersave::deepSleep();
  else if (sel == 1) displaySleep();
}

void displayOnLongPress(int screen) {
  (void)screen;
  s_lastActivityTime = millis();
  s_menuActive = true;
  const int tabForCt =
      ui_nav_mode::isTabMode() ? display_tabs::clampNavTabIndex(s_currentScreen) : s_currentScreen;
  display_tabs::ContentTab ct = contentTabAtIndex(tabForCt);

  if (ct == display_tabs::CT_HOME) {
    if (display_tabs::homeMenuIsPowerSlot(s_homeMenuIndex)) {
      displayShowPowerMenu();
    } else {
      s_currentScreen = display_tabs::homeMenuTargetScreen(s_homeMenuIndex);
    }
    s_menuActive = false;
    drawScreen(s_currentScreen);
    return;
  }

  if (ui_nav_mode::isTabMode()) {
    if (ct == display_tabs::CT_GPS && s_tabDrillIn) {
      if (!gps::isPresent() || s_gpsMenuIndex == 1) {
        s_tabDrillIn = false;
        s_gpsMenuIndex = 0;
      } else {
        ui_menu_exec::exec_gps_menu(0);
      }
      s_menuActive = false;
      drawScreen(s_currentScreen);
      return;
    }
    if (s_tabDrillIn && (ct == display_tabs::CT_MAIN || ct == display_tabs::CT_MSG || ct == display_tabs::CT_INFO)) {
      s_tabDrillIn = false;
      s_menuActive = false;
      drawScreen(s_currentScreen);
      return;
    }
    if (!s_tabDrillIn) {
      if (ct == display_tabs::CT_MAIN) {
        s_menuActive = false;
        return;
      }
      s_tabDrillIn = true;
      if (ct == display_tabs::CT_NET) s_netMenuIndex = 0;
      if (ct == display_tabs::CT_SYS) {
        s_sysMenuIndex = 0;
        s_sysScrollOff = 0;
        s_sysInDisplaySubmenu = false;
      }
      if (ct == display_tabs::CT_POWER) s_powerMenuIndex = 0;
      if (ct == display_tabs::CT_GPS) s_gpsMenuIndex = 0;
      s_menuActive = false;
      drawScreen(s_currentScreen);
      return;
    }
    if (ct == display_tabs::CT_POWER && s_tabDrillIn) {
      if (s_powerMenuIndex == 2) {
        s_tabDrillIn = false;
        s_powerMenuIndex = 0;
        s_menuActive = false;
        drawScreen(s_currentScreen);
        return;
      }
      displayShowPowerMenu();
      s_menuActive = false;
      drawScreen(s_currentScreen);
      return;
    }
  }

  if (ct == display_tabs::CT_MAIN) {
    s_homeMenuIndex = 0;
    s_currentScreen = 0;
    s_tpagerMainScrollPx = 0;
    s_menuActive = false;
    drawScreen(s_currentScreen);
    return;
  }

  if (ct == display_tabs::CT_MSG || ct == display_tabs::CT_INFO) {
    s_homeMenuIndex = 0;
    s_currentScreen = 0;
    s_msgScrollStart = 0;
    s_tpagerMainScrollPx = 0;
    s_menuActive = false;
    drawScreen(s_currentScreen);
    return;
  }

  if (ct == display_tabs::CT_NET) {
    if (s_netMenuIndex == 0) {
      ui_menu_exec::exec_net_menu(0);
    } else if (ui_nav_mode::isTabMode()) {
      s_netMenuIndex = 0;
      s_msgScrollStart = 0;
      s_tpagerMainScrollPx = 0;
      s_tabDrillIn = false;
    } else {
      s_homeMenuIndex = display_tabs::homeMenuSlotForContent(display_tabs::CT_NET);
      s_currentScreen = 0;
      s_msgScrollStart = 0;
      s_tpagerMainScrollPx = 0;
    }
    s_menuActive = false;
    drawScreen(s_currentScreen);
    return;
  }

  if (ct == display_tabs::CT_SYS) {
    if (s_sysInDisplaySubmenu) {
      const int bi = sysDisplaySubBackIdxTp();
      if (s_sysMenuIndex == bi) {
        s_sysInDisplaySubmenu = false;
        s_sysMenuIndex = 0;
        s_sysScrollOff = 0;
      } else if (s_sysMenuIndex == 0) {
        s_ui_hooks_tp.show_language_picker();
      } else if (s_sysMenuIndex == 1) {
        ui_display_prefs::setFlip180(!ui_display_prefs::getFlip180());
        displayApplyRotationFromPrefs();
      } else if (s_sysMenuIndex == 2) {
        const bool wasTabs = ui_nav_mode::isTabMode();
        const display_tabs::ContentTab styleAnchor = display_tabs::CT_SYS;
        ui_nav_mode::setTabMode(!wasTabs);
        if (wasTabs) {
          s_currentScreen = display_tabs::listTabIndexForContent(styleAnchor);
          s_homeMenuIndex = display_tabs::homeMenuSlotForContent(styleAnchor);
        } else {
          s_currentScreen = display_tabs::navTabIndexForContent(styleAnchor);
        }
        clampSysDisplaySubMenuIndexTp();
        if (!wasTabs && s_sysMenuIndex == 3) s_sysMenuIndex = 4;
        if (ui_nav_mode::isTabMode() && s_sysInDisplaySubmenu) {
          s_tabDrillIn = true;
        }
      } else if (s_sysMenuIndex == 3 && ui_nav_mode::isTabMode()) {
        ui_display_prefs::cycleTabBarHideIdleSeconds();
      }
      s_menuActive = false;
      drawScreen(s_currentScreen);
      return;
    }
    if (s_sysMenuIndex == 6) {
      if (ui_nav_mode::isTabMode()) {
        s_sysMenuIndex = 0;
        s_sysScrollOff = 0;
        s_msgScrollStart = 0;
        s_tpagerMainScrollPx = 0;
        s_tabDrillIn = false;
        s_sysInDisplaySubmenu = false;
      } else {
        s_homeMenuIndex = display_tabs::homeMenuSlotForContent(display_tabs::CT_SYS);
        s_currentScreen = 0;
        s_msgScrollStart = 0;
        s_tpagerMainScrollPx = 0;
        s_sysInDisplaySubmenu = false;
      }
    } else if (s_sysMenuIndex == 0) {
      s_sysInDisplaySubmenu = true;
      s_sysMenuIndex = 0;
      s_sysScrollOff = 0;
    } else {
      const int e = sysMainMenuIndexToExecSelTp(s_sysMenuIndex);
      if (e >= 0) ui_menu_exec::exec_sys_menu(e, s_ui_hooks_tp);
    }
    s_menuActive = false;
    drawScreen(s_currentScreen);
    return;
  }

  if (ct == display_tabs::CT_GPS) {
    if (!gps::isPresent() || s_gpsMenuIndex == 1) {
      s_homeMenuIndex = display_tabs::homeMenuSlotForContent(display_tabs::CT_GPS);
      s_currentScreen = 0;
      s_gpsMenuIndex = 0;
    } else {
      ui_menu_exec::exec_gps_menu(0);
    }
    s_menuActive = false;
    drawScreen(s_currentScreen);
    return;
  }

  s_menuActive = false;
  drawScreen(s_currentScreen);
}

void displaySetButtonPolledExternally(bool on) { s_buttonPolledExternally = on; }

void displayRequestInfoRedraw() {
  displayWakeRequest();
  s_needRedrawInfo = true;
}

void displayShowWarning(const char* line1, const char* line2, uint32_t durationMs) {
  if (!s_gfxReady) return;
  syncDraw([&]() {
    prepareTabLayoutShiftTpager();
    drawChromeTabsOrIdleRowTpager();
    drawSubmenuTitleBarTpager(locale::getForDisplay("warn_title"));
    gfx.setTextColor(COL_FG, COL_BG);
    const int listY = submenuListY0Tpager(true);
    if (line1) drawTruncRaw(8, listY, line1, MAX_LINE_CHARS);
    if (line2) drawTruncRaw(8, listY + LINE_H, line2, MAX_LINE_CHARS);
    drawSubmenuFooterSepTpager();
  });
  uint32_t start = millis();
  while (millis() - start < durationMs) {
    delay(50);
    yield();
  }
}

void displayShowSelftestSummary(bool radioOk, bool antennaOk, uint16_t batteryMv, uint32_t heapFree) {
  if (!s_gfxReady) return;
  char l0[32], l1[32], l2[32], l3[32];
  snprintf(l0, sizeof(l0), "Rf: %s", radioOk ? "OK" : "FAIL");
  snprintf(l1, sizeof(l1), "Ant: %s", antennaOk ? "OK" : "WARN");
  snprintf(l2, sizeof(l2), "Bat: %umV", (unsigned)batteryMv);
  snprintf(l3, sizeof(l3), "Heap: %uK", (unsigned)(heapFree / 1024));
  const char* items[] = {
    l0, l1, l2, l3,
    locale::getForDisplay("menu_back"),
  };
  for (;;) {
    int sel = displayShowPopupMenu("", items, 5, 4, POPUP_MODE_CANCEL, nullptr, true, true);
    if (sel < 0 || sel == 4) return;
  }
}

bool displayUpdate() {
  if (!s_gfxReady) return false;

  uint32_t now = millis();
  static bool s_prevTabStripTpager = true;
  ui_tab_bar_idle::tick(s_tabDrillIn);
  if (ui_nav_mode::isTabMode()) {
    const bool sh = ui_tab_bar_idle::tabStripVisible();
    if (sh != s_prevTabStripTpager) {
      s_prevTabStripTpager = sh;
      drawScreen(s_currentScreen);
      return true;
    }
  } else {
    s_prevTabStripTpager = true;
  }

  if (s_wakeRequested && s_displaySleeping) {
    s_wakeRequested = false;
    displayWake();
    drawScreen(s_currentScreen);
  }

  if (s_displaySleeping) {
    if (!s_buttonPolledExternally) {
      bool btn = BTN_PRESSED;
      if (btn) {
        if (!s_lastButton) { s_lastButton = true; s_pressStart = now; }
      } else if (s_lastButton && (now - s_pressStart) >= MIN_PRESS_MS) {
        s_lastButton = false;
        displayWake();
        drawScreen(s_currentScreen);
        return true;
      } else if (s_lastButton) s_lastButton = false;
    }
    return false;
  }

  if (s_needRedrawInfo) {
    s_needRedrawInfo = false;
    s_lastActivityTime = now;
    drawScreen(s_currentScreen);
  }
  if (s_needRedrawMsg && contentTabAtIndex(s_currentScreen) == display_tabs::CT_MSG) {
    s_needRedrawMsg = false;
    s_lastActivityTime = now;
    drawScreen(s_currentScreen);
  }

  if (!s_menuActive && !s_showingBootScreen && !s_buttonPolledExternally) {
    int32_t acc = s_encAccum.exchange(0);
    if (acc != 0) {
      int steps = 0;
      if (acc >= 4) steps = (int)(acc / 4);
      else if (acc <= -4) steps = -(int)((-acc) / 4);
      else if (acc > 0) steps = 1;
      else steps = -1;
      if (steps != 0) {
        if (ui_nav_mode::isTabMode()) {
          s_currentScreen = display_tabs::clampNavTabIndex(s_currentScreen);
          display_tabs::ContentTab ct = contentTabAtIndex(s_currentScreen);
          const bool innerNav = s_tabDrillIn || (ct == display_tabs::CT_SYS && s_sysInDisplaySubmenu);
          if (!innerNav) {
            const int n = display_tabs::getNavTabCount();
            int cur = display_tabs::clampNavTabIndex(s_currentScreen);
            int s = steps;
            while (s > 0) {
              cur = (cur + 1) % n;
              s--;
            }
            while (s < 0) {
              cur = (cur - 1 + n) % n;
              s++;
            }
            s_currentScreen = display_tabs::clampNavTabIndex(cur);
          } else {
            int s = steps;
            if (ct == display_tabs::CT_NET) {
              while (s > 0) {
                s_netMenuIndex = (s_netMenuIndex + 1) % 2;
                s--;
              }
              while (s < 0) {
                s_netMenuIndex = (s_netMenuIndex - 1 + 2) % 2;
                s++;
              }
            } else if (ct == display_tabs::CT_SYS) {
              const int mod = s_sysInDisplaySubmenu ? sysDisplaySubMenuCountTp() : 7;
              while (s > 0) {
                s_sysMenuIndex = (s_sysMenuIndex + 1) % mod;
                s--;
              }
              while (s < 0) {
                s_sysMenuIndex = (s_sysMenuIndex - 1 + mod) % mod;
                s++;
              }
            } else if (ct == display_tabs::CT_POWER) {
              while (s > 0) {
                s_powerMenuIndex = (s_powerMenuIndex + 1) % 3;
                s--;
              }
              while (s < 0) {
                s_powerMenuIndex = (s_powerMenuIndex - 1 + 3) % 3;
                s++;
              }
            } else if (ct == display_tabs::CT_GPS && s_tabDrillIn && gps::isPresent()) {
              while (s > 0) {
                s_gpsMenuIndex = (s_gpsMenuIndex + 1) % 2;
                s--;
              }
              while (s < 0) {
                s_gpsMenuIndex = (s_gpsMenuIndex - 1 + 2) % 2;
                s++;
              }
            }
          }
        } else if (display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_HOME) {
          int s = steps;
          while (s > 0) {
            s_homeMenuIndex = (s_homeMenuIndex + 1) % display_tabs::homeMenuCount();
            s--;
          }
          while (s < 0) {
            s_homeMenuIndex = (s_homeMenuIndex - 1 + display_tabs::homeMenuCount()) % display_tabs::homeMenuCount();
            s++;
          }
        } else if (display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_NET) {
          int s = steps;
          while (s > 0) {
            s_netMenuIndex = (s_netMenuIndex + 1) % 2;
            s--;
          }
          while (s < 0) {
            s_netMenuIndex = (s_netMenuIndex - 1 + 2) % 2;
            s++;
          }
        } else if (display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_SYS) {
          int s = steps;
          const int mod = s_sysInDisplaySubmenu ? sysDisplaySubMenuCountTp() : 7;
          while (s > 0) {
            s_sysMenuIndex = (s_sysMenuIndex + 1) % mod;
            s--;
          }
          while (s < 0) {
            s_sysMenuIndex = (s_sysMenuIndex - 1 + mod) % mod;
            s++;
          }
        } else if (display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_MAIN ||
                   display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_MSG ||
                   display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_INFO) {
          s_lastActivityTime = now;
          return true;
        } else {
          displayHandleShortPress();
        }
        s_lastActivityTime = now;
        drawScreen(s_currentScreen);
        displayNotifyTabChromeActivity();
        return true;
      }
    }
  }

  if (!s_buttonPolledExternally) {
    bool btn = BTN_PRESSED;
    if (btn) {
      if (!s_lastButton) { s_lastButton = true; s_pressStart = now; }
    } else if (s_lastButton) {
      uint32_t hold = now - s_pressStart;
      bool isLong = (hold >= LONG_PRESS_MS);
      bool isShort = (hold >= MIN_PRESS_MS && !isLong);
      s_lastActivityTime = now;
      if (isShort) {
        displayHandleShortPress();
        drawScreen(s_currentScreen);
      } else if (isLong) {
        if (!ui_nav_mode::isTabMode() && display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_HOME) {
          if (display_tabs::homeMenuIsPowerSlot(s_homeMenuIndex)) {
            displayShowPowerMenu();
          } else {
            s_currentScreen = display_tabs::homeMenuTargetScreen(s_homeMenuIndex);
          }
          drawScreen(s_currentScreen);
        } else {
          displayOnLongPress(s_currentScreen);
        }
      }
      displayNotifyTabChromeActivity();
      s_lastButton = false;
      return true;
    }
  }

  if (!s_displaySleeping && (now - s_lastActivityTime) > DISPLAY_SLEEP_MS) {
    displaySleep();
    return false;
  }

  if (!s_showingBootScreen && (millis() - s_lastScreenUpdate > 2000)) {
    s_lastScreenUpdate = millis();
    display_tabs::ContentTab ct = contentTabAtIndex(s_currentScreen);
    if (ct == display_tabs::CT_HOME || ct == display_tabs::CT_MAIN || ct == display_tabs::CT_MSG || ct == display_tabs::CT_INFO ||
        ct == display_tabs::CT_NET || ct == display_tabs::CT_SYS || ct == display_tabs::CT_GPS || ct == display_tabs::CT_POWER)
      drawScreen(s_currentScreen);
  }
  return false;
}
