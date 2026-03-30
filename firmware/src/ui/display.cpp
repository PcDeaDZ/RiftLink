/**
 * RiftLink Display — OLED SSD1306
 * Конфигурация по Meshtastic variant.h:
 * V3/V4: SDA=17, SCL=18, RST=21, Vext=36 (active LOW = питание ON)
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
#include "bootscreen_oled.h"
#include "locale/locale.h"
#include "ui_nav_mode.h"
#include "ui_display_prefs.h"
#include "ui_tab_bar_idle.h"
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
#include <cstring>
#include <cstdio>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/TomThumb.h>
#include "utf8rus.h"
#include "cp1251_to_rusfont.h"
#include <Wire.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#if defined(ARDUINO_LILYGO_T_BEAM)
  #define SDA_PIN 21
  #define SCL_PIN 22
  #define OLED_RST -1     // T-Beam: нет аппаратного RST на OLED
  #define VEXT_PIN -1     // T-Beam: питание OLED через AXP2101, не GPIO
  #define VEXT_ON_LEVEL LOW
  #define BUTTON_PIN 38   // T-Beam V1.1: кнопка USER
#else
  #define SDA_PIN 17
  #define SCL_PIN 18
  #define OLED_RST 21
  #define VEXT_PIN 36
  #define VEXT_ON_LEVEL LOW
  #define BUTTON_PIN 0
#endif

#define CONTENT_Y 14
/** Полоска вкладок под статусом (режим «вкладки»). */
#define TAB_BAR_H_OLED 10
/** Нижняя линия разделителя под статусом (см. drawSubScreenChrome). */
#define OLED_STATUS_SEP_BOTTOM 13
/** Зазор от разделителя до базовой линии первой строки «Узел» (px). */
#define NODE_MAIN_MIN_BASELINE_BELOW_SEP 5
#define NODE_MAIN_MIN_BASELINE_Y (OLED_STATUS_SEP_BOTTOM + NODE_MAIN_MIN_BASELINE_BELOW_SEP)
/** Доп. отступ к CONTENT_Y (итог не ниже NODE_MAIN_MIN_BASELINE_Y). */
#define NODE_MAIN_LIST_TOP_PAD_PX 3
#define CONTENT_H 50   // до низа экрана (было 38, футер убран)
#define FOOTER_Y 56   // не используется
#define ICON_W 8
#define ICON_H 8
#define LINE_H 8
/** Межстрочный интервал на вкладках (Node / Msg / …); чуть выше, чем раньше (7), без смены размера шрифта */
#define SUBMENU_CONTENT_LINE_H 8
#define MAX_LINE_CHARS 20
#define CONTENT_X 5
/** Отступ над строкой «Назад» от блока соседей (px). */
#define NODE_BACK_ROW_GAP_PX 5
/** Зазор между иконкой пункта и подписью (px); было 4 — визуально «слипалось» */
#define ICON_LABEL_GAP_OLED 6
/** Шаг строк главного меню: чуть больше LINE_H, чтобы строки не слипались (6–7 пунктов — впритык по высоте экрана). */
#define HOME_MENU_ROW_STEP_OLED_7 8
#define HOME_MENU_ROW_STEP_OLED_6 9
#define HOME_MENU_ROW_STEP_OLED_5 11
/** Подменю/экраны разделов: полоса заголовка (как у popup) */
#define SUBMENU_TITLE_TOP_OLED 14
#define SUBMENU_TITLE_H_OLED 7
#define SUBMENU_LIST_Y0_OLED (SUBMENU_TITLE_TOP_OLED + SUBMENU_TITLE_H_OLED + 2)
/** Полоса выбора в списках (главное меню = popup): отступы и высота как у displayShowPopupMenu */
#define MENU_SEL_X 1
#define MENU_SEL_W (SCREEN_WIDTH - 2)
/** Подменю: только SHORT/LONG (без автозакрытия по таймауту). Режимы оставлены для совместимости вызовов. */
#define POPUP_MODE_CANCEL 0
#define POPUP_MODE_PICKER 1

static int displayShowPopupMenu(const char* title, const char* items[], int count, int initialSel = 0, int mode = POPUP_MODE_CANCEL,
    const uint8_t* const* rowIcons = nullptr, bool lastRowNoIcon = false, bool noRowIcons = false);

/* submenuListY0 в профиле; в режиме вкладок полоска иконок в зоне топбара, s_layoutTabShiftY = 0 */
static_assert(ui_layout::profileOled128x64().submenuListY0 == SUBMENU_LIST_Y0_OLED, "ui_layout vs OLED");
static_assert(ui_layout::profileOled128x64().menuListRowHeight == 9, "ui_layout menuListRowHeight vs OLED popup");
static_assert(ui_layout::profileOled128x64().submenuContentLineHeight == SUBMENU_CONTENT_LINE_H,
              "ui_layout submenuContentLineHeight vs OLED");

#define SHORT_PRESS_MS  350   // короткое: меню / назад на главный
#define LONG_PRESS_MS   500   // длинное = подтверждение / вход в режим модификации
#define MIN_PRESS_MS    50    // минимум удержания для short / игнор дребезга контакта
#define DISPLAY_SLEEP_MS 30000  // 30 с неактивности → слип

static Adafruit_SSD1306* disp = nullptr;
static int s_currentScreen = 0;
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
static bool s_showingBootScreen = false;  // не перезаписывать бутскрин displayUpdate()
static int s_homeMenuIndex = 0;
/** Прокрутка списка главного меню (режим список). */
static int s_homeMenuScrollOff = 0;
/** Вкладка «Режим» (OLED): короткое — сменить пункт, длинное — подтвердить выбор. */
static int s_netMenuIndex = 0;
/** Вкладка «Настройки»: 0 «Дисплей», 1 PS, 2 регион, 3 модем, 4 скан, 5 тест, 6 «Назад». */
static int s_sysMenuIndex = 0;
static int s_sysScrollOff = 0;
/** Подменю «Дисплей» в SYS (flip / стиль / автоскрытие вкладок при стиле «вкладки»). */
static bool s_sysInDisplaySubmenu = false;
/** Смещение в s_lastMsgText для прокрутки (вкладка Msg). */
static size_t s_msgScrollStart = 0;
/** Вертикальный скролл вкладки «Узел» (пиксели, логический верх контента). */
static int s_oledMainScrollPx = 0;
static int s_oledMainMaxScrollPx = 0;
/** Сдвиг контента вниз при режиме вкладок (полоска под статусом). */
static int s_layoutTabShiftY = 0;
/** Режим вкладок: short листает вкладки до long; после long — short листает внутри NET/SYS. */
static bool s_tabDrillIn = false;
/** Вкладка «Питание» в таб-режиме после drill: 0..2 — выбранная строка предпросмотра. */
static int s_powerMenuIndex = 0;

/** Текущий раздел: в режиме вкладок — без HOME, индекс 0 = MAIN. */
static inline display_tabs::ContentTab contentTabAtIndex(int screen) {
  return ui_nav_mode::isTabMode() ? display_tabs::contentForNavTab(screen) : display_tabs::contentForTab(screen);
}
static inline int tabCountForUi() {
  return ui_nav_mode::isTabMode() ? display_tabs::getNavTabCount() : display_tabs::getTabCount();
}

/** Печать CP1251: конвертация в кодировку glcdfont AdafruitGFXRusFonts */
static void drawTruncRaw(int x, int y, const char* s, int maxLen) {
  if (!disp) return;
  char buf[MAX_LINE_CHARS + 4];
  int i = 0;
  while (s[i] && i < maxLen && i < (int)sizeof(buf) - 1) {
    buf[i] = (char)cp1251_to_rusfont((unsigned char)s[i]);
    i++;
  }
  buf[i] = '\0';
  disp->setCursor(x, y);
  disp->print(buf);
}

