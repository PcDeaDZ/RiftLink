/**
 * RiftLink Display — OLED SSD1306
 * Конфигурация по Meshtastic variant.h:
 * V3/V4: SDA=17, SCL=18, RST=21, Vext=36 (active LOW = питание ON)
 */

#include "display.h"
#include "display_tabs.h"
#include "bootscreen_oled.h"
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
#include <cstring>
#include <cstdio>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "utf8rus.h"
#include "cp1251_to_rusfont.h"
#include <Wire.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define SDA_PIN 17
#define SCL_PIN 18
#define OLED_RST 21
#define VEXT_PIN 36
#define VEXT_ON_LEVEL LOW
#define BUTTON_PIN 0

#define TAB_H 12
#define CONTENT_Y 14
#define CONTENT_H 50   // до низа экрана (было 38, футер убран)
#define FOOTER_Y 56   // не используется
#define ICON_W 8
#define ICON_H 8
#define LINE_H 8
#define MAX_LINE_CHARS 20
#define CONTENT_X 4

static const char* TAB_KEYS[] = {"tab_main", "tab_msg", "tab_info", "tab_sys", "tab_net", "tab_gps"};

#define SHORT_PRESS_MS  350   // короткое = смена вкладки
#define LONG_PRESS_MS   500   // длинное = подтверждение / вход в режим модификации
#define MIN_PRESS_MS    30    // минимум для debounce (игнор дребезга)
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
  int y = CONTENT_Y + 2 + line * LINE_H;
  if (useUtf8)
    drawTruncUtf8(CONTENT_X, y, s, MAX_LINE_CHARS);
  else
    drawTruncRaw(CONTENT_X, y, s, MAX_LINE_CHARS);
}

void displayInit() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, VEXT_ON_LEVEL);
  delay(150);

  Wire.begin(SDA_PIN, SCL_PIN);
  disp = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RST);
  // false = не вызывать Wire.begin() из SSD1306 (уже инициализировали) — иначе W "Bus already started"
  if (!disp->begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false)) {
    disp->begin(SSD1306_SWITCHCAPVCC, 0x3D, true, false);
  }
  disp->clearDisplay();
  disp->setTextColor(SSD1306_WHITE);
  disp->setTextSize(1);
  disp->cp437(true);  // без сдвига кодов >=176 — нужен для CP1251 кириллицы
  disp->display();
  s_lastActivityTime = millis();
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
  disp->setTextSize(1);
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
      delay(100);
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
  uint32_t lastPress = millis();
  const uint32_t CONFIRM_MS = 2500;

  while (1) {
    disp->clearDisplay();
    disp->setTextSize(1);
    disp->setTextColor(SSD1306_WHITE);

    drawTruncRaw(4, 4, locale::getForLang("lang_picker_title", pickLang), 18);
    disp->setCursor(36, 24);
    disp->print(pickLang == LANG_EN ? "[EN]" : " EN ");
    disp->print(" ");
    disp->print(pickLang == LANG_RU ? "[RU]" : " RU ");
    drawTruncRaw(4, 48, locale::getForLang("short_long_hint", pickLang), 18);

    disp->display();

    while (millis() - lastPress < CONFIRM_MS) {
      int pt = waitButtonPressWithType(CONFIRM_MS - (millis() - lastPress));
      if (pt == PRESS_SHORT) {
        pickLang = pickLang == LANG_EN ? LANG_RU : LANG_EN;
        lastPress = millis();
        break;  // перерисовать экран с новым языком
      } else if (pt == PRESS_LONG || pt == PRESS_NONE) {
        goto lang_done;
      }
    }
  }
lang_done:
  locale::setLang(pickLang);
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

  uint32_t lastPress = millis();
  const uint32_t CONFIRM_MS = 2500;

  while (1) {
    disp->clearDisplay();
    disp->setTextSize(1);
    disp->setTextColor(SSD1306_WHITE);

    drawTruncRaw(4, 4, locale::getForDisplay("select_country"), 18);
    // Варианты как на Paper: [EU] UK RU US AU — выбран в скобках
    disp->setCursor(4, 20);
    for (int i = 0; i < nPresets; i++) {
      const char* code = region::getPresetCode(i);
      if (i == pickIdx) {
        disp->print("[");
        disp->print(code);
        disp->print("]");
      } else {
        disp->print(" ");
        disp->print(code);
        disp->print(" ");
      }
    }
    drawTruncRaw(4, 36, locale::getForDisplay("short_long_hint"), 18);

    disp->display();

    while (millis() - lastPress < CONFIRM_MS) {
      int pt = waitButtonPressWithType(CONFIRM_MS - (millis() - lastPress));
      if (pt == PRESS_SHORT) {
        pickIdx = (pickIdx + 1) % nPresets;
        lastPress = millis();
        break;
      } else if (pt == PRESS_LONG || pt == PRESS_NONE) {
        goto region_done;
      }
    }
  }
