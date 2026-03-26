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
#include "powersave/powersave.h"
#include "radio/radio.h"
#include "radio_mode/radio_mode.h"
#include "ble/ble.h"
#include "async_tasks.h"
#include "telemetry/telemetry.h"
#include <freertos/FreeRTOS.h>
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
/** Подряд успешных fast/partial на вкладках НЕ MSG; при >= EINK_LIMIT_FASTREFRESH следующий кадр — full. */
static uint32_t s_fastRefreshCount = 0;
/** Только MSG: подряд partial до full (см. EINK_MSG_PARTIAL_BEFORE_FULL), отдельно от s_fastRefreshCount. */
static uint32_t s_msgPartialStreak = 0;
static uint32_t s_previousImageHash = 0;  // хеш последнего отображённого контента (пропуск дубликатов)
static uint32_t s_fullRefreshCount = 0;   // счётчик full refresh — периодический reinit
static bool s_lastWasFullRefresh = false; // после full+hibernate первый REDRAW — двойной partial
static bool s_panelHibernating = false;  // панель в hibernate — не вызывать повторно
static bool s_hibernateFromIdle = false; // hibernate из 30с idle (без full) — при wake нужен full
// volatile — s_needRedrawInfo пишется из BLE task (displayRequestInfoRedraw), читается в main loop
static volatile bool s_needRedrawMsg = false;
static volatile bool s_needRedrawInfo = false;
static bool s_buttonPolledExternally = false;
static volatile bool s_menuActive = false;
static char s_lastMsgFrom[17] = {0};
static char s_lastMsgText[64] = {0};

// Meshtastic-style rate limiting (EInkDynamicDisplay)
#define EINK_RATE_LIMIT_BACKGROUND_MS  30000  // BACKGROUND: min 30s между обновлениями
#define EINK_RATE_LIMIT_RESPONSIVE_MS  1000   // RESPONSIVE (кнопка, сообщение): min 1s
/*
 * Partial → full (анти-ghosting):
 * - Общий счётчик: s_fastRefreshCount на вкладках MAIN/INFO/NET/GPS/SYS (не MSG).
 *   После EINK_LIMIT_FASTREFRESH подряд успешных fast/partial на ЭТОЙ вкладке следующий кадр — full
 *   (см. usePartialForTab / applyPartialStreakAfter). После full счётчик обнуляется.
 * - MSG отдельно: s_msgPartialStreak и EINK_MSG_PARTIAL_BEFORE_FULL (0 = на MSG всегда full).
 * - Только выход с MSG: onPaperTabSwitch() ставит s_fastRefreshCount = EINK_LIMIT_FASTREFRESH,
 *   чтобы первый кадр на другой вкладке был full (иначе не накладывался контент MSG). Обычная смена вкладок — без этого.
 */
#define EINK_LIMIT_FASTREFRESH         4      // порог «подряд partial» на не-MSG → затем принудительный full
/** На вкладке сообщений: сколько подряд допустимо fast/partial до full. 0 = только full (минимум ghosting, дольше обновление). */
#define EINK_MSG_PARTIAL_BEFORE_FULL   0
#define EINK_COOLDOWN_HW_MS            600    // аппаратный минимум между display() — иначе зависает
/** 1 = не использовать fast LUT (всегда display(false)): медленнее, но если картинка «плывёт» даже с setFullWindow */
#ifndef EINK_FORCE_FULL_LUT
#define EINK_FORCE_FULL_LUT 0
#endif
#define EINK_IDLE_HIBERNATE_MS         30000  // 30 с неактивности → hibernate (панель не потребляет)
#define EINK_REINIT_AFTER_N            3      // только FC1: каждые N full refresh — RST+init. BN/B73: hibernate() уже даёт reinit при wake
#define PICKER_CONFIRM_MS              8000   // авто-принятие в пикерах (lang, region, powersave): e-ink медленный

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
  if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
}
// Глобальный SPI (FSPI) вместо HSPI — workaround hang в beginTransaction на ESP32-S3
#define EINK_USE_GLOBAL_SPI 1
#if !EINK_USE_GLOBAL_SPI
static SPIClass hspi(HSPI);
#endif
#endif

/** Определение панели как Meshtastic (einkDetect.h): RST LOW, read BUSY. LOW→FC1(V1.1), HIGH→E0213A367(V1.2) */
enum EInkModel { EINK_FC1, EINK_E0213A367 };
static EInkModel s_einkModelDetected = EINK_FC1;