/** Печать UTF-8: utf8rus→CP1251, затем в кодировку glcdfont */
static void drawTruncUtf8(int x, int y, const char* s, int maxLen) {
  if (!disp) return;
  char buf[MAX_LINE_CHARS + 4];
  const char* u = utf8rus(s);
  int i = 0;
  while (u[i] && i < maxLen && i < (int)sizeof(buf) - 1) {
    buf[i] = (char)cp1251_to_rusfont((unsigned char)u[i]);
    i++;
  }
  buf[i] = '\0';
  disp->setCursor(x, y);
  disp->print(buf);
}

/** Строка контента: line 0..4. useUtf8=true для nick/ssid/msg (могут быть UTF-8) */
static void drawContentLine(int line, const char* s, bool useUtf8 = false) {
  int y = SUBMENU_LIST_Y0_OLED + s_layoutTabShiftY + line * SUBMENU_CONTENT_LINE_H;
  if (useUtf8)
    drawTruncUtf8(CONTENT_X, y, s, MAX_LINE_CHARS);
  else
    drawTruncRaw(CONTENT_X, y, s, MAX_LINE_CHARS);
}

void displayApplyRotationFromPrefs() {
  if (!disp) return;
  disp->setRotation(ui_display_prefs::getFlip180() ? 2 : 0);
}

void displayInit() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);

#if VEXT_PIN >= 0
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, VEXT_ON_LEVEL);
  delay(150);
#endif

#if !defined(ARDUINO_LILYGO_T_BEAM)
  Wire.begin(SDA_PIN, SCL_PIN);
#endif
  disp = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RST);
  if (!disp->begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false)) {
    disp->begin(SSD1306_SWITCHCAPVCC, 0x3D, true, false);
  }
  disp->clearDisplay();
  disp->setTextColor(SSD1306_WHITE);
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizeOled());
  disp->cp437(true);  // без сдвига кодов >=176 — нужен для CP1251 кириллицы
  disp->display();
  s_lastActivityTime = millis();
  displayApplyRotationFromPrefs();
}

void displaySleep() {
  if (!disp || s_displaySleeping) return;
  disp->ssd1306_command(0xAE);  // SSD1306_DISPLAYOFF
  s_displaySleeping = true;
}

void displayWake() {
  if (!disp || !s_displaySleeping) return;
  disp->ssd1306_command(0xAF);  // SSD1306_DISPLAYON
  s_displaySleeping = false;
  s_lastActivityTime = millis();
}

void displayWakeRequest() {
  s_wakeRequested = true;
}

bool displayIsSleeping() {
  return s_displaySleeping;
}

bool displayIsMenuActive() {
  return s_menuActive;
}

void displayClear() {
  s_showingBootScreen = false;
  if (disp) disp->clearDisplay();
}

void displayText(int x, int y, const char* text) {
  if (!disp) return;
  char buf[64];
  int i = 0;
  while (text[i] && i < 63) {
    buf[i] = (char)cp1251_to_rusfont((unsigned char)text[i]);
    i++;
  }
  buf[i] = '\0';
  disp->setCursor(x, y);
  disp->print(buf);
}

void displayShow() {
  if (disp) disp->display();
}

void displaySetTextSize(uint8_t s) {
  if (disp) disp->setTextSize(s);
}

// Бут-скрин: логотип Rift Link из app_icon_source.png
void displayShowBootScreen() {
  if (!disp) return;
  s_showingBootScreen = true;
  disp->clearDisplay();
  disp->drawBitmap(0, 0, bootscreen_oled, BOOTSCREEN_OLED_W, BOOTSCREEN_OLED_H, SSD1306_WHITE);
  disp->setTextColor(SSD1306_WHITE);
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizeOled());
  char ver[16];
  snprintf(ver, sizeof(ver), "v%s", RIFTLINK_VERSION);
  disp->setCursor(2, SCREEN_HEIGHT - 8);
  disp->print(ver);
  disp->display();
}

// Кнопка: INPUT_PULLUP. На большинстве Heltec нажатие = GND (LOW).
// Если не работает — попробуйте инвертировать (HIGH вместо LOW).
#define BTN_ACTIVE_LOW  1
#define BTN_PRESSED  (digitalRead(BUTTON_PIN) == (BTN_ACTIVE_LOW ? LOW : HIGH))

enum PressType { PRESS_NONE = 0, PRESS_SHORT = 1, PRESS_LONG = 2 };

/** Ждёт нажатие+отпускание. Возвращает PRESS_SHORT, PRESS_LONG или PRESS_NONE (timeout). */
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
  if (!disp) return false;
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
  if (!disp) return false;
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
  if (!disp) return;
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

static constexpr int kBatBodyW = 19;
/** Чуть выше базового 8×8 глифа в топбаре (+1 px), один контур. */
static constexpr int kBatBodyH = 9;
static constexpr int kBatNubW = 2;
/** Корпус + ножка справа (процент внутри TomThumb; зарядка — молния). */
static constexpr int kBatteryIconBarW = kBatBodyW + kBatNubW;

/** Молния: геометрия 14×7; dy — вертикальное центрирование в kBatBodyH. */
static void drawBatteryChargingBoltOled(Adafruit_SSD1306* d, int x, int y) {
  const int ox = x + (kBatBodyW - 14) / 2 - 1;
  const int dy = (kBatBodyH - 7) / 2;
  const auto zig = [&](int dx) {
    d->drawLine(ox + 8 + dx, y + 1 + dy, ox + 6 + dx, y + 3 + dy, SSD1306_WHITE);
    d->drawLine(ox + 6 + dx, y + 3 + dy, ox + 9 + dx, y + 3 + dy, SSD1306_WHITE);
    d->drawLine(ox + 9 + dx, y + 3 + dy, ox + 7 + dx, y + 5 + dy, SSD1306_WHITE);
  };
  zig(0);
  zig(1);
}

/** Батарея: один контур; TomThumb по центру; зарядка — молния. */
static void drawBatteryIcon(int x, int y, int pct, bool charging) {
  if (!disp) return;
  disp->drawRect(x, y, kBatBodyW, kBatBodyH, SSD1306_WHITE);
  disp->fillRect(x + kBatBodyW, y + (kBatBodyH - 3) / 2, kBatNubW, 3, SSD1306_WHITE);
  if (charging) {
    drawBatteryChargingBoltOled(disp, x, y);
    return;
  }
  char b[8];
  if (pct >= 0) {
    snprintf(b, sizeof(b), "%d%%", pct);
  } else {
    snprintf(b, sizeof(b), "--");
  }
  disp->setFont(&TomThumb);
  disp->setTextSize(1);
  disp->setTextColor(SSD1306_WHITE);
  int16_t x1 = 0, y1 = 0;
  uint16_t tw = 0, th = 0;
  disp->getTextBounds(b, 0, 0, &x1, &y1, &tw, &th);
  const int16_t tx = x + (kBatBodyW - (int)tw) / 2 - x1;
  const int16_t ty = y + (kBatBodyH - (int)th) / 2 - y1;
  disp->setCursor(tx, ty);
  disp->print(b);
  disp->setFont(nullptr);
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizeOled());
}

static void drawSignalBarsColored(int x, int y, int barsCount, uint16_t fillCol, uint16_t lineCol) {
  if (!disp) return;
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;  // heights: 2,4,6,8
    int bx = x + i * 4;
    int by = y + 8 - h;
    if (i < barsCount)
      disp->fillRect(bx, by, 3, h, fillCol);
    else
      disp->drawRect(bx, by, 3, h, lineCol);
  }
}

