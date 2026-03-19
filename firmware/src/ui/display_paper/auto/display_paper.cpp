/**
 * RiftLink Display — E-Ink 2.13" Heltec Wireless Paper
 * Определение как Meshtastic: einkDetect.h — RST LOW, read BUSY. LOW→FC1(V1.1), HIGH→E0213A367(V1.2)
 * Пины: CS=4, BUSY=7, DC=5, RST=6, SCLK=3, MOSI=2, VEXT=45
 */

#include "../../display.h"
#include "../../display_tabs.h"
#include "bootscreen_paper.h"
#include "locale/locale.h"
#include "selftest/selftest.h"
#include "node/node.h"
#include "gps/gps.h"
#include "region/region.h"
#include "neighbors/neighbors.h"
#include "wifi/wifi.h"
#include "ota/ota.h"
#include "powersave/powersave.h"
#include "telemetry/telemetry.h"
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
static uint32_t s_fullRefreshCount = 0;   // счётчик full refresh — периодический reinit
static bool s_lastWasFullRefresh = false; // после full+hibernate первый REDRAW — двойной partial
static bool s_panelHibernating = false;  // панель в hibernate — не вызывать повторно
static bool s_hibernateFromIdle = false; // hibernate из 30с idle (без full) — при wake нужен full
// volatile — s_needRedrawInfo пишется из BLE task (displayRequestInfoRedraw), читается в main loop
static volatile bool s_needRedrawMsg = false;
static volatile bool s_needRedrawInfo = false;
static bool s_buttonPolledExternally = false;
static char s_lastMsgFrom[17] = {0};
static char s_lastMsgText[64] = {0};

// Meshtastic-style rate limiting (EInkDynamicDisplay)
#define EINK_RATE_LIMIT_BACKGROUND_MS  30000  // BACKGROUND: min 30s между обновлениями
#define EINK_RATE_LIMIT_RESPONSIVE_MS  1000   // RESPONSIVE (кнопка, сообщение): min 1s
#define EINK_LIMIT_FASTREFRESH         5      // после N partial — принудительный full (против ghosting)
#define EINK_COOLDOWN_HW_MS            600    // аппаратный минимум между display() — иначе зависает
#define EINK_IDLE_HIBERNATE_MS         30000  // 30 с неактивности → hibernate (панель не потребляет)
#define EINK_REINIT_AFTER_N            3      // только FC1: каждые N full refresh — RST+init. BN/B73: hibernate() уже даёт reinit при wake
#define PICKER_CONFIRM_MS              6000   // авто-принятие в пикерах (lang, region): e-ink медленный, 2.5s мало

#define BTN_ACTIVE_LOW 1
#define BTN_PRESSED (digitalRead(BUTTON_PIN) == (BTN_ACTIVE_LOW ? LOW : HIGH))
#define SHORT_PRESS_MS 350   // граница short/long (используется в waitButtonPressWithType)
#define LONG_PRESS_MS  500
#define MIN_PRESS_MS   80   // защита от дребезга
#define POST_PRESS_DEBOUNCE_MS 200  // пауза после обработки нажатия — против двойного срабатывания
#define LED_PIN 35  // Heltec V3/V4 — мигание при нажатии (обратная связь)
enum PressType { PRESS_NONE = 0, PRESS_SHORT = 1, PRESS_LONG = 2 };