static void applyBusyPinMode() {
  // BUSY active-low panels idle HIGH, active-high panels idle LOW.
  // Keep line pulled to idle to avoid floating BUSY and long wait loops.
  if (s_einkModelDetected == EINK_E0213A367) pinMode(EINK_BUSY, INPUT_PULLDOWN);
  else pinMode(EINK_BUSY, INPUT_PULLUP);
}

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
    h ^= (uint32_t)(region::getFreq() * 100) * 11;
    h ^= (uint32_t)radio::getSpreadingFactor() * 7;
    h ^= (uint32_t)radio::getModemPreset() * 23;
    h ^= (uint32_t)(radio::getBandwidth()) * 29;
    h ^= (uint32_t)neighbors::getCount() * 13;
    h ^= (uint32_t)telemetry::batteryPercent() * 19;
  } else if (ct == display_tabs::CT_INFO) {
    h ^= (uint32_t)neighbors::getCount() * 17;
    int n = neighbors::getCount();
    for (int i = 0; i < n && i < 7; i++) h ^= (uint32_t)(neighbors::getRssi(i) + 200) * (i + 3);
  } else if (ct == display_tabs::CT_NET) {
    h ^= (uint32_t)radio_mode::current() * 37;
    h ^= ble::isConnected() ? 0xBBBB : 0;
    if (radio_mode::current() == radio_mode::WIFI) {
      char ssid[24] = {0}, ip[20] = {0};
      wifi::getStatus(ssid, sizeof(ssid), ip, sizeof(ip));
      for (int i = 0; ssid[i] && i < 23; i++) h = h * 31 + (uint8_t)ssid[i];
      for (int i = 0; ip[i] && i < 19; i++) h = h * 31 + (uint8_t)ip[i];
    }
  } else if (ct == display_tabs::CT_MSG) {
    for (int i = 0; s_lastMsgFrom[i] && i < 16; i++) h = h * 31 + (uint8_t)s_lastMsgFrom[i];
    for (int i = 0; s_lastMsgText[i] && i < 63; i++) h = h * 31 + (uint8_t)s_lastMsgText[i];
  } else if (ct == display_tabs::CT_GPS) {
    h ^= gps::isEnabled() ? 1 : 0;
    h ^= gps::hasFix() ? 2 : 0;
    h ^= gps::hasPhoneSync() ? 4 : 0;
    h ^= (uint32_t)gps::getSatellites() * 19;
    h ^= (uint32_t)(gps::getLat() * 10000) * 23;
    h ^= (uint32_t)(gps::getLon() * 10000) * 29;
  }
  return h;
}

/** При уходе с вкладки сообщений: первый кадр на новой вкладке — full (счётчик → порог). Иначе смена вкладок — как обычно (partial по лимиту). */
static void onPaperTabSwitch(int prevTab, int newTab) {
  if (prevTab == newTab) return;
  if (display_tabs::contentForTab(prevTab) == display_tabs::CT_MSG &&
      display_tabs::contentForTab(newTab) != display_tabs::CT_MSG) {
    s_msgPartialStreak = 0;
    s_fastRefreshCount = EINK_LIMIT_FASTREFRESH;  // следующий draw на не-MSG — full, не каждый клик
  }
}

static bool usePartialForTab(display_tabs::ContentTab ct) {
  if (ct == display_tabs::CT_MSG)
    return s_msgPartialStreak < EINK_MSG_PARTIAL_BEFORE_FULL;
  return s_fastRefreshCount < EINK_LIMIT_FASTREFRESH;
}

static void applyPartialStreakAfter(display_tabs::ContentTab ct, bool usePartial) {
  if (ct == display_tabs::CT_MSG) {
    if (usePartial) s_msgPartialStreak++;
    else s_msgPartialStreak = 0;
  } else {
    if (usePartial) s_fastRefreshCount++;
    else s_fastRefreshCount = 0;
  }
}

static void ensureCooldownBeforeDisplay();
#if defined(ESP32) && EINK_USE_GLOBAL_SPI
static bool selectDisplaySPI();
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