/** Signal strength bars: 4 bars at x,y. barsCount 0-4 */
static void drawSignalBars(int x, int y, int barsCount) {
  drawSignalBarsColored(x, y, barsCount, SSD1306_WHITE, SSD1306_WHITE);
}

/** y0=0 — верхний топбар (в т.ч. замена полоски вкладок при автоскрытии). */
static void drawStatusBarCompactAt(int y0) {
  if (!disp) return;
  ui_topbar::Model tb;
  ui_topbar::fill(tb);
  char buf[32];
  const int pct = tb.batteryPercent;
  const bool chg = tb.charging;

  /* Левый край блока время + батарея (процент внутри иконки) — для центрирования региона и режима. */
  int rightClusterLeft = SCREEN_WIDTH - 2;
  if (tb.hasTime) {
    snprintf(buf, sizeof(buf), "%02d:%02d", tb.hour, tb.minute);
    rightClusterLeft -= (int)strlen(buf) * 6 + 4;
  }
  rightClusterLeft -= kBatteryIconBarW;

  drawSignalBars(2, y0 + 2, tb.signalBars);
  /* Полоса сигнала: x=2, 4×4 шаг + ширина столбца 3 → ~19px; отступ до центра. */
  const int leftBlockEnd = 22;
  char mid[40];
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
      drawTruncRaw(xMid, y0 + 3, mid, maxChars);
    }
  }

  /* Правый верх: время → батарея (процент внутри корпуса; при зарядке — молния без цифр) */
  int xRight = SCREEN_WIDTH - 2;
  if (tb.hasTime) {
    snprintf(buf, sizeof(buf), "%02d:%02d", tb.hour, tb.minute);
    const int tw = (int)strlen(buf) * 6;
    xRight -= tw;
    drawTruncRaw(xRight, y0 + 3, buf, 5);
    xRight -= 4;
  }
  xRight -= kBatteryIconBarW;
  drawBatteryIcon(xRight, y0 + 1, pct, chg);
}

static void drawStatusBarCompact() {
  drawStatusBarCompactAt(0);
}

/** drawUpperTopbar: false — только двойная линия под топбаром; статус рисуется отдельно (см. drawChromeTabsOrIdleRowOled). */
static void drawSubScreenChromeOled(bool drawUpperTopbar) {
  if (!disp) return;
  disp->clearDisplay();
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizeOled());
  disp->setTextColor(SSD1306_WHITE);
  if (drawUpperTopbar) drawStatusBarCompact();
  disp->drawLine(0, 11, SCREEN_WIDTH - 1, 11, SSD1306_WHITE);
  disp->drawLine(0, 13, SCREEN_WIDTH - 1, 13, SSD1306_WHITE);
}

static void drawSubScreenChrome() {
  drawSubScreenChromeOled(true);
}

/** Полоска вкладок в зоне топбара (заменяет статус при видимой полоске; y0 = 0). */
static void drawTabBarOled() {
  if (!disp) return;
  const int y0 = 0;
  const int h = TAB_BAR_H_OLED;
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
      disp->fillRect(x0, y0, w, h, SSD1306_WHITE);
      disp->drawBitmap(ix, iy, icon, ICON_W, ICON_H, SSD1306_BLACK);
      disp->drawFastHLine(x0, y0 + h - 1, w, SSD1306_BLACK);
    } else {
      disp->drawBitmap(ix, iy, icon, ICON_W, ICON_H, SSD1306_WHITE);
    }
  }
}

static void prepareTabLayoutShiftOled() {
  /* Вкладки занимают зону топбара, отдельной второй полосы под статусом нет. */
  s_layoutTabShiftY = 0;
}

static void drawChromeTabsOrIdleRowOled() {
  if (!ui_nav_mode::isTabMode()) {
    drawSubScreenChrome();
    return;
  }
  if (ui_tab_bar_idle::tabStripVisible()) {
    if (!disp) return;
    disp->clearDisplay();
    disp->setTextSize((uint8_t)ui_typography::bodyTextSizeOled());
    disp->setTextColor(SSD1306_WHITE);
    drawTabBarOled();
    disp->drawLine(0, 11, SCREEN_WIDTH - 1, 11, SSD1306_WHITE);
    disp->drawLine(0, 13, SCREEN_WIDTH - 1, 13, SSD1306_WHITE);
  } else {
    drawSubScreenChromeOled(false);
    drawStatusBarCompactAt(0);
  }
}

static void drawSubmenuTitleBarOled(const char* title) {
  if (!disp || !title || !title[0]) return;
  const int top = SUBMENU_TITLE_TOP_OLED + s_layoutTabShiftY;
  disp->fillRect(0, top, SCREEN_WIDTH, SUBMENU_TITLE_H_OLED, SSD1306_WHITE);
  disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  drawTruncRaw(4, top + 1, title, MAX_LINE_CHARS);
  disp->setTextColor(SSD1306_WHITE);
  disp->drawFastHLine(0, top + SUBMENU_TITLE_H_OLED, SCREEN_WIDTH, SSD1306_WHITE);
}

static void drawSubmenuFooterSepOled() {
  if (!disp) return;
  for (int x = 4; x < SCREEN_WIDTH - 4; x += 4)
    disp->drawPixel(x, SCREEN_HEIGHT - 12, SSD1306_WHITE);
}

void displayShowInitProgress(int doneCount, int totalSteps, const char* statusLine) {
  if (!disp) return;
  s_showingBootScreen = false;
  if (totalSteps < 1) totalSteps = 1;
  if (doneCount < 0) doneCount = 0;
  if (doneCount > totalSteps) doneCount = totalSteps;
  drawSubScreenChrome();
  disp->setTextSize(1);
  disp->setTextColor(SSD1306_WHITE);
  drawTruncRaw(CONTENT_X, 15, locale::getForDisplay("init_title"), MAX_LINE_CHARS);
  const int cy = 27;
  const int r = 3;
  const int n = totalSteps;
  const int pitch = n > 1 ? (SCREEN_WIDTH - 24) / (n - 1) : 0;
  const int pitchClamped = pitch < 14 ? 14 : (pitch > 26 ? 26 : pitch);
  const int startX = SCREEN_WIDTH / 2 - ((n - 1) * pitchClamped) / 2;
  for (int i = 0; i < n - 1; i++) {
    const int cx0 = startX + i * pitchClamped;
    const int cx1 = startX + (i + 1) * pitchClamped;
    disp->drawLine(cx0 + r, cy, cx1 - r, cy, SSD1306_WHITE);
  }
  for (int i = 0; i < n; i++) {
    const int cx = startX + i * pitchClamped;
    if (i < doneCount) {
      disp->fillCircle(cx, cy, r, SSD1306_WHITE);
    } else if (i == doneCount && doneCount < totalSteps) {
      disp->drawCircle(cx, cy, r, SSD1306_WHITE);
      disp->fillCircle(cx, cy, 1, SSD1306_WHITE);
    } else {
      disp->drawCircle(cx, cy, r, SSD1306_WHITE);
    }
  }
  if (statusLine && statusLine[0])
    drawTruncRaw(CONTENT_X, 37, statusLine, MAX_LINE_CHARS);
  drawTruncRaw(CONTENT_X, 49, locale::getForDisplay("init_hint"), MAX_LINE_CHARS);
  disp->display();
}