#if defined(ESP32)
#include <esp_task_wdt.h>
#include "../../../radio/radio.h"
static void busyCallback(const void*) {
  esp_task_wdt_status_t st;
  if (esp_task_wdt_status(NULL, &st) == ESP_OK) esp_task_wdt_reset();
}
// Глобальный SPI (FSPI) вместо HSPI — workaround hang в beginTransaction на ESP32-S3
#define EINK_USE_GLOBAL_SPI 1
#if !EINK_USE_GLOBAL_SPI
static SPIClass hspi(HSPI);
#endif
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
  display_tabs::ContentTab ct = display_tabs::contentForTab(tab);
  uint32_t h = (uint32_t)tab * 31;
  if (ct == display_tabs::CT_MAIN) {
    const uint8_t* id = node::getId();
    h ^= id[0] ^ (id[1] << 8) ^ (id[2] << 16) ^ (id[3] << 24);
    h ^= (uint32_t)region::getChannel() * 7;
    h ^= (uint32_t)(region::getFreq() * 100) * 11;
    h ^= (uint32_t)neighbors::getCount() * 13;
  } else if (ct == display_tabs::CT_INFO) {
    char nick[17];
    node::getNickname(nick, sizeof(nick));
    for (int i = 0; nick[i] && i < 16; i++) h = h * 31 + (uint8_t)nick[i];
    h ^= (uint32_t)(region::getFreq() * 100) * 17;
  } else if (ct == display_tabs::CT_WIFI) {
    char ssid[24] = {0}, ip[20] = {0};
    wifi::getStatus(ssid, sizeof(ssid), ip, sizeof(ip));
    for (int i = 0; ssid[i] && i < 23; i++) h = h * 31 + (uint8_t)ssid[i];
    for (int i = 0; ip[i] && i < 19; i++) h = h * 31 + (uint8_t)ip[i];
    h ^= ota::isActive() ? 0x1234 : 0;
  } else if (ct == display_tabs::CT_MSG) {
    for (int i = 0; s_lastMsgFrom[i] && i < 16; i++) h = h * 31 + (uint8_t)s_lastMsgFrom[i];
    for (int i = 0; s_lastMsgText[i] && i < 63; i++) h = h * 31 + (uint8_t)s_lastMsgText[i];
  } else if (ct == display_tabs::CT_GPS && gps::isPresent()) {
    h ^= gps::isEnabled() ? 1 : 0;
    h ^= gps::hasFix() ? 2 : 0;
    h ^= (uint32_t)gps::getSatellites() * 19;
    h ^= (uint32_t)(gps::getLat() * 10000) * 23;
    h ^= (uint32_t)(gps::getLon() * 10000) * 29;
  }
  return h;
}

static void ensureCooldownBeforeDisplay();
#if defined(ESP32) && EINK_USE_GLOBAL_SPI
static void selectDisplaySPI();
static void releaseDisplaySPI();
#endif

/** Периодический reinit — только для FC1 (LCMEN2R13EFC1). BN/B73: hibernate() уже даёт _reset() при wake. */
static void maybeDisplayReinit() {
  if (!dispFC1) return;  // BN и B73: hibernate = reinit при каждом full refresh
  s_fullRefreshCount++;
  if (s_fullRefreshCount < EINK_REINIT_AFTER_N) return;
  s_fullRefreshCount = 0;
  Serial.println("[RiftLink] E-Ink reinit (periodic, FC1)");
  ensureCooldownBeforeDisplay();
  digitalWrite(EINK_RST, LOW);
  delay(20);
  digitalWrite(EINK_RST, HIGH);
  delay(200);
  dispFC1->init(0, true, 20, false);
  s_lastDisplayEnd = millis();
}

/** Meshtastic: после full refresh вызывать hibernate() — powerOff + deep sleep. Следующий display() сделает wake.
 *  powersave OFF: не хибанируем, s_lastWasFullRefresh=false — иначе всегда full вместо partial. */
static void doDisplayHibernate(bool wasFull) {
  if (!wasFull) {
    s_lastWasFullRefresh = false;
    return;
  }
  if (!powersave::isEnabled()) {
    s_lastWasFullRefresh = false;  // без powersave — следующий partial ок
    return;
  }
  if (s_panelHibernating) return;  // уже в hibernate — не вызывать повторно
  s_lastWasFullRefresh = true;
  s_panelHibernating = true;
  if (dispBN) dispBN->epd2.hibernate();
  else if (dispFC1) dispFC1->epd2.hibernate();
  else if (dispB73) dispB73->epd2.hibernate();
}

static void doDisplay(bool partial) {
  s_panelHibernating = false;  // display() разбудит панель если была в hibernate
#if defined(ESP32) && EINK_USE_GLOBAL_SPI
  selectDisplaySPI();
#endif
  if (!partial) maybeDisplayReinit();
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
  doDisplayHibernate(!partial);
#if defined(ESP32) && EINK_USE_GLOBAL_SPI
  releaseDisplaySPI();
#endif
}