/** @return false если SPI/mutex недоступны — вызывающий не должен обновлять хеш/timestamps/s_lastWasFullRefresh */
static bool doDisplay(bool partial) {
  s_panelHibernating = false;  // display() разбудит панель если была в hibernate
#if defined(ESP32) && EINK_USE_GLOBAL_SPI
  if (!selectDisplaySPI()) return false;  // нет mutex — не рисуем на чужом SPI
#endif
  if (!dispBN && !dispFC1 && !dispB73) {
#if defined(ESP32) && EINK_USE_GLOBAL_SPI
    releaseDisplaySPI();
#endif
    return false;
  }
  const bool fastLut = partial && (EINK_FORCE_FULL_LUT == 0);
  if (!fastLut) maybeDisplayReinit();
  // Всегда full window при отрисовке всего буфера: setPartialWindow(0,0,w,h)+display()
  // на SSD16xx даёт артефакты «съехавшей» картинки; fast/full LUT — аргумент display(fastLut).
  if (dispBN) {
    dispBN->setFullWindow();
    dispBN->display(fastLut);
  } else if (dispFC1) {
    dispFC1->setFullWindow();
    dispFC1->display(fastLut);
  } else if (dispB73) {
    dispB73->setFullWindow();
    dispB73->display(fastLut);
  }
  doDisplayHibernate(!fastLut);
#if defined(ESP32) && EINK_USE_GLOBAL_SPI
  releaseDisplaySPI();
#endif
  return true;
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
static bool s_einkSpiRadioLocked = false;
/** true — сессия с radioScheduler (семафоры granted/done); иначе только legacy mutex. */
static bool s_einkDisplaySpiSessionActive = false;

static bool selectDisplaySPI() {
  s_einkSpiRadioLocked = false;
  s_einkDisplaySpiSessionActive = false;
#if defined(USE_EINK)
  // Сначала пауза радио в планировщике (standby), затем один takeMutex на время SPI E-Ink.
  bool granted = asyncRequestDisplaySpiSession(pdMS_TO_TICKS(5000));
  // Recovery path: if arbiter hold is already active but grant timed out,
  // nudge done once and retry shortly to clear stale DISPLAY_HOLD session.
  if (!granted && radio::isArbiterHold()) {
    asyncSignalDisplaySpiSessionDone();
    delay(5);
    granted = asyncRequestDisplaySpiSession(pdMS_TO_TICKS(150));
  }
  if (granted) {
    // RADIO_FSM_V2: арбитр уже перевёл радио в безопасное DISPLAY_HOLD окно.
    // Здесь не удерживаем radio mutex на весь refresh (секунды), чтобы не блокировать RX/TX path.
    radio::setArbiterHold(true);
    s_einkSpiRadioLocked = false;
    s_einkDisplaySpiSessionActive = true;
  } else
#endif
  {
    // Legacy: семафоры не созданы или до asyncTasksStart — один takeMutex как раньше.
    if (!radio::takeMutex(pdMS_TO_TICKS(5000))) {
      Serial.println("[RiftLink] E-Ink SPI: radio mutex timeout, skip reconfig");
      return false;
    }
    s_einkSpiRadioLocked = true;
  }
  SPI.end();  // иначе begin() не переконфигурирует пины (ESP32 Arduino)
  delay(5);
  SPI.begin(EINK_SCLK, -1, EINK_MOSI, EINK_CS);  // -1 = MISO не используется (3-wire E-Ink)
  applyBusyPinMode();  // re-assert BUSY pull each session; other drivers/tasks may alter pin mode.
  return true;
}
static void releaseDisplaySPI() {
  SPI.end();
  delay(5);
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  if (s_einkSpiRadioLocked) {
    radio::releaseMutex();
    s_einkSpiRadioLocked = false;
  }
#if defined(USE_EINK)
  if (s_einkDisplaySpiSessionActive) {
    s_einkDisplaySpiSessionActive = false;
    radio::setArbiterHold(false);
    asyncSignalDisplaySpiSessionDone();
  }
#endif
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
  s_einkModelDetected = model;
#if defined(USE_EINK_FORCE_BN)
  model = EINK_FC1;
  s_einkModelDetected = model;
#elif defined(USE_EINK_FORCE_B73)
  model = EINK_E0213A367;
  s_einkModelDetected = model;
#endif

  pinMode(EINK_CS, OUTPUT);   // иначе __digitalWrite: IO 4 is not set as GPIO (Arduino 3.x)
  pinMode(EINK_DC, OUTPUT);
  applyBusyPinMode();
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
    // Keep BUSY pull enforced for active-high panel.
    applyBusyPinMode();
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
bool displayIsMenuActive() { return s_menuActive; }

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
  if (!doDisplay(false)) return;
  s_fastRefreshCount = 0;
  s_msgPartialStreak = 0;
  s_lastDisplayEnd = millis();
}

void displaySetTextSize(uint8_t s) {
  if (disp) disp->setTextSize(s);
}

void displayShowWarning(const char* line1, const char* line2, uint32_t durationMs) {
  if (!disp) return;
  ensureCooldownBeforeDisplay();
  disp->fillScreen(GxEPD_WHITE);
  disp->setTextColor(GxEPD_BLACK);
  disp->setTextSize(1);
  disp->drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, GxEPD_BLACK);
  drawTruncRaw(8, 40, line1, MAX_LINE_CHARS);
  if (line2) drawTruncRaw(8, 60, line2, MAX_LINE_CHARS);
  if (!doDisplay(false)) return;
  s_fastRefreshCount = 0;
  s_msgPartialStreak = 0;
  s_lastDisplayEnd = millis();
  uint32_t start = millis();
  while (millis() - start < durationMs) {
    delay(50);
    yield();
  }
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
  if (!doDisplay(false)) return;
  s_fastRefreshCount = 0;
  s_msgPartialStreak = 0;
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
  s_msgPartialStreak = 0;
  int pickLang = locale::getLang();
  uint32_t lastPress = millis();
  const uint32_t CONFIRM_MS = PICKER_CONFIRM_MS;
  while (1) {
    yield();
    ensureCooldownBeforeDisplay();
    disp->fillScreen(GxEPD_WHITE);
    disp->setTextColor(GxEPD_BLACK);
    disp->setTextSize(1);
    drawTruncRaw(4, 25, locale::getForLang("lang_picker_title", pickLang), 30);
    disp->setCursor(98, 55);
    disp->print(pickLang == LANG_EN ? "[EN]" : " EN ");
    disp->print(" ");
    disp->print(pickLang == LANG_RU ? "[RU]" : " RU ");
    drawTruncRaw(4, 95, locale::getForLang("short_long_hint", pickLang), 30);
    bool usePartial = (s_fastRefreshCount < EINK_LIMIT_FASTREFRESH);
    bool drew = false;
    if (s_lastWasFullRefresh) {
      drew = doDisplay(false);
      if (drew) {
        s_hibernateFromIdle = false;
        s_lastWasFullRefresh = false;
        s_fastRefreshCount = 0;
        s_msgPartialStreak = 0;
      }
    } else {
      drew = doDisplay(usePartial);
      if (drew) {
        if (usePartial) s_fastRefreshCount++; else s_fastRefreshCount = 0;
      }
    }
    if (drew) s_lastDisplayEnd = millis();
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
  s_fastRefreshCount = 0;
  s_msgPartialStreak = 0;
  s_previousImageHash = 0;
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
  s_msgPartialStreak = 0;
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
    drawTruncRaw(4, 25, locale::getForDisplay("select_country"), 30);
    disp->setCursor(65, 55);
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
    drawTruncRaw(4, 95, locale::getForDisplay("short_long_hint"), 30);
    bool usePartial = (s_fastRefreshCount < EINK_LIMIT_FASTREFRESH);
    bool drew = false;
    if (s_lastWasFullRefresh) {
      drew = doDisplay(false);
      if (drew) {
        s_hibernateFromIdle = false;
        s_lastWasFullRefresh = false;
        s_fastRefreshCount = 0;
        s_msgPartialStreak = 0;
      }
    } else {
      drew = doDisplay(usePartial);
      if (drew) {
        if (usePartial) s_fastRefreshCount++; else s_fastRefreshCount = 0;
      }
    }
    if (drew) s_lastDisplayEnd = millis();
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
  s_fastRefreshCount = 0;
  s_msgPartialStreak = 0;
  s_previousImageHash = 0;
  yield();
  delay(100);
  return true;
}

static void displayShowModemPicker() {
  if (!disp) return;
  delay(200);

  int pickIdx = (int)radio::getModemPreset();
  if (pickIdx < 0 || pickIdx > 4) pickIdx = 1;

  const char* names[] = {"Speed", "Normal", "Range", "MaxRange", "Custom"};
  const char* desc[]  = {"SF7 BW250 CR5", "SF7 BW125 CR5", "SF10 BW125 CR5", "SF12 BW125 CR8", ""};

  uint32_t lastPress = millis();
  const uint32_t CONFIRM_MS = 8000;

  while (1) {
    ensureCooldownBeforeDisplay();
    disp->fillScreen(GxEPD_WHITE);
    disp->setTextColor(GxEPD_BLACK);
    disp->setTextSize(1);

    int tw;
    const char* title = "Modem Preset";
    tw = strlen(title) * 6;
    disp->setCursor((SCREEN_WIDTH - tw) / 2, 15);
    disp->print(title);

    int startX = 8;
    int y = 45;
    disp->setCursor(startX, y);
    for (int i = 0; i < 5; i++) {
      if (i == pickIdx) { disp->print("["); disp->print(names[i]); disp->print("]"); }
      else { disp->print(" "); disp->print(names[i][0]); }
    }

    if (pickIdx < 4) {
      tw = strlen(desc[pickIdx]) * 6;
      disp->setCursor((SCREEN_WIDTH - tw) / 2, 65);
      disp->print(desc[pickIdx]);
    } else {
      char buf[24];
      snprintf(buf, sizeof(buf), "SF%u BW%.0f CR%u",
          radio::getSpreadingFactor(), radio::getBandwidth(), radio::getCodingRate());
      tw = strlen(buf) * 6;
      disp->setCursor((SCREEN_WIDTH - tw) / 2, 65);
      disp->print(buf);
    }

    const char* hint = locale::getForDisplay("short_long_hint");
    tw = strlen(hint) * 6;
    disp->setCursor((SCREEN_WIDTH - tw) / 2, 95);
    disp->print(hint);

    if (doDisplay(true)) s_lastDisplayEnd = millis();

    while (millis() - lastPress < CONFIRM_MS) {
      int pt = waitButtonPressWithType(CONFIRM_MS - (millis() - lastPress));
      if (pt == PRESS_SHORT) {
        pickIdx = (pickIdx + 1) % 5;
        lastPress = millis();
        break;
      } else if (pt == PRESS_LONG || pt == PRESS_NONE) {
        goto modem_done_paper;
      }
    }
  }
modem_done_paper:
  if (pickIdx < 4) radio::requestModemPreset((radio::ModemPreset)pickIdx);
  s_fastRefreshCount = 0;
  s_msgPartialStreak = 0;
  s_previousImageHash = 0;
}

static void displayRunModemScan() {
  if (!disp) return;

  ensureCooldownBeforeDisplay();
  disp->fillScreen(GxEPD_WHITE);
  disp->setTextColor(GxEPD_BLACK);
  disp->setTextSize(1);
  const char* t = locale::getForDisplay("scanning");
  int tw = strlen(t) * 6;
  disp->setCursor((SCREEN_WIDTH - tw) / 2, 30);
  disp->print(t);
  disp->setCursor((SCREEN_WIDTH - 48) / 2, 55);
  disp->print("~36s ...");
  if (doDisplay(false)) s_lastDisplayEnd = millis();

  selftest::ScanResult res[6];
  int found = selftest::modemScan(res, 6);

  ensureCooldownBeforeDisplay();
  disp->fillScreen(GxEPD_WHITE);
  disp->setTextColor(GxEPD_BLACK);
  disp->setTextSize(1);

  if (found == 0) {
    const char* msg = locale::getForDisplay("scan_empty");
    tw = strlen(msg) * 6;
    disp->setCursor((SCREEN_WIDTH - tw) / 2, 50);
    disp->print(msg);
  } else {
    const char* hdr = locale::getForDisplay("scan_found");
    tw = strlen(hdr) * 6;
    disp->setCursor((SCREEN_WIDTH - tw) / 2, 15);
    disp->print(hdr);
    for (int i = 0; i < found && i < 5; i++) {
      char buf[28];
      snprintf(buf, sizeof(buf), "SF%u BW%.0f %ddBm", res[i].sf, res[i].bw, res[i].rssi);
      tw = strlen(buf) * 6;
      disp->setCursor((SCREEN_WIDTH - tw) / 2, 35 + i * 16);
      disp->print(buf);
    }
  }
  if (doDisplay(false)) {
    s_lastDisplayEnd = millis();
    s_fastRefreshCount = 0;
    s_msgPartialStreak = 0;
    s_previousImageHash = 0;
  }
  for (int i = 0; i < 50; i++) { delay(100); yield(); }
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
  char buf[32];

  // Line 0: nick (or ID) + clock right
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
    drawTruncRaw(SCREEN_WIDTH - 36, CONTENT_Y + 2, clk, 5);
  }

  // Line 1: modem preset or SF/BW
  radio::ModemPreset mp = radio::getModemPreset();
  if (mp < radio::MODEM_CUSTOM)
    snprintf(buf, sizeof(buf), "%.0fMHz %s", region::getFreq(), radio::modemPresetName(mp));
  else
    snprintf(buf, sizeof(buf), "%.0fMHz SF%u BW%.0f", region::getFreq(), (unsigned)radio::getSpreadingFactor(), radio::getBandwidth());
  drawTruncRaw(CONTENT_X, CONTENT_Y + 14, buf, MAX_LINE_CHARS);

  // Line 2: neighbors
  int n = neighbors::getCount();
  snprintf(buf, sizeof(buf), "%s %d", locale::getForDisplay("neighbors"), n);
  drawTruncRaw(CONTENT_X, CONTENT_Y + 26, buf, MAX_LINE_CHARS);

  // Bottom-right: battery
  int pct = batteryPercent(telemetry::readBatteryMv());
  if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
  else snprintf(buf, sizeof(buf), "--");
  drawTruncRaw(SCREEN_WIDTH - 30, CONTENT_Y + CONTENT_H - 14, buf, 5);
}

