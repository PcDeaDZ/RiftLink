/**
 * RiftLink Display — E-Ink 2.13" Heltec Wireless Paper
 * Определение как Meshtastic: einkDetect.h — RST LOW, read BUSY. LOW→FC1(V1.1), HIGH→E0213A367(V1.2)
 * Пины: CS=4, BUSY=7, DC=5, RST=6, SCLK=3, MOSI=2, VEXT=45
 */

#include "../../display.h"
#include "bootscreen_paper.h"
#include "locale/locale.h"
#include "selftest/selftest.h"
#include "node/node.h"
#include "gps/gps.h"
#include "region/region.h"
#include "neighbors/neighbors.h"
#include "wifi/wifi.h"
#include "ota/ota.h"
#include "version.h"
#include <cstring>
#include <cstdio>
#include <Adafruit_GFX.h>
#include <GxEPD2_BW.h>
#include <epd/GxEPD2_213_BN.h>
#include <epd/GxEPD2_213_FC1.h>
#include <epd/GxEPD2_213_E0213A367.h>
#include "utf8rus.h"
#include "cp1251_to_rusfont.h"
#include <SPI.h>

#define EINK_CS   4
#define EINK_BUSY 7
#define EINK_DC   5
#define EINK_RST  6
#define EINK_SCLK 3
#define EINK_MOSI 2
#define VEXT_PIN  45
#define EINK_SPI_HZ 4000000  // 4 MHz — как в Meshtastic
#define VEXT_ON_LEVEL LOW
#define BUTTON_PIN 0   // Heltec WiFi LoRa 32 V3 / Wireless Paper: USER_SW на GPIO0

#define SCREEN_WIDTH  250
#define SCREEN_HEIGHT 122
#define TAB_H 18
#define ICON_W 8
#define ICON_H 8
#define ICON_SCALE 2

static const uint8_t ICON_MAIN[]  = {0x08,0x0C,0x0E,0x08,0x08,0x08,0x1C,0x00};
static const uint8_t ICON_INFO[]  = {0x00,0x1C,0x08,0x08,0x08,0x08,0x1C,0x00};
static const uint8_t ICON_WIFI[]  = {0x00,0x08,0x14,0x22,0x41,0x22,0x14,0x08};
static const uint8_t ICON_SYS[]   = {0x18,0x3C,0x5A,0xBD,0xBD,0x5A,0x3C,0x18};
static const uint8_t ICON_MSG[]   = {0x3C,0x42,0x42,0x42,0x42,0x3C,0x7E,0x00};
static const uint8_t ICON_LANG[]  = {0x3C,0x42,0x99,0xBD,0xBD,0x99,0x42,0x3C};
static const uint8_t ICON_GPS[]   = {0x0C,0x1E,0x1E,0x1E,0x3F,0x1E,0x0C,0x00};
static const uint8_t* TAB_ICONS[] = {ICON_MAIN, ICON_INFO, ICON_WIFI, ICON_SYS, ICON_MSG, ICON_LANG, ICON_GPS};
#define CONTENT_Y TAB_H
#define CONTENT_H (SCREEN_HEIGHT - TAB_H)
#define LINE_H 12
#define MAX_LINE_CHARS 32
#define CONTENT_X 4

using DispBN = GxEPD2_BW<GxEPD2_213_BN, GxEPD2_213_BN::HEIGHT>;
using DispFC1 = GxEPD2_BW<GxEPD2_213_FC1, GxEPD2_213_FC1::HEIGHT>;
using DispB73 = GxEPD2_BW<GxEPD2_213_E0213A367, GxEPD2_213_E0213A367::HEIGHT>;

static DispBN* dispBN = nullptr;
static DispFC1* dispFC1 = nullptr;
static DispB73* dispB73 = nullptr;
#define disp (dispBN ? (Adafruit_GFX*)dispBN : (dispFC1 ? (Adafruit_GFX*)dispFC1 : (Adafruit_GFX*)dispB73))