/** 30 с неактивности → hibernate (панель не потребляет). Проверяем s_panelHibernating — не вызывать повторно. */
static void hibernateIfIdle() {
  uint32_t now = millis();
  if ((now - s_lastActivityTime) < EINK_IDLE_HIBERNATE_MS) return;
  if (s_panelHibernating) return;
  if ((now - s_lastDisplayEnd) < EINK_COOLDOWN_HW_MS) return;  // не прерывать cooldown
  s_panelHibernating = true;
  s_lastWasFullRefresh = true;
  s_hibernateFromIdle = true;  // wake без full перед этим — нужен full при выходе
  if (dispBN) dispBN->epd2.hibernate();
  else if (dispFC1) dispFC1->epd2.hibernate();
  else if (dispB73) dispB73->epd2.hibernate();
}

#if EINK_USE_GLOBAL_SPI
// Пины LoRa (должны совпадать с radio.cpp) — для переключения SPI после display
#define LORA_SCK  9
#define LORA_MISO 11
#define LORA_MOSI 10
#define LORA_NSS  8
static void selectDisplaySPI() {
  radio::takeMutex(portMAX_DELAY);
  SPI.begin(EINK_SCLK, -1, EINK_MOSI, EINK_CS);  // -1 = MISO не используется (3-wire E-Ink)
}
static void releaseDisplaySPI() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  radio::releaseMutex();
}
#endif

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

  pinMode(EINK_CS, OUTPUT);   // иначе __digitalWrite: IO 4 is not set as GPIO (Arduino 3.x)
  pinMode(EINK_DC, OUTPUT);
  pinMode(EINK_BUSY, INPUT);
  pinMode(EINK_RST, OUTPUT);

#if defined(ESP32)
#if EINK_USE_GLOBAL_SPI
  SPI.begin(EINK_SCLK, -1, EINK_MOSI, EINK_CS);  // -1 = MISO не используется (3-wire E-Ink)
  delay(100);
#else
  hspi.end();
  delay(10);
  hspi.begin(EINK_SCLK, EINK_MOSI, EINK_MOSI, EINK_CS);
  delay(100);
#endif
#endif

  // RST reset как в GxEPD2
  digitalWrite(EINK_RST, LOW);
  delay(20);
  digitalWrite(EINK_RST, HIGH);
  delay(200);