static void drawContentInfo() {
  if (!disp) return;
  char buf[32];
  int n = neighbors::getCount();

  snprintf(buf, sizeof(buf), "%s: %d", locale::getForDisplay("peers"), n);
  drawTruncRaw(CONTENT_X, CONTENT_Y + 2, buf, MAX_LINE_CHARS);

  int maxShow = (n > 7) ? 7 : n;
  for (int i = 0; i < maxShow; i++) {
    char hex[17];
    neighbors::getIdHex(i, hex);
    int rssi = neighbors::getRssi(i);
    snprintf(buf, sizeof(buf), "%c%c%c%c  %d dBm", hex[0], hex[1], hex[2], hex[3], rssi);
    drawTruncRaw(CONTENT_X, CONTENT_Y + 2 + (1 + i) * LINE_H, buf, MAX_LINE_CHARS);
  }
  if (n > 7) {
    snprintf(buf, sizeof(buf), "+%d more", n - 7);
    drawTruncRaw(CONTENT_X, CONTENT_Y + 2 + 8 * LINE_H, buf, MAX_LINE_CHARS);
  }
}

static void drawContentNet() {
  if (!disp) return;
  char buf[32];

  if (radio_mode::current() == radio_mode::BLE) {
    drawContentLine(0, locale::getForDisplay("ble_mode"));
    snprintf(buf, sizeof(buf), "%s %06u", locale::getForDisplay("pin"), (unsigned)ble::getPasskey());
    drawContentLine(1, buf);
    drawContentLine(2, ble::isConnected() ? locale::getForDisplay("connected") : "...");
    drawContentLine(6, locale::getForDisplay("hold_wifi"));
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
    drawContentLine(6, locale::getForDisplay("hold_ble"));
  }
}