static int s_currentScreen = 0;
static bool s_lastButton = false;
static uint32_t s_pressStart = 0;
static uint32_t s_lastDisplayEnd = 0;     // когда последний display() завершился
static uint32_t s_previousRunMs = 0;      // Meshtastic: для rate limiting (когда последний раз обновили)
static uint32_t s_lastActivityTime = 0;   // активность (сообщения, смена экрана)
static uint32_t s_fastRefreshCount = 0;   // подряд partial — после EINK_LIMIT_FASTREFRESH делаем full
static uint32_t s_previousImageHash = 0;  // хеш последнего отображённого контента (пропуск дубликатов)
// volatile — s_needRedrawInfo пишется из BLE task (displayRequestInfoRedraw), читается в main loop
static volatile bool s_needRedrawMsg = false;
static volatile bool s_needRedrawInfo = false;
static char s_lastMsgFrom[17] = {0};
static char s_lastMsgText[64] = {0};

// Meshtastic-style rate limiting (EInkDynamicDisplay)
#define EINK_RATE_LIMIT_BACKGROUND_MS  30000  // BACKGROUND: min 30s между обновлениями
#define EINK_RATE_LIMIT_RESPONSIVE_MS  1000   // RESPONSIVE (кнопка, сообщение): min 1s
#define EINK_LIMIT_FASTREFRESH         5      // после N partial — принудительный full (против ghosting)
#define EINK_COOLDOWN_HW_MS            600    // аппаратный минимум между display() — иначе зависает

#define BTN_ACTIVE_LOW 1
#define BTN_PRESSED (digitalRead(BUTTON_PIN) == (BTN_ACTIVE_LOW ? LOW : HIGH))
#define SHORT_PRESS_MS 350   // граница short/long (используется в waitButtonPressWithType)
#define LONG_PRESS_MS  500
#define MIN_PRESS_MS   80   // защита от дребезга
#define POST_PRESS_DEBOUNCE_MS 200  // пауза после обработки нажатия — против двойного срабатывания
#define LED_PIN 35  // Heltec V3/V4 — мигание при нажатии (обратная связь)
#define N_TABS 7

enum PressType { PRESS_NONE = 0, PRESS_SHORT = 1, PRESS_LONG = 2 };

#if defined(ESP32)
static SPIClass hspi(HSPI);
#include <esp_task_wdt.h>
static void busyCallback(const void*) { esp_task_wdt_reset(); }
#endif

/** Определение панели как Meshtastic (einkDetect.h): RST LOW, read BUSY. LOW→FC1(V1.1), HIGH→E0213A367(V1.2) */
enum EInkModel { EINK_FC1, EINK_E0213A367 };
static EInkModel detectEInk() {
  pinMode(EINK_RST, OUTPUT);
  digitalWrite(EINK_RST, LOW);
  delay(10);
  pinMode(EINK_BUSY, INPUT);
  bool busyLow = (digitalRead(EINK_BUSY) == LOW);
  pinMode(EINK_RST, INPUT);  // release
  return busyLow ? EINK_FC1 : EINK_E0213A367;
}