region_done:
  region::setRegion(region::getPresetCode(pickIdx));
  return true;
}

static void displayShowModemPicker() {
  if (!disp) return;
  delay(200);

  int pickIdx = (int)radio::getModemPreset();
  if (pickIdx < 0 || pickIdx > 4) pickIdx = 1;

  const char* names[] = {"Speed", "Normal", "Range", "MaxRange", "Custom"};
  const char* desc[]  = {"SF7 BW250", "SF7 BW125", "SF10 BW125", "SF12 BW125", ""};

  uint32_t lastPress = millis();
  const uint32_t CONFIRM_MS = 3000;

  while (1) {
    disp->clearDisplay();
    disp->setTextSize(1);
    disp->setTextColor(SSD1306_WHITE);

    drawTruncRaw(4, 4, "Modem Preset", 18);
    // [Normal] Speed Range...
    disp->setCursor(4, 20);
    for (int i = 0; i < 5; i++) {
      if (i == pickIdx) { disp->print("["); disp->print(names[i]); disp->print("]"); }
      else { disp->print(" "); disp->print(names[i][0]); }
    }
    if (pickIdx < 4) drawTruncRaw(4, 36, desc[pickIdx], 18);
    else {
      char buf[24];
      snprintf(buf, sizeof(buf), "SF%u BW%.0f CR%u",
          radio::getSpreadingFactor(), radio::getBandwidth(), radio::getCodingRate());
      drawTruncRaw(4, 36, buf, 18);
    }
    drawTruncRaw(4, 48, locale::getForDisplay("short_long_hint"), 18);
    disp->display();

    while (millis() - lastPress < CONFIRM_MS) {
      int pt = waitButtonPressWithType(CONFIRM_MS - (millis() - lastPress));
      if (pt == PRESS_SHORT) {
        pickIdx = (pickIdx + 1) % 5;
        lastPress = millis();
        break;
      } else if (pt == PRESS_LONG || pt == PRESS_NONE) {
        goto modem_done;
      }
    }
  }
modem_done:
  if (pickIdx < 4) radio::requestModemPreset((radio::ModemPreset)pickIdx);
}

static void displayRunModemScan() {
  if (!disp) return;
  disp->clearDisplay();
  disp->setTextSize(1);
  disp->setTextColor(SSD1306_WHITE);
  drawTruncRaw(4, 4, locale::getForDisplay("scanning"), 18);
  drawTruncRaw(4, 20, "~36s ...", 18);
  disp->display();

  selftest::ScanResult res[6];
  int found = selftest::modemScan(res, 6);

  disp->clearDisplay();
  if (found == 0) {
    drawTruncRaw(4, 4, locale::getForDisplay("scan_empty"), 18);
  } else {
    drawTruncRaw(4, 4, locale::getForDisplay("scan_found"), 18);
    for (int i = 0; i < found && i < 4; i++) {
      char buf[28];
      snprintf(buf, sizeof(buf), "SF%u BW%.0f %ddBm", res[i].sf, res[i].bw, res[i].rssi);
      drawTruncRaw(4, 16 + i * 12, buf, 18);
    }
  }
  disp->display();
  for (int i = 0; i < 50; i++) { delay(100); yield(); }
}