static void drawContentSys() {
  if (!disp) return;
  char buf[32];
  snprintf(buf, sizeof(buf), "RiftLink v%s", RIFTLINK_VERSION);
  drawContentLine(0, buf);

  const uint8_t* id = node::getId();
  snprintf(buf, sizeof(buf), "ID: %02X%02X%02X%02X", id[0], id[1], id[2], id[3]);
  drawContentLine(1, buf);

  radio::ModemPreset mp = radio::getModemPreset();
  if (mp < radio::MODEM_CUSTOM)
    snprintf(buf, sizeof(buf), "%s  %s  %ddBm", region::getCode(), radio::modemPresetName(mp), region::getPower());
  else
    snprintf(buf, sizeof(buf), "%s  SF%u/%u/%u", region::getCode(), radio::getSpreadingFactor(), (unsigned)radio::getBandwidth(), radio::getCodingRate());
  drawContentLine(2, buf);

  snprintf(buf, sizeof(buf), "PS:%s  %s", powersave::isEnabled() ? "ON" : "OFF", locale::getLang() == LANG_RU ? "[RU]" : "[EN]");
  drawContentLine(3, buf);

  drawContentLine(6, locale::getForDisplay("hold_settings"));
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
  drawContentLine(6, locale::getForDisplay("hold_gps"));
}