#if defined(USE_EINK_FORCE_BN)
  Serial.println("[RiftLink] E-Ink init (forced BN)...");
  dispBN = new DispBN(GxEPD2_213_BN(EINK_CS, EINK_DC, EINK_RST, EINK_BUSY, SPI));
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
    dispFC1 = new DispFC1(GxEPD2_213_FC1(EINK_CS, EINK_DC, EINK_RST, EINK_BUSY, SPI));
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
    dispB73 = new DispB73(GxEPD2_213_E0213A367(EINK_CS, EINK_DC, EINK_RST, EINK_BUSY, SPI));
    dispB73->epd2.setBusyCallback(busyCallback);
    Serial.println("[RiftLink] E-Ink epd init...");
    dispB73->init(0, true, 20, false);
    Serial.println("[RiftLink] E-Ink epd init ok");
    dispB73->setRotation(3);
    dispB73->fillScreen(GxEPD_WHITE);
    dispB73->setTextColor(GxEPD_BLACK);
    dispB73->setTextSize(1);
    dispB73->cp437(true);
    // E0213A367: BUSY=HIGH когда занят; pull-down при idle — иначе плавающий пин даёт зависание
    pinMode(EINK_BUSY, INPUT_PULLDOWN);
    Serial.println("[RiftLink] E-Ink display refresh (~3-5s)...");
    dispB73->display(false);
    Serial.println("[RiftLink] E-Ink display ok");
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
  s_fastRefreshCount = 0;
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
  s_fastRefreshCount = 0;
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
  s_fastRefreshCount = 0;
  int pickLang = locale::getLang();
  uint32_t lastPress = millis();
  const uint32_t CONFIRM_MS = PICKER_CONFIRM_MS;
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
    bool usePartial = (s_fastRefreshCount < EINK_LIMIT_FASTREFRESH);
    if (s_lastWasFullRefresh) {
      s_hibernateFromIdle = false;
      s_lastWasFullRefresh = false;
      doDisplay(false);
      s_fastRefreshCount = 0;
    } else {
      doDisplay(usePartial);
      if (usePartial) s_fastRefreshCount++; else s_fastRefreshCount = 0;
    }
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
  const uint32_t CONFIRM_MS = PICKER_CONFIRM_MS;
  while (1) {
    yield();
    ensureCooldownBeforeDisplay();
    disp->fillScreen(GxEPD_WHITE);
    disp->setTextColor(GxEPD_BLACK);
    disp->setTextSize(1);
    drawTruncRaw(4, 8, locale::getForDisplay("select_country"), 24);
    // Варианты как в language picker: [EU] UK RU US AU — выбран в скобках
    disp->setCursor(4, 36);
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
    drawTruncRaw(4, 56, locale::getForDisplay("short_long_hint"), 28);
    bool usePartial = (s_fastRefreshCount < EINK_LIMIT_FASTREFRESH);
    if (s_lastWasFullRefresh) {
      s_hibernateFromIdle = false;
      s_lastWasFullRefresh = false;
      doDisplay(false);
      s_fastRefreshCount = 0;
    } else {
      doDisplay(usePartial);
      if (usePartial) s_fastRefreshCount++; else s_fastRefreshCount = 0;
    }
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
  int nTabs = display_tabs::getTabCount();
  int tabW = SCREEN_WIDTH / nTabs;
  int iconSize = ICON_W * ICON_SCALE;
  for (int i = 0; i < nTabs; i++) {
    const uint8_t* icon = display_tabs::getIconForTab(i);
    int x = i * tabW;
    int iconX = x + (tabW - iconSize) / 2;
    int iconY = (TAB_H - iconSize) / 2;
    if (i == activeTab) {
      disp->fillRect(x + 1, 1, tabW - 2, TAB_H - 2, GxEPD_BLACK);
      drawIconScaled(iconX, iconY, icon, GxEPD_WHITE);
    } else {
      drawIconScaled(iconX, iconY, icon, GxEPD_BLACK);
    }
    if (i < nTabs - 1) {
      disp->drawFastVLine(x + tabW, 2, TAB_H - 2, GxEPD_BLACK);
    }
  }
  disp->drawFastHLine(0, TAB_H, SCREEN_WIDTH, GxEPD_BLACK);
  disp->drawRect(0, CONTENT_Y, SCREEN_WIDTH, CONTENT_H, GxEPD_BLACK);
}

/** Li-ion: 3.0V=0%, 4.2V=100%, линейно. 0 mV → — (нет батареи).
 *  Зарядка без батареи: VBAT ~3.9V (Heltec) → показываем 100%. */
static int batteryPercent(uint16_t mv) {
  if (mv < 3000) return -1;
  if (mv >= 3850) return 100;  // 3.85V+ = полный заряд или зарядка без батареи
  int pct = (int)((mv - 3000) / 12);  // 1200mV / 100
  if (pct > 100) pct = 100;
  return pct;
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
  int pct = batteryPercent(telemetry::readBatteryMv());
  const char* batLabel = locale::getForDisplay("battery");
  if (pct >= 0) snprintf(buf, sizeof(buf), "%s %d%%", batLabel, pct);
  else snprintf(buf, sizeof(buf), "%s --", batLabel);
  drawTruncRaw(CONTENT_X, CONTENT_Y + 50, buf, MAX_LINE_CHARS);
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
  switch (display_tabs::contentForTab(tab)) {
    case display_tabs::CT_MAIN: drawContentMain(); break;
    case display_tabs::CT_INFO: drawContentInfo(); break;
    case display_tabs::CT_WIFI: drawContentWiFi(); break;
    case display_tabs::CT_SYS: drawContentSys(); break;
    case display_tabs::CT_MSG: drawContentMsg(); break;
    case display_tabs::CT_LANG: drawContentLang(); break;
    case display_tabs::CT_GPS: drawContentGps(); break;
  }
}