static void displayRunModemScan() {
  if (!disp) return;
  prepareTabLayoutShiftOled();
  const int scanListY0 = CONTENT_Y + s_layoutTabShiftY;
  drawChromeTabsOrIdleRowOled();
  disp->setTextColor(SSD1306_WHITE);
  drawTruncRaw(CONTENT_X, scanListY0, locale::getForDisplay("scanning"), MAX_LINE_CHARS);
  drawTruncRaw(CONTENT_X, scanListY0 + SUBMENU_CONTENT_LINE_H, "~36s ...", MAX_LINE_CHARS);
  disp->display();

  static selftest::ScanResult res[6];
  int found = selftest::modemScan(res, 6);
  s_lastActivityTime = millis();  // скан ~36 с блокирует UI; без сброса сработает DISPLAY_SLEEP_MS сразу после выхода

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

/** Главное меню: только список (chrome и полоска вкладок — в drawScreen). */
static void drawHomeMenu() {
  if (!disp) return;
  const int nItems = display_tabs::homeMenuCount();
  const int menuY0 = CONTENT_Y + s_layoutTabShiftY;
  const int rowStep = (nItems >= 7) ? HOME_MENU_ROW_STEP_OLED_7
                                    : ((nItems >= 6) ? HOME_MENU_ROW_STEP_OLED_6 : HOME_MENU_ROW_STEP_OLED_5);
  const int bottomPad = 2;
  int showMax = (SCREEN_HEIGHT - menuY0 - bottomPad) / rowStep;
  if (showMax < 1) showMax = 1;
  if (showMax > nItems) showMax = nItems;
  int scrollRef = s_homeMenuScrollOff;
  ui_scroll::syncListWindow(s_homeMenuIndex, nItems, showMax, scrollRef);
  s_homeMenuScrollOff = scrollRef;

  if (ui_scroll::canScrollUp(s_homeMenuScrollOff)) {
    disp->fillTriangle(SCREEN_WIDTH - 6, menuY0, SCREEN_WIDTH - 10, menuY0 + 4, SCREEN_WIDTH - 2, menuY0 + 4, SSD1306_WHITE);
  }
  for (int vis = 0; vis < showMax; vis++) {
    const int i = s_homeMenuScrollOff + vis;
    const uint8_t* icon = display_tabs::iconForContent(display_tabs::homeMenuContentAt(i));
    int y = menuY0 + vis * rowStep;
    const int innerOff = (rowStep > 8) ? (rowStep - 8) / 2 : 0;
    if (i == s_homeMenuIndex) {
      disp->fillRect(MENU_SEL_X, y, MENU_SEL_W, rowStep, SSD1306_WHITE);
      disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
      disp->drawBitmap(4, y + innerOff, icon, ICON_W, ICON_H, SSD1306_BLACK);
    } else {
      disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
      disp->drawBitmap(4, y + innerOff, icon, ICON_W, ICON_H, SSD1306_WHITE);
    }
    drawTruncRaw(4 + ICON_W + ICON_LABEL_GAP_OLED, y + innerOff, homeMenuLabelForSlot(i), MAX_LINE_CHARS - 5);
  }
  if (ui_scroll::canScrollDown(s_homeMenuScrollOff, nItems, showMax)) {
    const int triY = menuY0 + (showMax - 1) * rowStep + rowStep - 4;
    disp->fillTriangle(SCREEN_WIDTH - 6, triY, SCREEN_WIDTH - 10, triY - 4, SCREEN_WIDTH - 2, triY - 4, SSD1306_WHITE);
  }
  disp->setTextColor(SSD1306_WHITE);
}

static void drawContentMain() {
  if (!disp) return;
  char buf[40];
  const uint8_t nodeSz = (uint8_t)ui_typography::nodeTabTextSizeOled();
  disp->setTextSize(nodeSz);
  const int clkW = 5 * 6 * (int)nodeSz;

  /* Без полосы заголовка «Узел» — только статус-бар и контент */
  const int listY0Raw = CONTENT_Y + NODE_MAIN_LIST_TOP_PAD_PX + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < NODE_MAIN_MIN_BASELINE_Y) ? NODE_MAIN_MIN_BASELINE_Y : listY0Raw;
  const int lh = ui_typography::nodeMsgLineStepOled();
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
  const int sMaxChrome = listY0 - NODE_MAIN_MIN_BASELINE_Y;
  s_oledMainMaxScrollPx = ui_content_scroll::maxScrollForOverflow(totalH, viewportH);
  s_oledMainScrollPx = ui_content_scroll::clampScroll(s_oledMainScrollPx, s_oledMainMaxScrollPx);
  {
    int rowTop = ry[numRows - 1];
    int s_lo = rowTop + lh - viewportBottom;
    if (s_lo < 0) s_lo = 0;
    int s_hi = rowTop - contentTop;
    if (s_hi > s_oledMainMaxScrollPx) s_hi = s_oledMainMaxScrollPx;
    if (sMaxChrome >= 0 && s_hi > sMaxChrome) s_hi = sMaxChrome;
    if (s_lo <= s_hi) {
      if (s_oledMainScrollPx < s_lo) s_oledMainScrollPx = s_lo;
      else if (s_oledMainScrollPx > s_hi) s_oledMainScrollPx = s_hi;
    }
  }
  if (sMaxChrome >= 0 && s_oledMainScrollPx > sMaxChrome) s_oledMainScrollPx = sMaxChrome;
  const int s = s_oledMainScrollPx;

  if (nick[0]) {
    disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    drawTruncUtf8(CONTENT_X, listY0 - s, nick, MAX_LINE_CHARS - 2);
    if (gps::hasTime()) {
      char clk[6];
      snprintf(clk, sizeof(clk), "%02d:%02d", gps::getHour(), gps::getMinute());
      drawTruncRaw(SCREEN_WIDTH - clkW - 2, listY0 - s, clk, 5);
    }
    const int yId = listY0 + lh - s;
    disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    snprintf(buf, sizeof(buf), "%s %s", locale::getForDisplay("id"), idHex);
    drawTruncRaw(CONTENT_X, yId, buf, MAX_LINE_CHARS);
  } else {
    disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    drawTruncRaw(CONTENT_X, listY0 - s, idHex, MAX_LINE_CHARS);
    if (gps::hasTime()) {
      char clk[6];
      snprintf(clk, sizeof(clk), "%02d:%02d", gps::getHour(), gps::getMinute());
      drawTruncRaw(SCREEN_WIDTH - clkW - 2, listY0 - s, clk, 5);
    }
  }

  int n = neighbors::getCount();
  int avgRssi = neighbors::getAverageRssi();
  snprintf(buf, sizeof(buf), "%s: %d", locale::getForDisplay("neighbors"), n);
  const int yNbDraw = listY0 + lh * (nick[0] ? 2 : 1) - s;
  disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  drawTruncRaw(CONTENT_X, yNbDraw, buf, MAX_LINE_CHARS);
  if (n > 0) {
    drawSignalBars(SCREEN_WIDTH - 20, yNbDraw, ui_topbar::rssiToBars(avgRssi));
  }

  if (!tabNoBack) {
    const int yBack = ry[numRows - 1] - s;
    disp->fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, SSD1306_WHITE);
    disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    drawTruncUtf8(CONTENT_X, yBack, locale::getForDisplay("menu_back"), MAX_LINE_CHARS);
    disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  }

  disp->setTextSize((uint8_t)ui_typography::bodyTextSizeOled());
}

