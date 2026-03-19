/**
 * RiftLink Display — OLED SSD1306
 * Конфигурация по Meshtastic variant.h:
 * V3/V4: SDA=17, SCL=18, RST=21, Vext=36 (active LOW = питание ON)
 */

#include "display.h"
#include "bootscreen_oled.h"
#include "locale/locale.h"
#include "selftest/selftest.h"
#include "node/node.h"
#include "gps/gps.h"
#include "region/region.h"
#include "neighbors/neighbors.h"
#include "wifi/wifi.h"
#include "ota/ota.h"
#include "telemetry/telemetry.h"
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

// Иконки вкладок 8x8 (1=пиксель, MSB слева) — уникальные для каждой вкладки
static const uint8_t ICON_MAIN[]  = {0x08,0x0C,0x0E,0x08,0x08,0x08,0x1C,0x00};  // антенна/радио
static const uint8_t ICON_INFO[]  = {0x00,0x1C,0x08,0x08,0x08,0x08,0x1C,0x00};  // i в скобках
static const uint8_t ICON_WIFI[]  = {0x00,0x08,0x14,0x22,0x41,0x22,0x14,0x08};  // wifi дуги
static const uint8_t ICON_SYS[]   = {0x18,0x3C,0x5A,0xBD,0xBD,0x5A,0x3C,0x18};  // шестерёнка
static const uint8_t ICON_MSG[]   = {0x3C,0x42,0x42,0x42,0x42,0x3C,0x7E,0x00};  // пузырь чата
static const uint8_t ICON_LANG[]  = {0x3C,0x42,0x99,0xBD,0xBD,0x99,0x42,0x3C};  // глобус
static const uint8_t ICON_GPS[]   = {0x0C,0x1E,0x1E,0x1E,0x3F,0x1E,0x0C,0x00};  // метка места
static const uint8_t* TAB_ICONS[] = {ICON_MAIN, ICON_INFO, ICON_WIFI, ICON_SYS, ICON_MSG, ICON_LANG, ICON_GPS};

static const char* TAB_KEYS[] = {"tab_main", "tab_info", "tab_wifi", "tab_sys", "tab_msg", "tab_lang", "tab_gps"};