// Рамка: вкладки сверху, разделитель, футер
static void drawFrame(int activeTab) {
  if (!disp) return;
  disp->clearDisplay();

  int nTabs = display_tabs::getTabCount();
  int tabW = SCREEN_WIDTH / nTabs;

  // Вкладки с иконками
  for (int i = 0; i < nTabs; i++) {
    int x = i * tabW;
    int iconX = x + (tabW - ICON_W) / 2;
    int iconY = 2;
    const uint8_t* icon = display_tabs::getIconForTab(i);
    if (i == activeTab) {
      disp->fillRoundRect(x + 1, 1, tabW - 2, TAB_H - 2, 1, SSD1306_WHITE);
      disp->drawBitmap(iconX, iconY, icon, ICON_W, ICON_H, SSD1306_BLACK);
    } else {
      disp->drawBitmap(iconX, iconY, icon, ICON_W, ICON_H, SSD1306_WHITE);
    }
    if (i < nTabs - 1) {
      disp->drawLine(x + tabW, 2, x + tabW, TAB_H - 2, SSD1306_WHITE);
    }
  }
  disp->setTextColor(SSD1306_WHITE);

  disp->drawLine(0, TAB_H, SCREEN_WIDTH - 1, TAB_H, SSD1306_WHITE);
  disp->drawRect(0, CONTENT_Y, SCREEN_WIDTH, CONTENT_H, SSD1306_WHITE);
}

/** Battery icon: 16x7 outline with fill proportional to percent */
static void drawBatteryIcon(int x, int y, int pct, bool charging) {
  if (!disp) return;
  // Body: 14x7, nub: 2x3 on right
  disp->drawRect(x, y, 14, 7, SSD1306_WHITE);
  disp->fillRect(x + 14, y + 2, 2, 3, SSD1306_WHITE);
  if (pct > 0) {
    int fill = (pct * 10) / 100;
    if (fill < 1 && pct > 0) fill = 1;
    if (fill > 10) fill = 10;
    disp->fillRect(x + 2, y + 2, fill, 3, SSD1306_WHITE);
  }
  if (charging) {
    // Lightning bolt: small zigzag inside icon
    disp->drawLine(x + 8, y + 1, x + 6, y + 3, SSD1306_WHITE);
    disp->drawLine(x + 6, y + 3, x + 9, y + 3, SSD1306_WHITE);
    disp->drawLine(x + 9, y + 3, x + 7, y + 5, SSD1306_WHITE);
  }
}

/** Signal strength bars: 4 bars at x,y. barsCount 0-4 */
static void drawSignalBars(int x, int y, int barsCount) {
  if (!disp) return;
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;  // heights: 2,4,6,8
    int bx = x + i * 4;
    int by = y + 8 - h;
    if (i < barsCount)
      disp->fillRect(bx, by, 3, h, SSD1306_WHITE);
    else
      disp->drawRect(bx, by, 3, h, SSD1306_WHITE);
  }
}

static int rssiToBars(int rssi) {
  if (rssi >= -75) return 4;
  if (rssi >= -85) return 3;
  if (rssi >= -95) return 2;
  if (rssi >= -105) return 1;
  return 0;
}

static void drawContentMain() {
  if (!disp) return;
  char buf[28];

  // Line 0: nick (or short ID) + clock right
  char nick[33];
  node::getNickname(nick, sizeof(nick));
  if (nick[0]) {
    drawTruncUtf8(CONTENT_X, CONTENT_Y + 2, nick, MAX_LINE_CHARS);
  } else {
    const uint8_t* id = node::getId();
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", id[0], id[1], id[2], id[3]);
    drawTruncRaw(CONTENT_X, CONTENT_Y + 2, buf, MAX_LINE_CHARS);
  }
  if (gps::hasTime()) {
    char clk[6];
    snprintf(clk, sizeof(clk), "%02d:%02d", gps::getHour(), gps::getMinute());
    drawTruncRaw(SCREEN_WIDTH - 30 - 2, CONTENT_Y + 2, clk, 5);
  }

  // Line 1: modem preset or SF/BW
  radio::ModemPreset mp = radio::getModemPreset();
  if (mp < radio::MODEM_CUSTOM)
    snprintf(buf, sizeof(buf), "%.0fMHz %s", region::getFreq(), radio::modemPresetName(mp));
  else
    snprintf(buf, sizeof(buf), "%.0fMHz SF%u BW%.0f", region::getFreq(), (unsigned)radio::getSpreadingFactor(), radio::getBandwidth());
  drawTruncRaw(CONTENT_X, CONTENT_Y + 10, buf, MAX_LINE_CHARS);

  // Line 2: neighbors + signal bars
  int n = neighbors::getCount();
  int avgRssi = neighbors::getAverageRssi();
  snprintf(buf, sizeof(buf), "%s %d", locale::getForDisplay("neighbors"), n);
  drawTruncRaw(CONTENT_X, CONTENT_Y + 18, buf, MAX_LINE_CHARS);
  if (n > 0) {
    drawSignalBars(SCREEN_WIDTH - 20, CONTENT_Y + 18, rssiToBars(avgRssi));
  }

  // Bottom-right: battery icon + percent
  int pct = telemetry::batteryPercent();
  bool chg = telemetry::isCharging();
  int batY = CONTENT_Y + CONTENT_H - 9;
  drawBatteryIcon(SCREEN_WIDTH - 38, batY, pct >= 0 ? pct : 0, chg);
  if (chg) snprintf(buf, sizeof(buf), "%s", locale::getForDisplay("charging"));
  else if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
  else snprintf(buf, sizeof(buf), "--");
  drawTruncRaw(SCREEN_WIDTH - 20, batY, buf, 4);
}