static void drawContentInfo() {
  if (!disp) return;
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizeOled());
  const int listY0Raw = CONTENT_Y + NODE_MAIN_LIST_TOP_PAD_PX + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < NODE_MAIN_MIN_BASELINE_Y) ? NODE_MAIN_MIN_BASELINE_Y : listY0Raw;
  const int peerLh = SUBMENU_CONTENT_LINE_H;
  const bool tabNoBack = ui_nav_mode::isTabMode();
  const int bottomPad = tabNoBack ? 2 : 6;
  const int yBack = SCREEN_HEIGHT - bottomPad - peerLh - 1;
  int maxNumLines = (yBack - listY0 - NODE_BACK_ROW_GAP_PX) / peerLh;
  if (maxNumLines < 1) maxNumLines = 1;

  char buf[28];
  int n = neighbors::getCount();
  disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
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
    int y = listY0 + (1 + i) * peerLh;
    drawTruncRaw(CONTENT_X, y, buf, 16);
    drawSignalBars(SCREEN_WIDTH - 20, y, ui_topbar::rssiToBars(rssi));
  }
  if (extraRow) {
    snprintf(buf, sizeof(buf), "+%d more", n - maxShow);
    drawTruncRaw(CONTENT_X, listY0 + (1 + maxShow) * peerLh, buf, MAX_LINE_CHARS);
  }

  if (!tabNoBack) {
    disp->fillRect(0, yBack - 1, SCREEN_WIDTH, peerLh + 1, SSD1306_WHITE);
    disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    drawTruncUtf8(CONTENT_X, yBack, locale::getForDisplay("menu_back"), MAX_LINE_CHARS);
    disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  }
}

static void drawContentNet() {
  if (!disp) return;
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizeOled());
  const int lh = SUBMENU_CONTENT_LINE_H;
  const int bottomPad = 6;
  const int gapMid = ui_nav_mode::isTabMode() ? 1 : 4;
  const int yBack = SCREEN_HEIGHT - bottomPad - lh - 1;
  const int listY0Raw = CONTENT_Y + NODE_MAIN_LIST_TOP_PAD_PX + s_layoutTabShiftY;
  const int yModeRow = (listY0Raw < NODE_MAIN_MIN_BASELINE_Y) ? NODE_MAIN_MIN_BASELINE_Y : listY0Raw;
  const int yStat0 = yModeRow + lh + gapMid;
  const bool haveMid = (yStat0 + 2 * lh <= yBack - 2);
  const int yStatCompact = yModeRow + lh + 1;
  const bool haveMidCompact = ui_nav_mode::isTabMode() && !haveMid && (yStatCompact + 2 * lh <= yBack - 2);
  const int yDrawStat = haveMid ? yStat0 : (haveMidCompact ? yStatCompact : yStat0);

  const bool isBle = (radio_mode::current() == radio_mode::BLE);
  char buf[40];
  const bool netSel = !ui_nav_mode::isTabMode() || s_tabDrillIn;

  disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);

  /* 1) Первая строка: только текст (локаль), без иконки. */
  const char* modeLine = locale::getForDisplay(isBle ? "net_mode_line_ble" : "net_mode_line_wifi");
  if (netSel && s_netMenuIndex == 0) {
    disp->fillRect(0, yModeRow - 1, SCREEN_WIDTH, lh + 1, SSD1306_WHITE);
    disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  } else {
    disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  }
  drawTruncRaw(CONTENT_X, yModeRow, modeLine, MAX_LINE_CHARS);
  disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);

  /* 2) Две строки: SSID/RL-… и IP/PIN */
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

  /* 3) Низ: «Назад» (в таб-режиме только после входа long — s_tabDrillIn) */
  if (!ui_nav_mode::isTabMode() || s_tabDrillIn) {
    if (netSel && s_netMenuIndex == 1) {
      disp->fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, SSD1306_WHITE);
      disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
      disp->fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, SSD1306_BLACK);
      disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    }
    drawTruncUtf8(CONTENT_X, yBack, locale::getForDisplay("menu_back"), MAX_LINE_CHARS);
    disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  }
}

static int sysDisplaySubMenuCount() {
  /* Язык, переворот экрана, стиль, [автоскрытие вкладок при табах], назад */
  return ui_nav_mode::isTabMode() ? 5 : 4;
}
static int sysDisplaySubBackIdx() { return sysDisplaySubMenuCount() - 1; }
static void clampSysDisplaySubMenuIndex() {
  int c = sysDisplaySubMenuCount();
  if (s_sysMenuIndex >= c) s_sysMenuIndex = c - 1;
}

static void displaySubFillLabelOled(int idx, char* buf, size_t bufSz) {
  const bool tabs = ui_nav_mode::isTabMode();
  const int backIdx = sysDisplaySubBackIdx();
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

static void sysMenuFillLabelOled(int idx, char* buf, size_t bufSz) {
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

/** Главное SYS: индекс строки → sel для exec_sys_menu (0 модем, 1 скан, 2 PS, 3 регион, 4 самотест). */
static int sysMainMenuIndexToExecSel(int idx) {
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
  if (!disp) return;
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizeOled());
  const int lh = SUBMENU_CONTENT_LINE_H;
  /** Шаг строки чуть больше lh: полоса выделения = rowStep без «дыр» между строками. */
  const int rowStep = lh + 1;
  const int bottomPad = 2;
  const int listY0Raw = CONTENT_Y + NODE_MAIN_LIST_TOP_PAD_PX + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < NODE_MAIN_MIN_BASELINE_Y) ? NODE_MAIN_MIN_BASELINE_Y : listY0Raw;
  int showMax = (SCREEN_HEIGHT - listY0 - bottomPad) / rowStep;
  if (showMax < 1) showMax = 1;

  char buf[36];
  disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);

  if (s_sysInDisplaySubmenu) {
    const int kItems = sysDisplaySubMenuCount();
    const int backIdx = sysDisplaySubBackIdx();
    if (showMax > kItems) showMax = kItems;
    int scrollRef = s_sysScrollOff;
    ui_scroll::syncListWindow(s_sysMenuIndex, kItems, showMax, scrollRef);
    s_sysScrollOff = scrollRef;
    if (s_sysScrollOff > 0) {
      disp->fillTriangle(SCREEN_WIDTH - 6, listY0, SCREEN_WIDTH - 10, listY0 + 4, SCREEN_WIDTH - 2, listY0 + 4, SSD1306_WHITE);
    }
    for (int vis = 0; vis < showMax; vis++) {
      int idx = s_sysScrollOff + vis;
      if (idx >= kItems) break;
      int y = listY0 + vis * rowStep;
      const bool rowSel = (s_sysMenuIndex == idx);
      if (idx == backIdx) {
        const char* backLbl = locale::getForDisplay("menu_back");
        if (rowSel) {
          disp->fillRect(0, y - 1, SCREEN_WIDTH, rowStep, SSD1306_WHITE);
          disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        } else {
          disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
        }
        drawTruncUtf8(CONTENT_X, y, backLbl, MAX_LINE_CHARS - 5);
      } else {
        displaySubFillLabelOled(idx, buf, sizeof(buf));
        if (rowSel) {
          disp->fillRect(0, y - 1, SCREEN_WIDTH, rowStep, SSD1306_WHITE);
          disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        } else {
          disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
        }
        /* «Перевёрнуть: Нет» — 16 символов после utf8rus; было MAX-5=15 → обрезало «т». */
        drawTruncUtf8(CONTENT_X, y, buf, MAX_LINE_CHARS - 3);
      }
    }
    disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    if (s_sysScrollOff + showMax < kItems) {
      const int triY = listY0 + (showMax - 1) * rowStep + rowStep - 4;
      disp->fillTriangle(SCREEN_WIDTH - 6, triY, SCREEN_WIDTH - 10, triY - 4, SCREEN_WIDTH - 2, triY - 4, SSD1306_WHITE);
    }
    return;
  }

  /** 0..5 — пункты, 6 «Назад». */
  constexpr int kSysListItems = 7;
  if (showMax > kSysListItems) showMax = kSysListItems;
  const bool sysTabBrowse = ui_nav_mode::isTabMode() && !s_tabDrillIn;
  const int sysListCount = sysTabBrowse ? (kSysListItems - 1) : kSysListItems;
  int scrollRef = sysTabBrowse ? 0 : s_sysScrollOff;
  ui_scroll::syncListWindow(sysTabBrowse ? 0 : s_sysMenuIndex, sysListCount, showMax, scrollRef);
  s_sysScrollOff = scrollRef;

  if (s_sysScrollOff > 0) {
    disp->fillTriangle(SCREEN_WIDTH - 6, listY0, SCREEN_WIDTH - 10, listY0 + 4, SCREEN_WIDTH - 2, listY0 + 4, SSD1306_WHITE);
  }
  for (int vis = 0; vis < showMax; vis++) {
    int idx = s_sysScrollOff + vis;
    if (idx >= sysListCount) break;
    int y = listY0 + vis * rowStep;
    const bool rowSel = !sysTabBrowse && (s_sysMenuIndex == idx);
    if (idx == 6) {
      const char* backLbl = locale::getForDisplay("menu_back");
      if (rowSel) {
        disp->fillRect(0, y - 1, SCREEN_WIDTH, rowStep, SSD1306_WHITE);
        disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
      } else {
        disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
      }
      drawTruncUtf8(CONTENT_X, y, backLbl, MAX_LINE_CHARS - 5);
    } else {
      sysMenuFillLabelOled(idx, buf, sizeof(buf));
      if (rowSel) {
        disp->fillRect(0, y - 1, SCREEN_WIDTH, rowStep, SSD1306_WHITE);
        disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        disp->drawBitmap(4, y, ui_icons::sysMenuIcon(idx), ICON_W, ICON_H, SSD1306_BLACK);
      } else {
        disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
        disp->drawBitmap(4, y, ui_icons::sysMenuIcon(idx), ICON_W, ICON_H, SSD1306_WHITE);
      }
      drawTruncRaw(4 + ICON_W + ICON_LABEL_GAP_OLED, y, buf, MAX_LINE_CHARS - 5);
    }
  }
  disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  if (s_sysScrollOff + showMax < sysListCount) {
    const int triY = listY0 + (showMax - 1) * rowStep + rowStep - 4;
    disp->fillTriangle(SCREEN_WIDTH - 6, triY, SCREEN_WIDTH - 10, triY - 4, SCREEN_WIDTH - 2, triY - 4, SSD1306_WHITE);
  }
}