enum ContentTab { CT_MAIN, CT_INFO, CT_WIFI, CT_SYS, CT_MSG, CT_LANG, CT_GPS };
static int getTabCount() {
  return wifi::isAvailable() ? (gps::isPresent() ? 7 : 6) : (gps::isPresent() ? 6 : 5);
}
static ContentTab contentForTab(int tab) {
  if (wifi::isAvailable()) {
    switch (tab) { case 0: return CT_MAIN; case 1: return CT_INFO; case 2: return CT_WIFI;
      case 3: return CT_SYS; case 4: return CT_MSG; case 5: return CT_LANG; default: return CT_GPS; }
  } else {
    switch (tab) { case 0: return CT_MAIN; case 1: return CT_INFO; case 2: return CT_SYS;
      case 3: return CT_MSG; case 4: return CT_LANG; default: return CT_GPS; }
  }
}

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
  if (!disp->begin(SSD1306_SWITCHCAPVCC, 0x3C, true, true)) {
    disp->begin(SSD1306_SWITCHCAPVCC, 0x3D, true, true);
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

    drawTruncRaw(4, 8, locale::getForLang("lang_picker_title", pickLang), 18);
    disp->setCursor(36, 28);
    disp->print(pickLang == LANG_EN ? "[EN]" : " EN ");
    disp->print(" ");
    disp->print(pickLang == LANG_RU ? "[RU]" : " RU ");

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

// Рамка: вкладки сверху, разделитель, футер
static void drawFrame(int activeTab) {
  if (!disp) return;
  disp->clearDisplay();

  int nTabs = getTabCount();
  int tabW = SCREEN_WIDTH / nTabs;

  // Вкладки с иконками
  for (int i = 0; i < nTabs; i++) {
    int x = i * tabW;
    int iconX = x + (tabW - ICON_W) / 2;
    int iconY = 2;
    const uint8_t* icon = TAB_ICONS[(int)contentForTab(i)];
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

static void drawContentMain() {
  if (!disp) return;
  const uint8_t* id = node::getId();
  char buf[28];

  snprintf(buf, sizeof(buf), "%s %02X%02X%02X%02X", locale::getForDisplay("id"), id[0], id[1], id[2], id[3]);
  drawTruncRaw(CONTENT_X, CONTENT_Y + 2, buf, MAX_LINE_CHARS);

  int ch = region::getChannel();
  int nCh = region::getChannelCount();
  if (nCh > 0) {
    snprintf(buf, sizeof(buf), "%s %d %.1fMHz", locale::getForDisplay("ch"), ch, region::getFreq());
  } else {
    snprintf(buf, sizeof(buf), "%.1f MHz", region::getFreq());
  }
  drawTruncRaw(CONTENT_X, CONTENT_Y + 10, buf, MAX_LINE_CHARS);

  snprintf(buf, sizeof(buf), "%s %s %ddBm", locale::getForDisplay("region"), region::getCode(), region::getPower());
  drawTruncRaw(CONTENT_X, CONTENT_Y + 18, buf, MAX_LINE_CHARS);

  int n = neighbors::getCount();
  snprintf(buf, sizeof(buf), "%s %d", locale::getForDisplay("neighbors"), n);
  drawTruncRaw(CONTENT_X, CONTENT_Y + 26, buf, MAX_LINE_CHARS);

  uint16_t batMv = telemetry::readBatteryMv();
  int pct = (batMv >= 3000) ? (int)((batMv - 3000) / 12) : -1;
  if (pct > 100) pct = 100;
  const char* batLabel = locale::getForDisplay("battery");
  if (pct >= 0) snprintf(buf, sizeof(buf), "%s %d%%", batLabel, pct);
  else snprintf(buf, sizeof(buf), "%s --", batLabel);
  drawTruncRaw(CONTENT_X, CONTENT_Y + 34, buf, MAX_LINE_CHARS);
}

static void drawContentInfo() {
  if (!disp) return;
  char nick[17];
  node::getNickname(nick, sizeof(nick));
  char buf[28];

  drawContentLine(0, locale::getForDisplay("nickname"));
  drawContentLine(1, nick[0] ? nick : locale::getForDisplay("not_set"), true);

  snprintf(buf, sizeof(buf), "%.1f MHz", region::getFreq());
  drawTruncRaw(CONTENT_X, CONTENT_Y + 2 + 2 * LINE_H, buf, MAX_LINE_CHARS);

  snprintf(buf, sizeof(buf), "%s %ddBm", region::getCode(), region::getPower());
  drawTruncRaw(CONTENT_X, CONTENT_Y + 2 + 3 * LINE_H, buf, MAX_LINE_CHARS);
}

static void drawContentWiFi() {
  if (!disp) return;
  char ssid[24] = {0}, ip[20] = {0};
  wifi::getStatus(ssid, sizeof(ssid), ip, sizeof(ip));

  if (ota::isActive()) {
    char buf[24];
    drawContentLine(0, locale::getForDisplay("ota_ap"));
    drawContentLine(1, "RiftLink-OTA");
    drawContentLine(2, "192.168.4.1");
    snprintf(buf, sizeof(buf), "%s riftlink123", locale::getForDisplay("pass"));
    drawTruncRaw(CONTENT_X, CONTENT_Y + 2 + 3 * LINE_H, buf, MAX_LINE_CHARS);
  } else if (wifi::isConnected()) {
    drawContentLine(0, locale::getForDisplay("connected"));
    drawContentLine(1, ssid[0] ? ssid : "-", true);
    drawContentLine(2, ip[0] ? ip : "-");
    drawContentLine(3, locale::getForDisplay("sta_mode"));
  } else {
    drawContentLine(0, locale::getForDisplay("wifi_off"));
    drawContentLine(1, wifi::hasCredentials() ? locale::getForDisplay("reconnecting") : locale::getForDisplay("no_config"));
    drawContentLine(2, locale::getForDisplay("ble_wifi"));
    drawContentLine(3, locale::getForDisplay("ssid_pass"));
  }
}

static void drawContentSys() {
  if (!disp) return;
  char buf[24];
  snprintf(buf, sizeof(buf), "RiftLink v%s", RIFTLINK_VERSION);
  drawContentLine(0, buf);
  drawContentLine(1, "BLE + LoRa + E2E");
  drawContentLine(2, locale::getForDisplay("ota_cmd"));
}

static void drawContentMsg() {
  if (!disp) return;
  drawContentLine(0, locale::getForDisplay("from"));
  drawContentLine(1, s_lastMsgFrom[0] ? s_lastMsgFrom : "-");
  drawContentLine(2, s_lastMsgText[0] ? s_lastMsgText : locale::getForDisplay("no_messages"), true);
}

static void drawContentLang() {
  if (!disp) return;
  drawContentLine(0, locale::getForDisplay("select_lang"));
  int lang = locale::getLang();
  drawContentLine(1, lang == LANG_RU ? "[RU]" : " EN ");
}

static void drawContentGps() {
  if (!disp) return;
  char buf[32];
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
}

static void drawScreen(int tab) {
  drawFrame(tab);
  switch (contentForTab(tab)) {
    case CT_MAIN: drawContentMain(); break;
    case CT_INFO: drawContentInfo(); break;
    case CT_WIFI: drawContentWiFi(); break;
    case CT_SYS: drawContentSys(); break;
    case CT_MSG: drawContentMsg(); break;
    case CT_LANG: drawContentLang(); break;
    default: drawContentGps(); break;
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
  if (s_currentScreen == 4) s_needRedrawMsg = true;  // отложить — не блокировать handlePacket (stack overflow)
}

void displayShowScreen(int screen) {
  int nTabs = getTabCount();
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
  return (current + 1) % getTabCount();
}

void displayOnLongPress(int screen) {
  s_lastActivityTime = millis();
  ContentTab ct = contentForTab(screen);
  if (ct == CT_MAIN) {
    displayShowRegionPicker();
    drawScreen(s_currentScreen);
  } else if (ct == CT_LANG) {
    displayShowLanguagePicker();
    drawScreen(s_currentScreen);
  } else if (ct == CT_GPS && gps::isPresent()) {
    gps::toggle();
    drawScreen(s_currentScreen);
  } else if (ct == CT_SYS) {
    selftest::run(nullptr);
  }
}

void displaySetButtonPolledExternally(bool on) {
  s_buttonPolledExternally = on;
}

void displayRequestInfoRedraw() {
  displayWakeRequest();
  s_needRedrawInfo = true;
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

  if (s_needRedrawInfo && s_currentScreen == 1) {
    s_needRedrawInfo = false;
    s_lastActivityTime = now;  // BLE/ник — считаем активностью
    drawScreen(1);
  }
  if (s_needRedrawMsg && s_currentScreen == 4) {
    s_needRedrawMsg = false;
    s_lastActivityTime = now;  // входящее сообщение — активность
    drawScreen(4);
  }

  if (!s_buttonPolledExternally) {
    bool btn = BTN_PRESSED;
    if (btn) {
      if (!s_lastButton) { s_lastButton = true; s_pressStart = now; }
    } else if (s_lastButton) {
      uint32_t hold = now - s_pressStart;
      bool isLong = (hold >= LONG_PRESS_MS);
      bool isShort = (hold >= MIN_PRESS_MS && hold < SHORT_PRESS_MS);
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
  }

  if (!s_showingBootScreen && (millis() - s_lastScreenUpdate > 2000)) {
    s_lastScreenUpdate = millis();
    ContentTab ct = contentForTab(s_currentScreen);
    if (ct == CT_MAIN || ct == CT_INFO || ct == CT_WIFI || ct == CT_GPS) drawScreen(s_currentScreen);
  }
  return false;
}