static void drawContentInfo() {
  if (!disp) return;
  char buf[28];
  int n = neighbors::getCount();

  snprintf(buf, sizeof(buf), "%s: %d", locale::getForDisplay("peers"), n);
  drawTruncRaw(CONTENT_X, CONTENT_Y + 2, buf, MAX_LINE_CHARS);

  int maxShow = (n > 5) ? 5 : n;
  for (int i = 0; i < maxShow; i++) {
    char hex[17];
    neighbors::getIdHex(i, hex);
    int rssi = neighbors::getRssi(i);
    snprintf(buf, sizeof(buf), "%c%c%c%c %ddBm", hex[0], hex[1], hex[2], hex[3], rssi);
    drawTruncRaw(CONTENT_X, CONTENT_Y + 2 + (1 + i) * LINE_H, buf, 16);
    drawSignalBars(SCREEN_WIDTH - 20, CONTENT_Y + 2 + (1 + i) * LINE_H, rssiToBars(rssi));
  }
  if (n > 5) {
    snprintf(buf, sizeof(buf), "+%d more", n - 5);
    drawTruncRaw(CONTENT_X, CONTENT_Y + 2 + 6 * LINE_H, buf, MAX_LINE_CHARS);
  }
}

static void drawContentNet() {
  if (!disp) return;
  char buf[24];

  if (radio_mode::current() == radio_mode::BLE) {
    drawContentLine(0, locale::getForDisplay("ble_mode"));
    snprintf(buf, sizeof(buf), "%s %06u", locale::getForDisplay("pin"), (unsigned)ble::getPasskey());
    drawContentLine(1, buf);
    drawContentLine(2, ble::isConnected() ? locale::getForDisplay("connected") : "...");
    drawContentLine(4, locale::getForDisplay("hold_wifi"));
  } else {
    if (wifi::isConnected()) {
      char ssid[24] = {0}, ip[20] = {0};
      wifi::getStatus(ssid, sizeof(ssid), ip, sizeof(ip));
      drawContentLine(0, locale::getForDisplay("connected"));
      drawContentLine(1, ssid[0] ? ssid : "-", true);
      drawContentLine(2, ip[0] ? ip : "-");
    } else {
      drawContentLine(0, locale::getForDisplay("wifi_mode"));
      drawContentLine(1, "not connected");
      drawContentLine(2, "...");
    }
    drawContentLine(4, locale::getForDisplay("hold_ble"));
  }
}

static void drawContentSys() {
  if (!disp) return;
  char buf[24];
  snprintf(buf, sizeof(buf), "RiftLink v%s", RIFTLINK_VERSION);
  drawContentLine(0, buf);

  const uint8_t* id = node::getId();
  snprintf(buf, sizeof(buf), "ID: %02X%02X%02X%02X", id[0], id[1], id[2], id[3]);
  drawContentLine(1, buf);

  radio::ModemPreset mp = radio::getModemPreset();
  if (mp < radio::MODEM_CUSTOM)
    snprintf(buf, sizeof(buf), "%s %s %ddBm", region::getCode(), radio::modemPresetName(mp), region::getPower());
  else
    snprintf(buf, sizeof(buf), "%s SF%u/%u/%u", region::getCode(), radio::getSpreadingFactor(), (unsigned)radio::getBandwidth(), radio::getCodingRate());
  drawContentLine(2, buf);

  snprintf(buf, sizeof(buf), "PS:%s %s", powersave::isEnabled() ? "ON" : "OFF", locale::getLang() == LANG_RU ? "[RU]" : "[EN]");
  drawContentLine(3, buf);

  drawContentLine(4, locale::getForDisplay("hold_settings"));
}