static void drawTruncRaw(int x, int y, const char* s, int maxLen) {
  if (!disp || !s) return;
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

static void drawTruncUtf8(int x, int y, const char* s, int maxLen) {
  if (!disp || !s) return;
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

static void drawContentLine(int line, const char* s, bool useUtf8 = false) {
  int y = CONTENT_Y + 2 + line * LINE_H;
  if (useUtf8) drawTruncUtf8(CONTENT_X, y, s, MAX_LINE_CHARS);
  else drawTruncRaw(CONTENT_X, y, s, MAX_LINE_CHARS);
}

/** Хеш контента вкладки — для пропуска идентичных кадров (как Meshtastic checkFrameMatchesPrevious) */
static uint32_t computeContentHash(int tab) {
  uint32_t h = (uint32_t)tab * 31;
  if (tab == 0) {
    const uint8_t* id = node::getId();
    h ^= id[0] ^ (id[1] << 8) ^ (id[2] << 16) ^ (id[3] << 24);
    h ^= (uint32_t)region::getChannel() * 7;
    h ^= (uint32_t)(region::getFreq() * 100) * 11;
    h ^= (uint32_t)neighbors::getCount() * 13;
  } else if (tab == 1) {
    char nick[17];
    node::getNickname(nick, sizeof(nick));
    for (int i = 0; nick[i] && i < 16; i++) h = h * 31 + (uint8_t)nick[i];
    h ^= (uint32_t)(region::getFreq() * 100) * 17;
  } else if (tab == 2) {
    char ssid[24] = {0}, ip[20] = {0};
    wifi::getStatus(ssid, sizeof(ssid), ip, sizeof(ip));
    for (int i = 0; ssid[i] && i < 23; i++) h = h * 31 + (uint8_t)ssid[i];
    for (int i = 0; ip[i] && i < 19; i++) h = h * 31 + (uint8_t)ip[i];
    h ^= ota::isActive() ? 0x1234 : 0;
  } else if (tab == 4) {
    for (int i = 0; s_lastMsgFrom[i] && i < 16; i++) h = h * 31 + (uint8_t)s_lastMsgFrom[i];
    for (int i = 0; s_lastMsgText[i] && i < 63; i++) h = h * 31 + (uint8_t)s_lastMsgText[i];
  } else if (tab == 6 && gps::isPresent()) {
    h ^= gps::isEnabled() ? 1 : 0;
    h ^= gps::hasFix() ? 2 : 0;
    h ^= (uint32_t)gps::getSatellites() * 19;
    h ^= (uint32_t)(gps::getLat() * 10000) * 23;
    h ^= (uint32_t)(gps::getLon() * 10000) * 29;
  }
  return h;
}

static void doDisplay(bool partial) {
  if (dispBN) {
    if (partial) dispBN->setPartialWindow(0, 0, dispBN->width(), dispBN->height());
    else dispBN->setFullWindow();
    dispBN->display(partial);
  } else if (dispFC1) {
    if (partial) dispFC1->setPartialWindow(0, 0, dispFC1->width(), dispFC1->height());
    else dispFC1->setFullWindow();
    dispFC1->display(partial);
  } else if (dispB73) {
    if (partial) dispB73->setPartialWindow(0, 0, dispB73->width(), dispB73->height());
    else dispB73->setFullWindow();
    dispB73->display(partial);
  }
}

/** Аппаратный cooldown — E-Ink требует паузу между display(), иначе зависает. */
static void ensureCooldownBeforeDisplay() {
  uint32_t now = millis();
  uint32_t wait = (now - s_lastDisplayEnd) < EINK_COOLDOWN_HW_MS
      ? (EINK_COOLDOWN_HW_MS - (now - s_lastDisplayEnd)) : 0;
  while (wait > 0) {
    uint32_t chunk = (wait > 100) ? 100 : wait;
    delay(chunk);
    yield();
    wait -= chunk;
  }
}

void displayInit() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Meshtastic: detect display BEFORE starting SPI (einkDetect.h)
  EInkModel model = detectEInk();
#if defined(USE_EINK_FORCE_BN)
  model = EINK_FC1;
#elif defined(USE_EINK_FORCE_B73)
  model = EINK_E0213A367;
#endif

  pinMode(EINK_DC, OUTPUT);
  pinMode(EINK_BUSY, INPUT);
  pinMode(EINK_RST, OUTPUT);

#if defined(ESP32)
  // Meshtastic: hspi.begin(SCLK, -1, MOSI, CS). ESP32-S3: -1→spiAttachMiso, 3-wire: MOSI=MISO
  hspi.begin(EINK_SCLK, EINK_MOSI, EINK_MOSI, EINK_CS);
  delay(50);
#endif

  // RST reset как в GxEPD2
  digitalWrite(EINK_RST, LOW);
  delay(20);
  digitalWrite(EINK_RST, HIGH);
  delay(200);

#if defined(USE_EINK_FORCE_BN)
  Serial.println("[RiftLink] E-Ink init (forced BN)...");
  dispBN = new DispBN(GxEPD2_213_BN(EINK_CS, EINK_DC, EINK_RST, EINK_BUSY, hspi));
  dispBN->epd2.setBusyCallback(busyCallback);
  dispBN->init(0, true, 20, false);
  dispBN->setRotation(3);
  dispBN->fillScreen(GxEPD_WHITE);
  dispBN->setTextColor(GxEPD_BLACK);
  dispBN->setTextSize(1);
  dispBN->cp437(true);
  dispBN->display(false);
  delay(200);  // панель должна стабилизироваться после первого display()
  s_lastDisplayEnd = millis();
  s_previousRunMs = millis();
  Serial.println("[RiftLink] E-Ink BN init done");
#elif defined(ESP32)
  if (model == EINK_FC1) {
    Serial.println("[RiftLink] E-Ink init (FC1/LCMEN2R13EFC1 V1.1)...");
    dispFC1 = new DispFC1(GxEPD2_213_FC1(EINK_CS, EINK_DC, EINK_RST, EINK_BUSY, hspi));
    dispFC1->epd2.setBusyCallback(busyCallback);
    dispFC1->init(0, true, 20, false);
    dispFC1->setRotation(3);
    dispFC1->fillScreen(GxEPD_WHITE);
    dispFC1->setTextColor(GxEPD_BLACK);
    dispFC1->setTextSize(1);
    dispFC1->cp437(true);
    dispFC1->display(false);
  delay(200);  // панель должна стабилизироваться после первого display()
  s_lastDisplayEnd = millis();
  s_previousRunMs = millis();
  s_lastActivityTime = millis();
  Serial.println("[RiftLink] E-Ink FC1 init done");
  } else {
    Serial.println("[RiftLink] E-Ink init (E0213A367 V1.2)...");
    dispB73 = new DispB73(GxEPD2_213_E0213A367(EINK_CS, EINK_DC, EINK_RST, EINK_BUSY, hspi));
    dispB73->epd2.setBusyCallback(busyCallback);
    dispB73->init(0, true, 20, false);
    dispB73->setRotation(3);
    dispB73->fillScreen(GxEPD_WHITE);
    dispB73->setTextColor(GxEPD_BLACK);
    dispB73->setTextSize(1);
    dispB73->cp437(true);
    dispB73->display(false);
    delay(200);  // панель должна стабилизироваться после первого display()
    s_lastDisplayEnd = millis();
    s_previousRunMs = millis();
    s_lastActivityTime = millis();
    Serial.println("[RiftLink] E-Ink E0213A367 init done");
  }
#else
  if (model == EINK_FC1) {
    dispFC1 = new DispFC1(GxEPD2_213_FC1(EINK_CS, EINK_DC, EINK_RST, EINK_BUSY, SPI));
    dispFC1->init(115200, true, 20, false);
    dispFC1->setRotation(3);
    dispFC1->fillScreen(GxEPD_WHITE);
    dispFC1->setTextColor(GxEPD_BLACK);
    dispFC1->setTextSize(1);
    dispFC1->cp437(true);
    dispFC1->display(false);
  } else {
    dispB73 = new DispB73(GxEPD2_213_E0213A367(EINK_CS, EINK_DC, EINK_RST, EINK_BUSY, SPI));
    dispB73->init(115200, true, 20, false);
    dispB73->setRotation(3);
    dispB73->fillScreen(GxEPD_WHITE);
    dispB73->setTextColor(GxEPD_BLACK);
    dispB73->setTextSize(1);
    dispB73->cp437(true);
    dispB73->display(false);
  }
  delay(100);
  s_lastDisplayEnd = millis();
  s_previousRunMs = millis();
  s_lastActivityTime = millis();
  Serial.println("[RiftLink] E-Ink init done");
#endif
}

void displaySleep() {}
void displayWake() {}
void displayWakeRequest() {}
bool displayIsSleeping() { return false; }

void displayClear() {
  if (dispBN) dispBN->fillScreen(GxEPD_WHITE);
  else if (dispFC1) dispFC1->fillScreen(GxEPD_WHITE);
  else if (dispB73) dispB73->fillScreen(GxEPD_WHITE);
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
  ensureCooldownBeforeDisplay();
  doDisplay(false);
  s_lastDisplayEnd = millis();
}

void displaySetTextSize(uint8_t s) {
  if (disp) disp->setTextSize(s);
}

// Бут-скрин: логотип Rift Link из app_icon_source.png
void displayShowBootScreen() {
  if (!disp) return;
  ensureCooldownBeforeDisplay();
  disp->fillScreen(GxEPD_WHITE);
  disp->drawBitmap(0, 0, bootscreen_paper, BOOTSCREEN_PAPER_W, BOOTSCREEN_PAPER_H, GxEPD_BLACK);
  disp->setTextColor(GxEPD_BLACK);
  disp->setTextSize(2);
  char ver[16];
  snprintf(ver, sizeof(ver), "v%s", RIFTLINK_VERSION);
  disp->setCursor(4, SCREEN_HEIGHT - 18);
  disp->print(ver);
  doDisplay(false);
  s_lastDisplayEnd = millis();
}

static int waitButtonPressWithType(uint32_t timeoutMs) {
  if (timeoutMs == 0) return PRESS_NONE;
  if (timeoutMs > 30000) timeoutMs = 30000;  // защита от overflow (remaining мог бы быть 0xFFFFFFFF)
  uint32_t start = millis();
  uint32_t pressStart = 0;
  bool wasPressed = false;
  while (millis() - start < timeoutMs) {
    yield();
    if (BTN_PRESSED) {
      if (!wasPressed) pressStart = millis();
      wasPressed = true;
    } else if (wasPressed) {
      uint32_t hold = millis() - pressStart;
      delay(30);
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
  s_fastRefreshCount = 0;  // сброс — picker всегда full refresh
  int pickLang = locale::getLang();
  uint32_t lastPress = millis();
  const uint32_t CONFIRM_MS = 2500;
  while (1) {
    yield();
    ensureCooldownBeforeDisplay();
    disp->fillScreen(GxEPD_WHITE);
    disp->setTextColor(GxEPD_BLACK);
    disp->setTextSize(1);
    drawTruncRaw(4, 8, locale::getForLang("lang_picker_title", pickLang), 24);
    disp->setCursor(60, 40);
    disp->print(pickLang == LANG_EN ? "[EN]" : " EN ");
    disp->print(" ");
    disp->print(pickLang == LANG_RU ? "[RU]" : " RU ");
    drawTruncRaw(4, 60, locale::getForLang("short_long_hint", pickLang), 28);
    doDisplay(false);  // всегда full — против ghosting
    s_lastDisplayEnd = millis();
    while (millis() - lastPress < CONFIRM_MS) {
      yield();
      uint32_t elapsed = millis() - lastPress;
      if (elapsed >= CONFIRM_MS) break;
      uint32_t remaining = CONFIRM_MS - elapsed;
      int pt = waitButtonPressWithType(remaining);
      if (pt == PRESS_SHORT) {
        pickLang = pickLang == LANG_EN ? LANG_RU : LANG_EN;
        lastPress = millis();
        for (int i = 0; i < 4; i++) { delay(50); yield(); }  // 200ms чанками
        break;
      } else if (pt == PRESS_LONG || pt == PRESS_NONE) {
        goto lang_done;
      }
    }
    if (millis() - lastPress >= CONFIRM_MS) break;
  }
lang_done:
  locale::setLang(pickLang);
  yield();
  delay(100);
  return true;
}

bool displayShowRegionPicker() {
  if (!disp) return false;
  int nPresets = region::getPresetCount();
  if (nPresets <= 0) return false;  // защита от div-by-zero
  delay(200);
  s_fastRefreshCount = 0;
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
    yield();
    ensureCooldownBeforeDisplay();
    disp->fillScreen(GxEPD_WHITE);
    disp->setTextColor(GxEPD_BLACK);
    disp->setTextSize(1);
    drawTruncRaw(4, 4, locale::getForDisplay("select_country"), 24);
    drawTruncRaw(4, 18, locale::getForDisplay("country_rules"), 24);
    disp->setCursor(80, 50);
    disp->print(region::getPresetCode(pickIdx));
    doDisplay(false);  // всегда full
    s_lastDisplayEnd = millis();
    while (millis() - lastPress < CONFIRM_MS) {
      yield();
      uint32_t elapsed = millis() - lastPress;
      if (elapsed >= CONFIRM_MS) break;
      uint32_t remaining = CONFIRM_MS - elapsed;
      int pt = waitButtonPressWithType(remaining);
      if (pt == PRESS_SHORT) {
        pickIdx = (pickIdx + 1) % nPresets;
        lastPress = millis();
        for (int i = 0; i < 4; i++) { delay(50); yield(); }  // 200ms чанками
        break;
      } else if (pt == PRESS_LONG || pt == PRESS_NONE) {
        goto region_done;
      }
    }
    if (millis() - lastPress >= CONFIRM_MS) break;
  }
region_done:
  region::setRegion(region::getPresetCode(pickIdx));
  yield();
  delay(100);
  return true;
}

static void drawIconScaled(int x, int y, const uint8_t* icon, uint16_t color) {
  if (!icon || !disp) return;
  for (int row = 0; row < ICON_H; row++) {
    uint8_t line = icon[row];
    for (int col = 0; col < ICON_W; col++) {
      if (line & (0x80 >> col)) {
        disp->fillRect(x + col * ICON_SCALE, y + row * ICON_SCALE, ICON_SCALE, ICON_SCALE, color);
      }
    }
  }
}

static void drawFrame(int activeTab) {
  if (!disp) return;
  disp->fillScreen(GxEPD_WHITE);
  disp->setTextColor(GxEPD_BLACK);
  int tabW = SCREEN_WIDTH / N_TABS;
  int iconSize = ICON_W * ICON_SCALE;
  for (int i = 0; i < N_TABS; i++) {
    int x = i * tabW;
    int iconX = x + (tabW - iconSize) / 2;
    int iconY = (TAB_H - iconSize) / 2;
    if (i == activeTab) {
      disp->fillRect(x + 1, 1, tabW - 2, TAB_H - 2, GxEPD_BLACK);
      drawIconScaled(iconX, iconY, TAB_ICONS[i], GxEPD_WHITE);
    } else {
      drawIconScaled(iconX, iconY, TAB_ICONS[i], GxEPD_BLACK);
    }
    if (i < N_TABS - 1) {
      disp->drawFastVLine(x + tabW, 2, TAB_H - 2, GxEPD_BLACK);
    }
  }
  disp->drawFastHLine(0, TAB_H, SCREEN_WIDTH, GxEPD_BLACK);
  disp->drawRect(0, CONTENT_Y, SCREEN_WIDTH, CONTENT_H, GxEPD_BLACK);
}

static void drawContentMain() {
  if (!disp) return;
  const uint8_t* id = node::getId();
  char buf[32];
  snprintf(buf, sizeof(buf), "%s %02X%02X%02X%02X", locale::getForDisplay("id"), id[0], id[1], id[2], id[3]);
  drawTruncRaw(CONTENT_X, CONTENT_Y + 2, buf, MAX_LINE_CHARS);
  int ch = region::getChannel();
  int nCh = region::getChannelCount();
  if (nCh > 0) snprintf(buf, sizeof(buf), "%s %d %.1fMHz", locale::getForDisplay("ch"), ch, region::getFreq());
  else snprintf(buf, sizeof(buf), "%.1f MHz", region::getFreq());
  drawTruncRaw(CONTENT_X, CONTENT_Y + 14, buf, MAX_LINE_CHARS);
  snprintf(buf, sizeof(buf), "%s %s %ddBm", locale::getForDisplay("region"), region::getCode(), region::getPower());
  drawTruncRaw(CONTENT_X, CONTENT_Y + 26, buf, MAX_LINE_CHARS);
  int n = neighbors::getCount();
  snprintf(buf, sizeof(buf), "%s %d", locale::getForDisplay("neighbors"), n);
  drawTruncRaw(CONTENT_X, CONTENT_Y + 38, buf, MAX_LINE_CHARS);
}

static void drawContentInfo() {
  if (!disp) return;
  char nick[17];
  node::getNickname(nick, sizeof(nick));
  char buf[32];
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
    drawContentLine(0, locale::getForDisplay("ota_ap"));
    drawContentLine(1, "RiftLink-OTA");
    drawContentLine(2, "192.168.4.1");
    char buf[32];
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
  char buf[32];
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

static void drawScreenContent(int tab) {
  drawFrame(tab);
  if (tab == 0) drawContentMain();
  else if (tab == 1) drawContentInfo();
  else if (tab == 2) drawContentWiFi();
  else if (tab == 3) drawContentSys();
  else if (tab == 4) drawContentMsg();
  else if (tab == 5) drawContentLang();
  else drawContentGps();
}

/** Meshtastic-style: rate limit, hash skip, partial/full. forceUpdate=true — пропуск rate limit (действие пользователя). */
static bool performDisplayUpdate(int tab, bool isResponsive, bool forceUpdate = false) {
  uint32_t now = millis();
  if ((now - s_lastDisplayEnd) < EINK_COOLDOWN_HW_MS) return false;
  if (!forceUpdate && s_previousRunMs <= now) {
    if (isResponsive && (now - s_previousRunMs) < EINK_RATE_LIMIT_RESPONSIVE_MS) return false;
    if (!isResponsive && (now - s_previousRunMs) < EINK_RATE_LIMIT_BACKGROUND_MS) return false;
  }

  drawScreenContent(tab);
  uint32_t hash = computeContentHash(tab);
  if (!forceUpdate && hash == s_previousImageHash && tab != 3 && tab != 5) {
    s_previousRunMs = now;  // не спамить — иначе CPU burn при неизменном контенте
    return false;
  }

  // Пickers и смена контекста — всегда full refresh (против ghosting)
  bool usePartial = isResponsive && (s_fastRefreshCount < EINK_LIMIT_FASTREFRESH) && !forceUpdate;
  ensureCooldownBeforeDisplay();
  doDisplay(usePartial);

  s_lastDisplayEnd = millis();
  s_previousRunMs = now;
  s_previousImageHash = hash;
  if (usePartial) s_fastRefreshCount++;
  else s_fastRefreshCount = 0;
  return true;
}

static void drawScreen(int tab) {
  drawScreenContent(tab);
  ensureCooldownBeforeDisplay();
  doDisplay(false);
  s_lastDisplayEnd = millis();
  s_previousRunMs = millis();
  s_previousImageHash = computeContentHash(tab);
  s_fastRefreshCount = 0;
}

void displayRequestInfoRedraw() {
  displayWakeRequest();
  s_needRedrawInfo = true;
}

void displaySetLastMsg(const char* fromHex, const char* text) {
  displayWakeRequest();
  s_lastActivityTime = millis();
  if (fromHex) { strncpy(s_lastMsgFrom, fromHex, 16); s_lastMsgFrom[16] = '\0'; }
  if (text) { strncpy(s_lastMsgText, text, 63); s_lastMsgText[63] = '\0'; }
  if (s_currentScreen == 4) s_needRedrawMsg = true;  // отложить draw — не блокировать handlePacket
}

void displayShowScreen(int screen) {
  s_currentScreen = screen % N_TABS;
  s_previousRunMs = 0;
  ensureCooldownBeforeDisplay();
  drawScreen(s_currentScreen);
}

bool displayUpdate() {
  if (!disp) return false;
  uint32_t now = millis();

  // Сначала кнопка — иначе pending redraw блокирует реакцию на нажатие до 600ms
  bool btn = BTN_PRESSED;
  if (btn) {
    if (!s_lastButton) { s_lastButton = true; s_pressStart = now; }
  } else {
    if (s_lastButton) {
      uint32_t hold = now - s_pressStart;
      bool isLong = (hold >= LONG_PRESS_MS);
      bool isShort = (hold >= MIN_PRESS_MS && hold < LONG_PRESS_MS);  // 80–500ms = tab, 500ms+ = menu (без dead zone 350–500)
      // LED feedback — устройство реагирует (против ощущения зависания)
      pinMode(LED_PIN, OUTPUT);
      digitalWrite(LED_PIN, HIGH);
      delay(20);
      digitalWrite(LED_PIN, LOW);
      if (isShort) {
        ensureCooldownBeforeDisplay();  // ждём cooldown, иначе performDisplayUpdate вернёт false
        s_currentScreen = (s_currentScreen + 1) % N_TABS;
        if (!performDisplayUpdate(s_currentScreen, true, true)) {
          drawScreen(s_currentScreen);  // fallback — при сбое принудительный redraw
        }
      } else if (isLong) {
        ensureCooldownBeforeDisplay();  // перед picker/selftest — гарантировать готовность дисплея
        if (s_currentScreen == 0) {
          displayShowRegionPicker();
          s_previousImageHash = 0;  // сброс — принудительный redraw
          drawScreen(s_currentScreen);
        } else if (s_currentScreen == 5) {
          displayShowLanguagePicker();
          s_previousImageHash = 0;
          drawScreen(s_currentScreen);
        } else if (s_currentScreen == 6 && gps::isPresent()) {
          gps::toggle();
          s_previousImageHash = 0;
          drawScreen(s_currentScreen);
        } else if (s_currentScreen == 3) {
          selftest::run(nullptr);
          s_previousImageHash = 0;
          drawScreen(s_currentScreen);
        }
      }
      s_lastButton = false;
      for (int i = 0; i < 4; i++) { delay(50); yield(); }  // 200ms чанками — BLE/radio не теряют связь
      return true;
    }
  }

  if (s_needRedrawInfo && s_currentScreen == 1) {
    if (performDisplayUpdate(1, true, true)) s_needRedrawInfo = false;
    return false;
  }
  if (s_needRedrawMsg && s_currentScreen == 4) {
    if (performDisplayUpdate(4, true, true)) s_needRedrawMsg = false;
    return false;
  }
  if ((now - s_lastDisplayEnd) < EINK_COOLDOWN_HW_MS) return false;

  if (s_currentScreen == 0 || s_currentScreen == 1 || s_currentScreen == 2 || s_currentScreen == 6) {
    performDisplayUpdate(s_currentScreen, false);
  }
  return false;
}