static void drawContentMsg() {
  if (!disp) return;
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizeOled());
  const int listY0Raw = CONTENT_Y + NODE_MAIN_LIST_TOP_PAD_PX + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < NODE_MAIN_MIN_BASELINE_Y) ? NODE_MAIN_MIN_BASELINE_Y : listY0Raw;
  const int lh = SUBMENU_CONTENT_LINE_H;
  disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
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
    disp->fillTriangle(SCREEN_WIDTH - 6, listY0 + 1 * lh, SCREEN_WIDTH - 10, listY0 + 1 * lh + 4, SCREEN_WIDTH - 2, listY0 + 1 * lh + 4, SSD1306_WHITE);
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
    disp->fillTriangle(SCREEN_WIDTH - 6, listY0 + 3 * lh + lh - 4, SCREEN_WIDTH - 10, listY0 + 3 * lh - 4, SCREEN_WIDTH - 2, listY0 + 3 * lh - 4, SSD1306_WHITE);
  }

  /* Полоса «Назад» как на других экранах; в режиме вкладок убрана — см. drawContentInfo (tabNoBack). */
  if (!ui_nav_mode::isTabMode()) {
    const int yBack = listY0 + 4 * lh + NODE_BACK_ROW_GAP_PX;
    disp->fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, SSD1306_WHITE);
    disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    drawTruncUtf8(CONTENT_X, yBack, locale::getForDisplay("menu_back"), MAX_LINE_CHARS);
    disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  }
}

static void drawContentGps() {
  if (!disp) return;
  char buf[32];
  if (!gps::isPresent() && gps::hasPhoneSync()) {
    drawContentLine(0, locale::getForDisplay("gps_phone"));
    if (gps::hasTime()) {
      snprintf(buf, sizeof(buf), "%02d:%02d UTC", gps::getHour(), gps::getMinute());
      drawContentLine(1, buf);
    }
    return;
  }
  if (!gps::isPresent()) {
    drawContentLine(0, locale::getForDisplay("gps_not_present"));
    drawContentLine(1, "BLE: gps rx,tx,en");
    return;
  }
  drawContentLine(0, gps::isEnabled() ? locale::getForDisplay("gps_on") : locale::getForDisplay("gps_off"));
  drawContentLine(1, gps::hasFix() ? locale::getForDisplay("gps_fix") : locale::getForDisplay("gps_no_fix"));
  {
    char line2[40];
    line2[0] = '\0';
    if (gps::isEnabled()) {
      uint32_t sat = gps::getSatellites();
      float course = gps::getCourseDeg();
      const char* card = gps::getCourseCardinal();
      if (sat > 0 && course >= 0) snprintf(buf, sizeof(buf), "%u sat %0.0f %s", (unsigned)sat, course, card);
      else if (sat > 0) snprintf(buf, sizeof(buf), "%u sat", (unsigned)sat);
      else if (course >= 0) snprintf(buf, sizeof(buf), "%0.0f %s", course, card);
      else snprintf(buf, sizeof(buf), "-");
      if (gps::hasFix()) {
        snprintf(line2, sizeof(line2), "%s %.3f %.3f", buf, (double)gps::getLat(), (double)gps::getLon());
      } else {
        strncpy(line2, buf, sizeof(line2));
        line2[sizeof(line2) - 1] = '\0';
      }
    } else if (gps::hasFix()) {
      snprintf(line2, sizeof(line2), "%.4f %.4f", (double)gps::getLat(), (double)gps::getLon());
    }
    if (line2[0]) drawContentLine(2, line2);
  }
}

static void drawContentPower() {
  if (!disp) return;
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizeOled());
  const int listY0Raw = CONTENT_Y + NODE_MAIN_LIST_TOP_PAD_PX + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < NODE_MAIN_MIN_BASELINE_Y) ? NODE_MAIN_MIN_BASELINE_Y : listY0Raw;
  const int lh = SUBMENU_CONTENT_LINE_H;
  const int nLines = (!ui_nav_mode::isTabMode() || s_tabDrillIn) ? 3 : 2;
  const char* const lines[3] = {
    locale::getForDisplay("menu_power_off"),
    locale::getForDisplay("menu_power_sleep"),
    locale::getForDisplay("menu_back"),
  };
  for (int i = 0; i < nLines; i++) {
    const int y = listY0 + i * lh;
    const bool sel = ui_nav_mode::isTabMode() && s_tabDrillIn && (s_powerMenuIndex == i);
    if (sel) {
      disp->fillRect(0, y - 1, SCREEN_WIDTH, lh + 1, SSD1306_WHITE);
      disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
      disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    }
    drawTruncUtf8(CONTENT_X, y, lines[i], MAX_LINE_CHARS);
  }
}