/** Meshtastic-style: rate limit, hash skip, partial/full. forceUpdate=true — пропуск rate limit (действие пользователя). */
/** При 0 соседях — реже обновляем e-ink, чтобы не блокировать SPI и не мешать приёму HELLO. */
static bool performDisplayUpdate(int tab, bool isResponsive, bool forceUpdate = false) {
  uint32_t now = millis();
  if ((now - s_lastDisplayEnd) < EINK_COOLDOWN_HW_MS) return false;
  if (!forceUpdate && s_previousRunMs <= now) {
    uint32_t bgLimit = EINK_RATE_LIMIT_BACKGROUND_MS;
    if (!isResponsive && neighbors::getCount() == 0)
      bgLimit = 60000;  // 60s при поиске соседей — меньше блокировок SPI
    if (isResponsive && (now - s_previousRunMs) < EINK_RATE_LIMIT_RESPONSIVE_MS) return false;
    if (!isResponsive && (now - s_previousRunMs) < bgLimit) return false;
  }

  drawScreenContent(tab);
  uint32_t hash = computeContentHash(tab);
  display_tabs::ContentTab ct = display_tabs::contentForTab(tab);
  if (!forceUpdate && hash == s_previousImageHash && ct != display_tabs::CT_SYS && ct != display_tabs::CT_LANG) {
    s_previousRunMs = now;  // не спамить — иначе CPU burn при неизменном контенте
    return false;
  }

  // Partial везде; каждые EINK_LIMIT_FASTREFRESH — full для очистки ghosting
  bool usePartial = (s_fastRefreshCount < EINK_LIMIT_FASTREFRESH);
  if (s_lastWasFullRefresh) {
    // wake из сна: всегда full — иначе ghosting
    s_hibernateFromIdle = false;
    s_lastWasFullRefresh = false;
    s_previousImageHash = 0;
    ensureCooldownBeforeDisplay();
    doDisplay(false);
    s_fastRefreshCount = 0;
  } else {
    ensureCooldownBeforeDisplay();
    doDisplay(usePartial);
    if (usePartial) s_fastRefreshCount++;
    else s_fastRefreshCount = 0;
  }

  s_lastDisplayEnd = millis();
  s_previousRunMs = now;
  s_previousImageHash = hash;
  s_lastActivityTime = now;  // обновление данных — активность (откладывает hibernate)
  return true;
}

static void drawScreen(int tab, bool forceFull = false) {
  drawScreenContent(tab);
  // powersave: всегда full, не зависеть от s_lastWasFullRefresh — иначе вторая смена вкладки перестаёт рисоваться
  if (forceFull) {
    s_hibernateFromIdle = false;
    s_lastWasFullRefresh = false;
    s_previousImageHash = 0;
    ensureCooldownBeforeDisplay();
    doDisplay(false);
    s_fastRefreshCount = 0;
  } else {
    bool usePartial = (s_fastRefreshCount < EINK_LIMIT_FASTREFRESH);
    if (s_lastWasFullRefresh) {
      // wake из сна: всегда full — иначе ghosting
      s_hibernateFromIdle = false;
      s_lastWasFullRefresh = false;
      s_previousImageHash = 0;
      ensureCooldownBeforeDisplay();
      doDisplay(false);
      s_fastRefreshCount = 0;
    } else {
      ensureCooldownBeforeDisplay();
      doDisplay(usePartial);
      if (usePartial) s_fastRefreshCount++;
      else s_fastRefreshCount = 0;
    }
  }
  s_lastDisplayEnd = millis();
  s_previousRunMs = millis();
  s_previousImageHash = computeContentHash(tab);
  s_lastActivityTime = millis();  // отрисовка — активность (откладывает hibernate)
}

void displaySetButtonPolledExternally(bool on) {
  s_buttonPolledExternally = on;
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
  if (display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_MSG) s_needRedrawMsg = true;
}

void displayShowScreen(int screen) {
  s_lastActivityTime = millis();
  int nTabs = display_tabs::getTabCount();
  if (screen >= nTabs) screen = nTabs - 1;
  s_currentScreen = screen;
  s_previousRunMs = 0;
  bool forceFull = powersave::isEnabled();  // режим экономии: full → hibernate после каждой вкладки
  drawScreen(s_currentScreen, forceFull);
}