static void drawScreenContent(int tab) {
  drawFrame(tab);
  switch (display_tabs::contentForTab(tab)) {
    case display_tabs::CT_MAIN: drawContentMain(); break;
    case display_tabs::CT_MSG:  drawContentMsg(); break;
    case display_tabs::CT_INFO: drawContentInfo(); break;
    case display_tabs::CT_NET:  drawContentNet(); break;
    case display_tabs::CT_SYS:  drawContentSys(); break;
    case display_tabs::CT_GPS:  drawContentGps(); break;
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
  if (!forceUpdate && hash == s_previousImageHash && ct != display_tabs::CT_SYS) {
    s_previousRunMs = now;  // не спамить — иначе CPU burn при неизменном контенте
    return false;
  }

  // Partial; на MSG — свой счётчик (частые смены текста → чаще full против ghosting)
  bool usePartial = usePartialForTab(ct);
  bool ok = false;
  if (s_lastWasFullRefresh) {
    // wake из сна: всегда full — иначе ghosting (флаги сбрасываем только после успешного display)
    ensureCooldownBeforeDisplay();
    ok = doDisplay(false);
    if (ok) {
      s_hibernateFromIdle = false;
      s_lastWasFullRefresh = false;
      s_fastRefreshCount = 0;
      s_msgPartialStreak = 0;
      s_previousImageHash = 0;
    }
  } else {
    ensureCooldownBeforeDisplay();
    ok = doDisplay(usePartial);
    if (ok) applyPartialStreakAfter(ct, usePartial);
  }

  if (!ok) return false;

  s_lastDisplayEnd = millis();
  s_previousRunMs = now;
  s_previousImageHash = hash;
  s_lastActivityTime = now;  // обновление данных — активность (откладывает hibernate)
  return true;
}

static bool drawScreen(int tab, bool forceFull = false) {
  drawScreenContent(tab);
  display_tabs::ContentTab ct = display_tabs::contentForTab(tab);
  // powersave: всегда full, не зависеть от s_lastWasFullRefresh — иначе вторая смена вкладки перестаёт рисоваться
  bool ok = false;
  if (forceFull) {
    ensureCooldownBeforeDisplay();
    ok = doDisplay(false);
    if (ok) {
      s_hibernateFromIdle = false;
      s_lastWasFullRefresh = false;
      s_fastRefreshCount = 0;
      s_msgPartialStreak = 0;
      s_previousImageHash = 0;
    }
  } else {
    bool usePartial = usePartialForTab(ct);
    if (s_lastWasFullRefresh) {
      ensureCooldownBeforeDisplay();
      ok = doDisplay(false);
      if (ok) {
        s_hibernateFromIdle = false;
        s_lastWasFullRefresh = false;
        s_fastRefreshCount = 0;
        s_msgPartialStreak = 0;
        s_previousImageHash = 0;
      }
    } else {
      ensureCooldownBeforeDisplay();
      ok = doDisplay(usePartial);
      if (ok) applyPartialStreakAfter(ct, usePartial);
    }
  }
  if (!ok) return false;
  s_lastDisplayEnd = millis();
  s_previousRunMs = millis();
  s_previousImageHash = computeContentHash(tab);
  s_lastActivityTime = millis();  // отрисовка — активность (откладывает hibernate)
  return true;
}

void displaySetButtonPolledExternally(bool on) {
  s_buttonPolledExternally = on;
}

void displayRequestInfoRedraw() {
  displayWakeRequest();
  s_needRedrawInfo = true;
}

void displaySetLastMsg(const char* fromHex, const char* text) {
  s_lastActivityTime = millis();
  if (fromHex) { strncpy(s_lastMsgFrom, fromHex, 16); s_lastMsgFrom[16] = '\0'; }
  if (text) { strncpy(s_lastMsgText, text, 63); s_lastMsgText[63] = '\0'; }
  if (display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_MSG) s_needRedrawMsg = true;
}

void displayShowScreen(int screen) {
  s_lastActivityTime = millis();
  int nTabs = display_tabs::getTabCount();
  if (screen >= nTabs) screen = nTabs - 1;
  onPaperTabSwitch(s_currentScreen, screen);
  s_currentScreen = screen;
  s_previousRunMs = 0;
  bool forceFull = powersave::isEnabled();  // режим экономии: full → hibernate после каждой вкладки
  (void)drawScreen(s_currentScreen, forceFull);
}

void displayShowScreenForceFull(int screen) {
  int nTabs = display_tabs::getTabCount();
  if (screen >= nTabs) screen = nTabs - 1;
  onPaperTabSwitch(s_currentScreen, screen);
  s_currentScreen = screen;
  s_previousRunMs = 0;
  (void)drawScreen(s_currentScreen, true);  // full refresh — против ghosting при смене вкладки
}

int displayGetCurrentScreen() {
  return s_currentScreen;
}

int displayGetNextScreen(int current) {
  return (current + 1) % display_tabs::getTabCount();
}

/** Popup menu for E-Ink: short press = next item, long press = select, timeout = back.
 *  Returns selected index or -1 on timeout. */
static int displayShowPopupMenu(const char* items[], int count) {
  if (!disp || count <= 0) return -1;
  delay(200);
  s_fastRefreshCount = 0;
  s_msgPartialStreak = 0;
  int selected = 0;
  int scrollOff = 0;
  uint32_t lastPress = millis();
  const uint32_t MENU_TIMEOUT_MS = 15000;
  const int maxVisible = (CONTENT_H - 4) / LINE_H;

  while (1) {
    if (selected < scrollOff) scrollOff = selected;
    if (selected >= scrollOff + maxVisible) scrollOff = selected - maxVisible + 1;

    yield();
    ensureCooldownBeforeDisplay();
    drawFrame(s_currentScreen);
    disp->setTextSize(1);
    int show = count - scrollOff;
    if (show > maxVisible) show = maxVisible;
    for (int i = 0; i < show; i++) {
      int idx = scrollOff + i;
      int y = CONTENT_Y + 4 + i * LINE_H;
      if (idx == selected) {
        disp->fillRect(1, y - 2, SCREEN_WIDTH - 2, LINE_H, GxEPD_BLACK);
        disp->setTextColor(GxEPD_WHITE);
      } else {
        disp->setTextColor(GxEPD_BLACK);
      }
      drawTruncRaw(CONTENT_X + 4, y, items[idx], MAX_LINE_CHARS - 2);
    }
    disp->setTextColor(GxEPD_BLACK);
    if (scrollOff > 0)
      disp->fillTriangle(SCREEN_WIDTH - 8, CONTENT_Y + 2, SCREEN_WIDTH - 12, CONTENT_Y + 7, SCREEN_WIDTH - 4, CONTENT_Y + 7, GxEPD_BLACK);
    if (scrollOff + maxVisible < count)
      disp->fillTriangle(SCREEN_WIDTH - 8, CONTENT_Y + CONTENT_H - 3, SCREEN_WIDTH - 12, CONTENT_Y + CONTENT_H - 8, SCREEN_WIDTH - 4, CONTENT_Y + CONTENT_H - 8, GxEPD_BLACK);

    bool usePartial = (s_fastRefreshCount < EINK_LIMIT_FASTREFRESH);
    bool drew = false;
    if (s_lastWasFullRefresh) {
      drew = doDisplay(false);
      if (drew) {
        s_hibernateFromIdle = false;
        s_lastWasFullRefresh = false;
        s_fastRefreshCount = 0;
        s_msgPartialStreak = 0;
      }
    } else {
      drew = doDisplay(usePartial);
      if (drew) {
        if (usePartial) s_fastRefreshCount++; else s_fastRefreshCount = 0;
      }
    }
    if (drew) s_lastDisplayEnd = millis();

    while (millis() - lastPress < MENU_TIMEOUT_MS) {
      yield();
      uint32_t elapsed = millis() - lastPress;
      if (elapsed >= MENU_TIMEOUT_MS) break;
      uint32_t remaining = MENU_TIMEOUT_MS - elapsed;
      int pt = waitButtonPressWithType(remaining);
      if (pt == PRESS_SHORT) {
        selected = (selected + 1) % count;
        lastPress = millis();
        for (int i = 0; i < 4; i++) { delay(50); yield(); }
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
  ensureCooldownBeforeDisplay();
  display_tabs::ContentTab ct = display_tabs::contentForTab(screen);

  if (ct == display_tabs::CT_SYS) {
    char psBuf[24];
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
    char gpsBuf[24];
    snprintf(gpsBuf, sizeof(gpsBuf), "GPS: %s", gpsOn ? "ON -> OFF" : "OFF -> ON");
    const char* items[] = {
      gpsBuf,
      locale::getForDisplay("menu_back")
    };
    int sel = displayShowPopupMenu(items, 2);
    if (sel == 0 && gps::isPresent()) { gps::toggle(); }
  }

  s_menuActive = false;
  s_previousImageHash = 0;
  s_fastRefreshCount = 0;
  s_msgPartialStreak = 0;
  (void)drawScreen(s_currentScreen, true);
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
        int prevTab = s_currentScreen;
        s_currentScreen = displayGetNextScreen(s_currentScreen);
        onPaperTabSwitch(prevTab, s_currentScreen);
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
  if (s_needRedrawInfo) {
    if (performDisplayUpdate(s_currentScreen, true, true) ||
        drawScreen(s_currentScreen, powersave::isEnabled())) {
      s_needRedrawInfo = false;
    }
    return false;
  }
  if (s_needRedrawMsg && display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_MSG) {
    if (performDisplayUpdate(s_currentScreen, true, true) ||
        drawScreen(s_currentScreen, powersave::isEnabled())) {
      s_needRedrawMsg = false;
    }
    return false;
  }
  if ((now - s_lastDisplayEnd) < EINK_COOLDOWN_HW_MS) return false;

  hibernateIfIdle();  // 30 с неактивности → панель в hibernate

  // Периодика только для вкладок с живыми данными. Lang, Sys, Msg — только по событию
  display_tabs::ContentTab ct = display_tabs::contentForTab(s_currentScreen);
  const bool tabHasLiveData = (ct == display_tabs::CT_MAIN || ct == display_tabs::CT_INFO || ct == display_tabs::CT_NET || ct == display_tabs::CT_GPS);
  if (tabHasLiveData) {
    performDisplayUpdate(s_currentScreen, false);
  }
  return false;
}