static void drawScreen(int tab) {
  if (ui_nav_mode::isTabMode()) {
    tab = display_tabs::clampNavTabIndex(tab);
    s_currentScreen = tab;
  }
  prepareTabLayoutShiftOled();
  if (ui_nav_mode::isTabMode()) {
    drawChromeTabsOrIdleRowOled();
    display_tabs::ContentTab ct = display_tabs::contentForNavTab(tab);
    if (ct != display_tabs::CT_MAIN && ct != display_tabs::CT_MSG && ct != display_tabs::CT_INFO &&
        ct != display_tabs::CT_NET && ct != display_tabs::CT_SYS && ct != display_tabs::CT_POWER) {
      drawSubmenuTitleBarOled(ui_section::sectionTitleForContent(ct));
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
    disp->display();
    return;
  }
  if (display_tabs::contentForTab(tab) == display_tabs::CT_HOME) {
    drawSubScreenChrome();
    drawHomeMenu();
    disp->display();
    return;
  }
  drawSubScreenChrome();
  {
    display_tabs::ContentTab ct = display_tabs::contentForTab(tab);
    if (ct != display_tabs::CT_MAIN && ct != display_tabs::CT_MSG && ct != display_tabs::CT_INFO &&
        ct != display_tabs::CT_NET && ct != display_tabs::CT_SYS) {
      drawSubmenuTitleBarOled(ui_section::sectionTitleForContent(ct));
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
  disp->display();
}

void displaySetLastMsg(const char* fromHex, const char* text) {
  displayWakeRequest();
  s_lastActivityTime = millis();  // входящее сообщение — активность
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
  /* При sysToSys курсор сохранялся; не оставлять выделение на «Назад» (6) главного SYS. */
  if (next != prev && sysToSys && !s_sysInDisplaySubmenu && s_sysMenuIndex == 6) {
    s_sysMenuIndex = 0;
    s_sysScrollOff = 0;
  }
  if (next != prev) {
    s_oledMainScrollPx = 0;
    if (!sysToSys) {
      s_tabDrillIn = false;
      s_sysInDisplaySubmenu = false;
    }
    s_powerMenuIndex = 0;
  }
  s_lastActivityTime = millis();  // смена экрана (пикеры) — активность
  drawScreen(s_currentScreen);
}

void displayShowScreenForceFull(int screen) {
  displayShowScreen(screen);  // OLED — full/partial не различаются
}

int displayGetCurrentScreen() {
  return s_currentScreen;
}

int displayHandleShortPress() {
  if (!ui_nav_mode::isTabMode() && display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_HOME) {
    s_homeMenuIndex = (s_homeMenuIndex + 1) % display_tabs::homeMenuCount();
    return s_currentScreen;
  }
  if (ui_nav_mode::isTabMode()) {
    s_currentScreen = display_tabs::clampNavTabIndex(s_currentScreen);
    display_tabs::ContentTab ct = contentTabAtIndex(s_currentScreen);
    /* Подменю «Дисплей» при s_tabDrillIn=false: иначе short уходит в смену вкладок (SYS→Power). */
    if (ct == display_tabs::CT_SYS && s_sysInDisplaySubmenu) {
      s_sysMenuIndex = (s_sysMenuIndex + 1) % sysDisplaySubMenuCount();
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
    if (ct == display_tabs::CT_SYS) {
      /* Подменю «Дисплей» — только в ветке выше; здесь главное меню SYS после long. */
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
      s_sysMenuIndex = (s_sysMenuIndex + 1) % sysDisplaySubMenuCount();
    } else {
      s_sysMenuIndex = (s_sysMenuIndex + 1) % 7;
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

/** Подменю: заголовок, список как главное меню. Выход только по SHORT (листать) / LONG (подтвердить), без таймаута. */
static int displayShowPopupMenu(const char* title, const char* items[], int count, int initialSel, int mode,
    const uint8_t* const* rowIcons, bool lastRowNoIcon, bool noRowIcons) {
  (void)mode;
  if (!disp || count <= 0) return -1;
  delay(200);
  prepareTabLayoutShiftOled();
  if (initialSel < 0) initialSel = 0;
  if (initialSel >= count) initialSel = count - 1;
  int selected = initialSel;
  int scrollOff = 0;
  const bool hasTitle = (title && title[0]);
  const int menuY0 = hasTitle ? (SUBMENU_LIST_Y0_OLED + s_layoutTabShiftY) : (CONTENT_Y + s_layoutTabShiftY);
  const auto prof = ui_layout::profileOled128x64();
  const int rowStep = prof.menuListRowHeight;
  const int txOff = prof.menuListTextOffsetY;
  const int icOff = prof.menuListIconTopOffsetY;
  const int maxVisible = (SCREEN_HEIGHT - 9 - menuY0) / rowStep;
  const int showMax = (maxVisible >= 1) ? maxVisible : 1;

  while (1) {
    s_lastActivityTime = millis();
    ui_scroll::syncListWindow(selected, count, showMax, scrollOff);

    drawChromeTabsOrIdleRowOled();
    if (hasTitle) drawSubmenuTitleBarOled(title);
    disp->setTextSize((uint8_t)ui_typography::bodyTextSizeOled());
    int show = count - scrollOff;
    if (show > showMax) show = showMax;
    for (int i = 0; i < show; i++) {
      int idx = scrollOff + i;
      int y = menuY0 + i * rowStep;
      const bool noIcon = noRowIcons || (lastRowNoIcon && (idx == count - 1));
      if (noIcon) {
        if (idx == selected) {
          disp->fillRect(MENU_SEL_X, y, MENU_SEL_W, rowStep, SSD1306_WHITE);
          disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        } else {
          disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
        }
        drawTruncRaw(CONTENT_X, y + txOff, items[idx], MAX_LINE_CHARS - 2);
        continue;
      }
      const uint8_t* bullet = rowIcons ? rowIcons[idx] : display_tabs::ICON_MAIN;
      if (idx == selected) {
        disp->fillRect(MENU_SEL_X, y, MENU_SEL_W, rowStep, SSD1306_WHITE);
        disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        disp->drawBitmap(4, y + icOff, bullet, ICON_W, ICON_H, SSD1306_BLACK);
      } else {
        disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
        disp->drawBitmap(4, y + icOff, bullet, ICON_W, ICON_H, SSD1306_WHITE);
      }
      drawTruncRaw(4 + ICON_W + ICON_LABEL_GAP_OLED, y + txOff, items[idx], MAX_LINE_CHARS - 5);
    }
    disp->setTextColor(SSD1306_WHITE);
    if (scrollOff > 0)
      disp->fillTriangle(SCREEN_WIDTH - 6, menuY0, SCREEN_WIDTH - 10, menuY0 + 4, SCREEN_WIDTH - 2, menuY0 + 4, SSD1306_WHITE);
    if (scrollOff + showMax < count)
      disp->fillTriangle(SCREEN_WIDTH - 6, SCREEN_HEIGHT - 11, SCREEN_WIDTH - 10, SCREEN_HEIGHT - 15, SCREEN_WIDTH - 2, SCREEN_HEIGHT - 15, SSD1306_WHITE);
    disp->display();

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
static int displayShowCenteredPowerMenuOled() {
  const char* items[] = {
    locale::getForDisplay("menu_power_off"),
    locale::getForDisplay("menu_power_sleep"),
    locale::getForDisplay("menu_back"),
  };
  const int count = 3;
  prepareTabLayoutShiftOled();
  const auto prof = ui_layout::profileOled128x64();
  int rowStep;
  int menuY0;
  const int txOff = prof.menuListTextOffsetY;
  if (ui_nav_mode::isTabMode()) {
    /* Как drawContentPower: тот же верх и шаг строк, что у MAIN/MSG/NET — без скачка при открытии с вкладки */
    rowStep = SUBMENU_CONTENT_LINE_H;
    const int listY0Raw = CONTENT_Y + NODE_MAIN_LIST_TOP_PAD_PX + s_layoutTabShiftY;
    menuY0 = (listY0Raw < NODE_MAIN_MIN_BASELINE_Y) ? NODE_MAIN_MIN_BASELINE_Y : listY0Raw;
  } else {
    rowStep = prof.menuListRowHeight;
    const int totalH = count * rowStep;
    menuY0 = (SCREEN_HEIGHT - totalH) / 2;
    if (menuY0 < CONTENT_Y + s_layoutTabShiftY) menuY0 = CONTENT_Y + s_layoutTabShiftY;
  }
  delay(200);
  int selected = 0;
  if (ui_nav_mode::isTabMode() && s_powerMenuIndex >= 0 && s_powerMenuIndex < count) selected = s_powerMenuIndex;

  for (;;) {
    prepareTabLayoutShiftOled();
    if (ui_nav_mode::isTabMode()) {
      const int listY0Raw = CONTENT_Y + NODE_MAIN_LIST_TOP_PAD_PX + s_layoutTabShiftY;
      menuY0 = (listY0Raw < NODE_MAIN_MIN_BASELINE_Y) ? NODE_MAIN_MIN_BASELINE_Y : listY0Raw;
    }
    drawChromeTabsOrIdleRowOled();
    disp->setTextSize((uint8_t)ui_typography::bodyTextSizeOled());
    for (int i = 0; i < count; i++) {
      int y = menuY0 + i * rowStep;
      if (i == selected) {
        disp->fillRect(MENU_SEL_X, y, MENU_SEL_W, rowStep, SSD1306_WHITE);
        disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
      } else {
        disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
      }
      drawTruncRaw(CONTENT_X, y + txOff, items[i], MAX_LINE_CHARS - 2);
    }
    disp->setTextColor(SSD1306_WHITE);
    disp->display();

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
  int sel = displayShowCenteredPowerMenuOled();
  if (sel == 0) powersave::deepSleep();
  else if (sel == 1) displaySleep();
}

static void hook_modem() { displayShowModemPicker(); }
static void hook_scan() { displayRunModemScan(); }
static void hook_region() { displayShowRegionPicker(); }
static void hook_lang() { displayShowLanguagePicker(); }
static void hook_selftest() { selftest::run(nullptr); }

static const UiDisplayHooks s_ui_hooks = {hook_modem, hook_scan, hook_region, hook_lang, hook_selftest};

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
    if (ct == display_tabs::CT_GPS) {
      bool gpsOn = gps::isPresent() && gps::isEnabled();
      char gpsBuf[20];
      snprintf(gpsBuf, sizeof(gpsBuf), "GPS: %s", gpsOn ? "ON -> OFF" : "OFF -> ON");
      const char* items[] = {
        gpsBuf,
        locale::getForDisplay("menu_back")
      };
      const uint8_t* gIcons[2] = {ui_icons::gpsMenuIcon(0), ui_icons::gpsMenuIcon(1)};
      int sel = displayShowPopupMenu("", items, 2, 0, POPUP_MODE_CANCEL, gIcons, true, false);
      ui_menu_exec::exec_gps_menu(sel);
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
    s_oledMainScrollPx = 0;
    s_menuActive = false;
    drawScreen(s_currentScreen);
    return;
  }

  if (ct == display_tabs::CT_MSG || ct == display_tabs::CT_INFO) {
    s_homeMenuIndex = 0;
    s_currentScreen = 0;
    s_msgScrollStart = 0;
    s_oledMainScrollPx = 0;
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
      s_oledMainScrollPx = 0;
      s_tabDrillIn = false;
    } else {
      s_homeMenuIndex = display_tabs::homeMenuSlotForContent(display_tabs::CT_NET);
      s_currentScreen = 0;
      s_msgScrollStart = 0;
      s_oledMainScrollPx = 0;
    }
    s_menuActive = false;
    drawScreen(s_currentScreen);
    return;
  }

  if (ct == display_tabs::CT_SYS) {
    if (s_sysInDisplaySubmenu) {
      const int bi = sysDisplaySubBackIdx();
      if (s_sysMenuIndex == bi) {
        s_sysInDisplaySubmenu = false;
        s_sysMenuIndex = 0;
        s_sysScrollOff = 0;
      } else if (s_sysMenuIndex == 0) {
        s_ui_hooks.show_language_picker();
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
        clampSysDisplaySubMenuIndex();
        /* Список: 4 пункта, «Назад»=3; вкладки: 5, «Назад»=4 — после list→tabs сдвинуть курсор с «Назад». */
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
        s_oledMainScrollPx = 0;
        s_msgScrollStart = 0;
        s_tabDrillIn = false;
        s_sysInDisplaySubmenu = false;
      } else {
        s_homeMenuIndex = display_tabs::homeMenuSlotForContent(display_tabs::CT_SYS);
        s_currentScreen = 0;
        s_oledMainScrollPx = 0;
        s_msgScrollStart = 0;
        s_sysInDisplaySubmenu = false;
      }
    } else if (s_sysMenuIndex == 0) {
      s_sysInDisplaySubmenu = true;
      s_sysMenuIndex = 0;
      s_sysScrollOff = 0;
    } else {
      const int e = sysMainMenuIndexToExecSel(s_sysMenuIndex);
      if (e >= 0) ui_menu_exec::exec_sys_menu(e, s_ui_hooks);
    }
    s_menuActive = false;
    drawScreen(s_currentScreen);
    return;
  }

  if (ct == display_tabs::CT_GPS) {
    bool gpsOn = gps::isPresent() && gps::isEnabled();
    char gpsBuf[20];
    snprintf(gpsBuf, sizeof(gpsBuf), "GPS: %s", gpsOn ? "ON -> OFF" : "OFF -> ON");
    const char* items[] = {
      gpsBuf,
      locale::getForDisplay("menu_back")
    };
    const uint8_t* gIcons[2] = {ui_icons::gpsMenuIcon(0), ui_icons::gpsMenuIcon(1)};
    int sel = displayShowPopupMenu("", items, 2, 0, POPUP_MODE_CANCEL, gIcons, true, false);
    ui_menu_exec::exec_gps_menu(sel);
  }

  s_menuActive = false;
  drawScreen(s_currentScreen);
}

void displaySetButtonPolledExternally(bool on) {
  s_buttonPolledExternally = on;
}

void displayRequestInfoRedraw() {
  displayWakeRequest();
  s_needRedrawInfo = true;
}

void displayShowWarning(const char* line1, const char* line2, uint32_t durationMs) {
  if (!disp) return;
  prepareTabLayoutShiftOled();
  drawChromeTabsOrIdleRowOled();
  drawSubmenuTitleBarOled(locale::getForDisplay("warn_title"));
  disp->setTextColor(SSD1306_WHITE);
  if (line1) drawTruncRaw(CONTENT_X, SUBMENU_LIST_Y0_OLED + s_layoutTabShiftY, line1, MAX_LINE_CHARS);
  if (line2)
    drawTruncRaw(CONTENT_X, SUBMENU_LIST_Y0_OLED + s_layoutTabShiftY + SUBMENU_CONTENT_LINE_H, line2, MAX_LINE_CHARS);
  drawSubmenuFooterSepOled();
  disp->display();
  uint32_t start = millis();
  while (millis() - start < durationMs) {
    delay(50);
    yield();
  }
}

void displayShowSelftestSummary(bool radioOk, bool antennaOk, uint16_t batteryMv, uint32_t heapFree) {
  if (!disp) return;
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
  if (!disp) return false;

  uint32_t now = millis();
  static bool s_prevTabStripOled = true;
  ui_tab_bar_idle::tick(s_tabDrillIn);
  if (ui_nav_mode::isTabMode()) {
    const bool sh = ui_tab_bar_idle::tabStripVisible();
    if (sh != s_prevTabStripOled) {
      s_prevTabStripOled = sh;
      drawScreen(s_currentScreen);
      return true;
    }
  } else {
    s_prevTabStripOled = true;
  }

  // Запрос пробуждения (BLE connect, LoRa msg и т.п.)
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

  // Таймер неактивности → слип
  if (!s_displaySleeping && (now - s_lastActivityTime) > DISPLAY_SLEEP_MS) {
    displaySleep();
    return false;  // не вызывать drawScreen — иначе display() может включить дисплей обратно
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