void displayShowScreenForceFull(int screen) {
  int nTabs = display_tabs::getTabCount();
  if (screen >= nTabs) screen = nTabs - 1;
  s_currentScreen = screen;
  s_previousRunMs = 0;
  drawScreen(s_currentScreen, true);  // full refresh — против ghosting при смене вкладки
}

int displayGetCurrentScreen() {
  return s_currentScreen;
}

int displayGetNextScreen(int current) {
  return (current + 1) % display_tabs::getTabCount();
}

void displayOnLongPress(int screen) {
  s_lastActivityTime = millis();
  ensureCooldownBeforeDisplay();
  display_tabs::ContentTab ct = display_tabs::contentForTab(screen);
  if (ct == display_tabs::CT_MAIN) {
    displayShowRegionPicker();
    s_previousImageHash = 0;
    drawScreen(s_currentScreen);
  } else if (ct == display_tabs::CT_LANG) {
    displayShowLanguagePicker();
    s_previousImageHash = 0;
    drawScreen(s_currentScreen);
  } else if (ct == display_tabs::CT_GPS && gps::isPresent()) {
    gps::toggle();
    s_previousImageHash = 0;
    drawScreen(s_currentScreen);
  } else if (ct == display_tabs::CT_SYS) {
    selftest::run(nullptr);
    s_previousImageHash = 0;
    drawScreen(s_currentScreen);
  }
}

bool displayUpdate() {
  if (!disp) return false;
  uint32_t now = millis();

  if (!s_buttonPolledExternally) {
    bool btn = BTN_PRESSED;
    if (btn) {
      if (!s_lastButton) { s_lastButton = true; s_pressStart = now; }
    } else if (s_lastButton) {
      uint32_t hold = now - s_pressStart;
      bool isLong = (hold >= LONG_PRESS_MS);
      bool isShort = (hold >= MIN_PRESS_MS && hold < LONG_PRESS_MS);
      if (isShort || isLong) {
        pinMode(LED_PIN, OUTPUT);
        digitalWrite(LED_PIN, HIGH);
        delay(20);
        digitalWrite(LED_PIN, LOW);
      }
      if (isShort) {
        s_lastActivityTime = millis();
        ensureCooldownBeforeDisplay();
        s_currentScreen = displayGetNextScreen(s_currentScreen);
        bool forceFull = powersave::isEnabled();  // режим экономии: full → hibernate после каждой вкладки
        if (forceFull) drawScreen(s_currentScreen, true);
        else if (!performDisplayUpdate(s_currentScreen, true, true)) drawScreen(s_currentScreen, false);
      } else if (isLong) displayOnLongPress(s_currentScreen);
      s_lastButton = false;
      for (int i = 0; i < 4; i++) { delay(50); yield(); }
      return true;
    }
  }

  // Обновляем только если пользователь на вкладке с этими данными — иначе смысла нет
  if (s_needRedrawInfo && s_currentScreen == 1) {
    if (performDisplayUpdate(1, true, true)) s_needRedrawInfo = false;
    else {
      drawScreen(1, powersave::isEnabled());  // fallback при cooldown — ждём и рисуем
      s_needRedrawInfo = false;
    }
    return false;
  }
  if (s_needRedrawMsg && display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_MSG) {
    if (performDisplayUpdate(s_currentScreen, true, true)) s_needRedrawMsg = false;
    else {
      drawScreen(s_currentScreen, powersave::isEnabled());  // fallback при cooldown — ждём и рисуем
      s_needRedrawMsg = false;
    }
    return false;
  }
  if ((now - s_lastDisplayEnd) < EINK_COOLDOWN_HW_MS) return false;

  hibernateIfIdle();  // 30 с неактивности → панель в hibernate

  // Периодика только для вкладок с живыми данными. Lang, Sys, Msg — только по событию
  display_tabs::ContentTab ct = display_tabs::contentForTab(s_currentScreen);
  const bool tabHasLiveData = (ct == display_tabs::CT_MAIN || ct == display_tabs::CT_INFO || ct == display_tabs::CT_WIFI || ct == display_tabs::CT_GPS);
  if (tabHasLiveData) {
    performDisplayUpdate(s_currentScreen, false);
  }
  return false;
}