static void drawContentMsg() {
  if (!disp) return;
  drawContentLine(0, locale::getForDisplay("from"));
  drawContentLine(1, s_lastMsgFrom[0] ? s_lastMsgFrom : "-");
  drawContentLine(2, s_lastMsgText[0] ? s_lastMsgText : locale::getForDisplay("no_messages"), true);
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
  if (gps::isEnabled()) {
    uint32_t sat = gps::getSatellites();
    float course = gps::getCourseDeg();
    const char* card = gps::getCourseCardinal();
    if (sat > 0 && course >= 0) snprintf(buf, sizeof(buf), "%u sat %0.0f° %s", (unsigned)sat, course, card);
    else if (sat > 0) snprintf(buf, sizeof(buf), "%u sat", (unsigned)sat);
    else if (course >= 0) snprintf(buf, sizeof(buf), "%0.0f° %s", course, card);
    else snprintf(buf, sizeof(buf), "-");
    drawContentLine(2, buf);
  }
  if (gps::hasFix()) {
    snprintf(buf, sizeof(buf), "%.4f %.4f", gps::getLat(), gps::getLon());
    drawContentLine(3, buf);
  }
  drawContentLine(4, locale::getForDisplay("hold_gps"));
}

static void drawScreen(int tab) {
  drawFrame(tab);
  switch (display_tabs::contentForTab(tab)) {
    case display_tabs::CT_MAIN: drawContentMain(); break;
    case display_tabs::CT_MSG:  drawContentMsg(); break;
    case display_tabs::CT_INFO: drawContentInfo(); break;
    case display_tabs::CT_NET:  drawContentNet(); break;
    case display_tabs::CT_SYS:  drawContentSys(); break;
    case display_tabs::CT_GPS:  drawContentGps(); break;
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
  if (display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_MSG) s_needRedrawMsg = true;
}

void displayShowScreen(int screen) {
  int nTabs = display_tabs::getTabCount();
  s_currentScreen = (screen < nTabs) ? screen : (nTabs - 1);
  s_lastActivityTime = millis();  // смена экрана (пикеры) — активность
  drawScreen(s_currentScreen);
}

void displayShowScreenForceFull(int screen) {
  displayShowScreen(screen);  // OLED — full/partial не различаются
}

int displayGetCurrentScreen() {
  return s_currentScreen;
}

int displayGetNextScreen(int current) {
  return (current + 1) % display_tabs::getTabCount();
}

/** Popup menu: short press = next item, long press = select, timeout = back.
 *  Returns selected index or -1 on timeout. */
static int displayShowPopupMenu(const char* items[], int count) {
  if (!disp || count <= 0) return -1;
  delay(200);
  int selected = 0;
  int scrollOff = 0;
  uint32_t lastPress = millis();
  const uint32_t MENU_TIMEOUT_MS = 10000;
  const int maxVisible = (CONTENT_H - 4) / LINE_H;

  while (1) {
    if (selected < scrollOff) scrollOff = selected;
    if (selected >= scrollOff + maxVisible) scrollOff = selected - maxVisible + 1;

    drawFrame(s_currentScreen);
    disp->setTextSize(1);
    int show = count - scrollOff;
    if (show > maxVisible) show = maxVisible;
    for (int i = 0; i < show; i++) {
      int idx = scrollOff + i;
      int y = CONTENT_Y + 2 + i * LINE_H;
      if (idx == selected) {
        disp->fillRect(1, y - 1, SCREEN_WIDTH - 2, LINE_H, SSD1306_WHITE);
        disp->setTextColor(SSD1306_BLACK, SSD1306_WHITE);
      } else {
        disp->setTextColor(SSD1306_WHITE, SSD1306_BLACK);
      }
      drawTruncRaw(CONTENT_X + 2, y, items[idx], MAX_LINE_CHARS - 1);
    }
    disp->setTextColor(SSD1306_WHITE);
    if (scrollOff > 0)
      disp->fillTriangle(SCREEN_WIDTH - 6, CONTENT_Y + 2, SCREEN_WIDTH - 10, CONTENT_Y + 6, SCREEN_WIDTH - 2, CONTENT_Y + 6, SSD1306_WHITE);
    if (scrollOff + maxVisible < count)
      disp->fillTriangle(SCREEN_WIDTH - 6, CONTENT_Y + CONTENT_H - 3, SCREEN_WIDTH - 10, CONTENT_Y + CONTENT_H - 7, SCREEN_WIDTH - 2, CONTENT_Y + CONTENT_H - 7, SSD1306_WHITE);
    disp->display();

    while (millis() - lastPress < MENU_TIMEOUT_MS) {
      int pt = waitButtonPressWithType(MENU_TIMEOUT_MS - (millis() - lastPress));
      if (pt == PRESS_SHORT) {
        selected = (selected + 1) % count;
        lastPress = millis();
        break;
      } else if (pt == PRESS_LONG) {
        return selected;
      } else if (pt == PRESS_NONE) {
        return -1;
      }
    }
    if (millis() - lastPress >= MENU_TIMEOUT_MS) return -1;
  }
}

void displayOnLongPress(int screen) {
  s_lastActivityTime = millis();
  s_menuActive = true;
  display_tabs::ContentTab ct = display_tabs::contentForTab(screen);

  if (ct == display_tabs::CT_SYS) {
    char psBuf[20];
    snprintf(psBuf, sizeof(psBuf), "PS: %s", powersave::isEnabled() ? "ON -> OFF" : "OFF -> ON");
    const char* items[] = {
      locale::getForDisplay("menu_modem"),
      locale::getForDisplay("scan_title"),
      psBuf,
      locale::getForDisplay("region"),
      locale::getForDisplay("select_lang"),
      locale::getForDisplay("menu_selftest"),
      locale::getForDisplay("menu_back")
    };
    int sel = displayShowPopupMenu(items, 7);
    if (sel == 0) { displayShowModemPicker(); }
    else if (sel == 1) { displayRunModemScan(); }
    else if (sel == 2) { powersave::setEnabled(!powersave::isEnabled()); }
    else if (sel == 3) { displayShowRegionPicker(); }
    else if (sel == 4) { displayShowLanguagePicker(); }
    else if (sel == 5) { selftest::run(nullptr); }

  } else if (ct == display_tabs::CT_NET) {
    bool isBle = (radio_mode::current() == radio_mode::BLE);
    const char* items[] = {
      isBle ? "-> WiFi" : "-> BLE",
      locale::getForDisplay("menu_back")
    };
    int sel = displayShowPopupMenu(items, 2);
    if (sel == 0) {
      radio_mode::switchTo(isBle ? radio_mode::WIFI : radio_mode::BLE);
    }

  } else if (ct == display_tabs::CT_GPS) {
    bool gpsOn = gps::isPresent() && gps::isEnabled();
    char gpsBuf[20];
    snprintf(gpsBuf, sizeof(gpsBuf), "GPS: %s", gpsOn ? "ON -> OFF" : "OFF -> ON");
    const char* items[] = {
      gpsBuf,
      locale::getForDisplay("menu_back")
    };
    int sel = displayShowPopupMenu(items, 2);
    if (sel == 0 && gps::isPresent()) { gps::toggle(); }
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
  disp->clearDisplay();
  disp->setTextSize(1);
  disp->setTextColor(SSD1306_WHITE);
  disp->drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  drawTruncRaw(8, 20, line1, MAX_LINE_CHARS);
  if (line2) drawTruncRaw(8, 36, line2, MAX_LINE_CHARS);
  disp->display();
  uint32_t start = millis();
  while (millis() - start < durationMs) {
    delay(50);
    yield();
  }
}

bool displayUpdate() {
  if (!disp) return false;

  uint32_t now = millis();

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
  if (s_needRedrawMsg && display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_MSG) {
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
        s_currentScreen = displayGetNextScreen(s_currentScreen);
        drawScreen(s_currentScreen);
      } else if (isLong) displayOnLongPress(s_currentScreen);
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
    display_tabs::ContentTab ct = display_tabs::contentForTab(s_currentScreen);
    if (ct == display_tabs::CT_MAIN || ct == display_tabs::CT_INFO || ct == display_tabs::CT_NET || ct == display_tabs::CT_GPS) drawScreen(s_currentScreen);
  }
  return false;
}
