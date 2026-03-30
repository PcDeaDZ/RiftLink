/**
 * RiftLink Display — E-Ink 2.13" Heltec Wireless Paper
 * Определение как Meshtastic: einkDetect.h — RST LOW, read BUSY. LOW→FC1(V1.1), HIGH→E0213A367(V1.2)
 * Пины: CS=4, BUSY=7, DC=5, RST=6, SCLK=3, MOSI=2, VEXT=45
 */

#include "../../display.h"
#include "../../display_tabs.h"
#include "../../region_modem_fmt.h"
#include "../../ui_section_titles.h"
#include "../../ui_scroll.h"
#include "../../ui_menu_exec.h"
#include "../../ui_icons.h"
#include "../../ui_msg_scroll.h"
#include "../../ui_layout_profile.h"
#include "../../ui_topbar_fill.h"
#include "../../ui_typography.h"
#include "../../ui_nav_mode.h"
#include "../../ui_display_prefs.h"
#include "../../ui_tab_bar_idle.h"
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
/** Шаг строки списка меню (popup, power) — из ui_layout; контент вкладок — LINE_H */
static constexpr auto kProfPaper = ui_layout::profilePaper250x122();
static constexpr int MENU_LIST_ROW_H_PP = kProfPaper.menuListRowHeight;
static constexpr int MENU_LIST_TEXT_OFF_PP = kProfPaper.menuListTextOffsetY;
static constexpr int MENU_LIST_ICON_OFF_PP = kProfPaper.menuListIconTopOffsetY;
#define MAX_LINE_CHARS 32
#define CONTENT_X 4
/** Полоса выбора в списках (главное меню согласовано с displayShowPopupMenu по ширине) */
#define MENU_SEL_X 1
#define MENU_SEL_W (SCREEN_WIDTH - 2)
#define POPUP_MODE_CANCEL 0
#define POPUP_MODE_PICKER 1
#define SUBMENU_TITLE_H_PAPER 16
/** Высота полосы вкладок в зоне топбара (совпадает с TAB_H). */
#define TAB_BAR_H_PAPER 18
static int s_layoutTabShiftY = 0;
static bool s_tabDrillIn = false;
static int s_powerMenuIndex = 0;
static int s_gpsMenuIndex = 0;
/** Экран «Сообщения» без полосы заголовка: как «Узел» — отступ от разделителя и строка «Назад». */
#define PAPER_STATUS_SEP_BOTTOM 15
#define PAPER_MSG_LIST_TOP_PAD_PX 2
#define PAPER_MSG_MIN_BASELINE_Y (PAPER_STATUS_SEP_BOTTOM + 4)
#define PAPER_MSG_BACK_GAP_PX 5
/** Главное меню: межстрочный интервал; при нехватке высоты — прокрутка (стрелки). */
#define PAPER_HOME_ROW_STEP_7 16
#define PAPER_HOME_ROW_STEP_6 19
#define PAPER_HOME_ROW_STEP_5 23

/* submenuListY0 в профиле — без полоски вкладок; при isTabMode() + TAB_BAR_H_PAPER */
static_assert(ui_layout::profilePaper250x122().submenuListY0 == (CONTENT_Y + SUBMENU_TITLE_H_PAPER + 4), "ui_layout vs Paper");
static_assert(kProfPaper.menuListRowHeight == 16, "ui_layout menuListRowHeight Paper");
static_assert(kProfPaper.submenuContentLineHeight == LINE_H, "LINE_H vs profile submenu content");

static void drawSubScreenChrome();
static void drawTabBarPaper();
static void prepareTabLayoutShiftPaper();
static void drawChromeTabsOrIdleRowPaper();
static int displayShowPopupMenu(const char* title, const char* items[], int count, int initialSel = 0, int mode = POPUP_MODE_CANCEL,
    const uint8_t* const* rowIcons = nullptr, bool lastRowNoIcon = false, bool noRowIcons = false);
static int submenuListY0Paper(bool hasTitle);
static void drawSubmenuTitleBarPaper(const char* title);
static void drawSubmenuFooterSepPaper(int hintY);

using DispBN = GxEPD2_BW<GxEPD2_213_BN, GxEPD2_213_BN::HEIGHT>;
using DispFC1 = GxEPD2_BW<GxEPD2_213_FC1, GxEPD2_213_FC1::HEIGHT>;
using DispB73 = GxEPD2_BW<GxEPD2_213_E0213A367, GxEPD2_213_E0213A367::HEIGHT>;

static DispBN* dispBN = nullptr;
static DispFC1* dispFC1 = nullptr;
static DispB73* dispB73 = nullptr;
#define disp (dispBN ? (Adafruit_GFX*)dispBN : (dispFC1 ? (Adafruit_GFX*)dispFC1 : (Adafruit_GFX*)dispB73))

void displayApplyRotationFromPrefs() {
  if (!disp) return;
  const uint8_t base = 3;
  const uint8_t r = ui_display_prefs::getFlip180() ? (uint8_t)((base + 2) % 4) : base;
  disp->setRotation(r);
}

static int s_currentScreen = 0;
static int s_homeMenuIndex = 0;
static int s_homeMenuScrollOff = 0;
static int s_netMenuIndex = 0;
static int s_sysMenuIndex = 0;
static int s_sysScrollOff = 0;
static bool s_sysInDisplaySubmenu = false;
static size_t s_msgScrollStart = 0;
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

static inline display_tabs::ContentTab contentTabAtIndex(int screen) {
  return ui_nav_mode::isTabMode() ? display_tabs::contentForNavTab(screen) : display_tabs::contentForTab(screen);
}
static inline int tabCountForUi() {
  return ui_nav_mode::isTabMode() ? display_tabs::getNavTabCount() : display_tabs::getTabCount();
}

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
#define BTN_ACTIVE_LOW 1
#define BTN_PRESSED (digitalRead(BUTTON_PIN) == (BTN_ACTIVE_LOW ? LOW : HIGH))
#define SHORT_PRESS_MS 350   // граница short/long (используется в waitButtonPressWithType)
#define LONG_PRESS_MS  500
#define MIN_PRESS_MS   50   // минимум удержания для short / игнор дребезга
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
  int y = submenuListY0Paper(true) + line * LINE_H;
  if (useUtf8) drawTruncUtf8(CONTENT_X, y, s, MAX_LINE_CHARS);
  else drawTruncRaw(CONTENT_X, y, s, MAX_LINE_CHARS);
}

/** Хеш контента вкладки — для пропуска идентичных кадров (как Meshtastic checkFrameMatchesPrevious) */
static uint32_t computeContentHash(int tab) {
  display_tabs::ContentTab ct = contentTabAtIndex(tab);
  uint32_t h = (uint32_t)tab * 31;
  if (ct == display_tabs::CT_MAIN) {
    h ^= (uint32_t)(region::getFreq() * 100) * 11;
    h ^= (uint32_t)radio::getSpreadingFactor() * 7;
    h ^= (uint32_t)radio::getModemPreset() * 23;
    h ^= (uint32_t)(radio::getBandwidth()) * 29;
    h ^= (uint32_t)neighbors::getCount() * 13;
    h ^= (uint32_t)(neighbors::getAverageRssi() + 200) * 43;
    h ^= (uint32_t)telemetry::batteryPercent() * 19;
  } else if (ct == display_tabs::CT_INFO) {
    h ^= (uint32_t)neighbors::getCount() * 17;
    int n = neighbors::getCount();
    for (int i = 0; i < n && i < 7; i++) h ^= (uint32_t)(neighbors::getRssi(i) + 200) * (i + 3);
  } else if (ct == display_tabs::CT_NET) {
    h ^= (uint32_t)s_netMenuIndex * 53u;
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
    h ^= (uint32_t)s_gpsMenuIndex * 47u;
    h ^= gps::isEnabled() ? 1 : 0;
    h ^= gps::hasFix() ? 2 : 0;
    h ^= gps::hasPhoneSync() ? 4 : 0;
    h ^= (uint32_t)gps::getSatellites() * 19;
    h ^= (uint32_t)(gps::getLat() * 10000) * 23;
    h ^= (uint32_t)(gps::getLon() * 10000) * 29;
  } else if (ct == display_tabs::CT_SYS) {
    h ^= (uint32_t)s_sysMenuIndex * 59u;
    h ^= (uint32_t)s_sysScrollOff * 61u;
    h ^= s_sysInDisplaySubmenu ? 0x2000000u : 0u;
    h ^= powersave::isEnabled() ? 3u : 0u;
  } else if (ct == display_tabs::CT_HOME) {
    h ^= (uint32_t)s_homeMenuIndex * 41u;
    h ^= (uint32_t)s_homeMenuScrollOff * 43u;
    h ^= (uint32_t)(telemetry::readBatteryMv() / 50u) * 19u;
    h ^= (uint32_t)neighbors::getCount() * 13;
  } else if (ct == display_tabs::CT_POWER) {
    h ^= 0x505u;
  }
  h ^= ui_nav_mode::isTabMode() ? 0x80000000u : 0u;
  h ^= s_tabDrillIn ? 0x20000000u : 0u;
  h ^= ui_tab_bar_idle::tabStripVisible() ? 0x1000000u : 0u;
  h ^= ui_display_prefs::getFlip180() ? 0x800000u : 0u;
  h ^= (uint32_t)ui_display_prefs::getTabBarHideIdleSeconds() * 131u;
  return h;
}

/** При уходе с вкладки сообщений: первый кадр на новой вкладке — full (счётчик → порог). Иначе смена вкладок — как обычно (partial по лимиту). */
static void onPaperTabSwitch(int prevTab, int newTab) {
  if (prevTab == newTab) return;
  if (contentTabAtIndex(prevTab) == display_tabs::CT_MSG &&
      contentTabAtIndex(newTab) != display_tabs::CT_MSG) {
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
  displayApplyRotationFromPrefs();
  dispBN->fillScreen(GxEPD_WHITE);
  dispBN->setTextColor(GxEPD_BLACK);
  dispBN->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
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
    displayApplyRotationFromPrefs();
    dispFC1->fillScreen(GxEPD_WHITE);
    dispFC1->setTextColor(GxEPD_BLACK);
    dispFC1->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
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
    displayApplyRotationFromPrefs();
    dispB73->fillScreen(GxEPD_WHITE);
    dispB73->setTextColor(GxEPD_BLACK);
    dispB73->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
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
    displayApplyRotationFromPrefs();
    dispFC1->fillScreen(GxEPD_WHITE);
    dispFC1->setTextColor(GxEPD_BLACK);
    dispFC1->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
    dispFC1->cp437(true);
    dispFC1->display(false);
  } else {
    dispB73 = new DispB73(GxEPD2_213_E0213A367(EINK_CS, EINK_DC, EINK_RST, EINK_BUSY, SPI));
    dispB73->init(115200, true, 20, false);
    displayApplyRotationFromPrefs();
    dispB73->fillScreen(GxEPD_WHITE);
    dispB73->setTextColor(GxEPD_BLACK);
    dispB73->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
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

void displayShowInitProgress(int doneCount, int totalSteps, const char* statusLine) {
  if (!disp) return;
  if (totalSteps < 1) totalSteps = 1;
  if (doneCount < 0) doneCount = 0;
  if (doneCount > totalSteps) doneCount = totalSteps;
  ensureCooldownBeforeDisplay();
  drawSubScreenChrome();
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
  disp->setTextColor(GxEPD_BLACK);
  const int top = TAB_H + 6;
  drawTruncRaw(4, top, locale::getForDisplay("init_title"), MAX_LINE_CHARS);
  const int cy = top + 26;
  const int r = 4;
  const int n = totalSteps;
  const int pitch = n > 1 ? (SCREEN_WIDTH - 20) / (n - 1) : 0;
  const int pitchClamped = pitch < 12 ? 12 : (pitch > 24 ? 24 : pitch);
  const int startX = SCREEN_WIDTH / 2 - ((n - 1) * pitchClamped) / 2;
  for (int i = 0; i < n - 1; i++) {
    const int cx0 = startX + i * pitchClamped;
    const int cx1 = startX + (i + 1) * pitchClamped;
    disp->drawLine(cx0 + r, cy, cx1 - r, cy, GxEPD_BLACK);
  }
  for (int i = 0; i < n; i++) {
    const int cx = startX + i * pitchClamped;
    if (i < doneCount) {
      disp->fillCircle(cx, cy, r, GxEPD_BLACK);
    } else if (i == doneCount && doneCount < totalSteps) {
      disp->drawCircle(cx, cy, r, GxEPD_BLACK);
      disp->fillCircle(cx, cy, 1, GxEPD_BLACK);
    } else {
      disp->drawCircle(cx, cy, r, GxEPD_BLACK);
    }
  }
  if (statusLine && statusLine[0])
    drawTruncRaw(4, top + 44, statusLine, MAX_LINE_CHARS);
  drawTruncRaw(4, top + 62, locale::getForDisplay("init_hint"), MAX_LINE_CHARS);
  if (!doDisplay(false)) return;
  s_fastRefreshCount = 0;
  s_msgPartialStreak = 0;
  s_previousImageHash = 0;
  s_lastDisplayEnd = millis();
}

void displayShowWarning(const char* line1, const char* line2, uint32_t durationMs) {
  if (!disp) return;
  ensureCooldownBeforeDisplay();
  prepareTabLayoutShiftPaper();
  drawChromeTabsOrIdleRowPaper();
  drawSubmenuTitleBarPaper(locale::getForDisplay("warn_title"));
  disp->setTextColor(GxEPD_BLACK);
  const int listY = submenuListY0Paper(true);
  if (line1) drawTruncRaw(4, listY, line1, MAX_LINE_CHARS);
  if (line2) drawTruncRaw(4, listY + LINE_H, line2, MAX_LINE_CHARS);
  drawSubmenuFooterSepPaper(SCREEN_HEIGHT - 12);
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

// Бут-скрин: логотип Rift Link из app_icon_source.png
void displayShowBootScreen() {
  if (!disp) return;
  ensureCooldownBeforeDisplay();
  disp->fillScreen(GxEPD_WHITE);
  disp->drawBitmap(0, 0, bootscreen_paper, BOOTSCREEN_PAPER_W, BOOTSCREEN_PAPER_H, GxEPD_BLACK);
  disp->setTextColor(GxEPD_BLACK);
  disp->setTextSize((uint8_t)ui_typography::bootVersionTextSizePaper());
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
  if (nPresets <= 0) return false;
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
  s_fastRefreshCount = 0;
  s_msgPartialStreak = 0;
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
  s_fastRefreshCount = 0;
  s_msgPartialStreak = 0;
  s_previousImageHash = 0;
}

static void displayRunModemScan() {
  if (!disp) return;
  prepareTabLayoutShiftPaper();
  const int listY = submenuListY0Paper(false);
  ensureCooldownBeforeDisplay();
  drawChromeTabsOrIdleRowPaper();
  disp->setTextColor(GxEPD_BLACK);
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
  drawTruncRaw(4, listY, locale::getForDisplay("scanning"), MAX_LINE_CHARS);
  drawTruncRaw(4, listY + LINE_H, "~36s ...", MAX_LINE_CHARS);
  if (doDisplay(false)) s_lastDisplayEnd = millis();

  selftest::ScanResult res[6];
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

static void drawSignalBarsPaper(int x, int y, int barsCount) {
  if (!disp) return;
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;
    int bx = x + i * 4;
    int by = y + 8 - h;
    if (i < barsCount) disp->fillRect(bx, by, 3, h, GxEPD_BLACK);
    else disp->drawRect(bx, by, 3, h, GxEPD_BLACK);
  }
}

static void drawBatteryIconPaper(int x, int y, int pct, bool charging) {
  if (!disp) return;
  disp->drawRect(x, y, 14, 7, GxEPD_BLACK);
  disp->fillRect(x + 14, y + 2, 2, 3, GxEPD_BLACK);
  if (pct > 0) {
    int fill = (pct * 10) / 100;
    if (fill < 1 && pct > 0) fill = 1;
    if (fill > 10) fill = 10;
    disp->fillRect(x + 2, y + 2, fill, 3, GxEPD_BLACK);
  }
  if (charging) {
    disp->drawLine(x + 8, y + 1, x + 6, y + 3, GxEPD_BLACK);
    disp->drawLine(x + 6, y + 3, x + 9, y + 3, GxEPD_BLACK);
    disp->drawLine(x + 9, y + 3, x + 7, y + 5, GxEPD_BLACK);
  }
}

/** y0=0 — верхний топбар; y0=CONTENT_Y-2 — тот же топбар в полосе вкладок. */
static void drawStatusBarPaperAt(int y0) {
  if (!disp) return;
  disp->setTextColor(GxEPD_BLACK);
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
  if (pct >= 0) {
    snprintf(buf, sizeof(buf), "%d%%", pct);
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  rightClusterLeft -= (int)strlen(buf) * 6 + 4;
  rightClusterLeft -= 16;

  drawSignalBarsPaper(2, y0 + 2, tb.signalBars);
  const int leftBlockEnd = 22;
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
      drawTruncRaw(xMid, y0 + 4, mid, maxChars);
    }
  }

  int xRight = SCREEN_WIDTH - 2;
  xRight -= 16;
  const int batX = xRight;
  drawBatteryIconPaper(batX, y0 + 2, pct >= 0 ? pct : 0, chg);
  xRight -= 4;
  if (pct >= 0) {
    snprintf(buf, sizeof(buf), "%d%%", pct);
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  {
    const int pw = (int)strlen(buf) * 6;
    xRight -= pw;
    drawTruncRaw(xRight, y0 + 4, buf, 4);
    xRight -= 4;
  }
  if (tb.hasTime) {
    snprintf(buf, sizeof(buf), "%02d:%02d", tb.hour, tb.minute);
    const int tw = (int)strlen(buf) * 6;
    xRight -= tw;
    drawTruncRaw(xRight, y0 + 4, buf, 5);
  }
}

/** drawUpperTopbar: false — только разделители; статус рисуется отдельно (см. drawChromeTabsOrIdleRowPaper). */
static void drawSubScreenChromePaper(bool drawUpperTopbar) {
  if (!disp) return;
  disp->fillScreen(GxEPD_WHITE);
  disp->setTextColor(GxEPD_BLACK);
  if (drawUpperTopbar) drawStatusBarPaperAt(0);
  disp->drawFastHLine(0, 13, SCREEN_WIDTH, GxEPD_BLACK);
  disp->drawFastHLine(0, 15, SCREEN_WIDTH, GxEPD_BLACK);
}

static void drawSubScreenChrome() {
  drawSubScreenChromePaper(true);
}

static void drawTabBarPaper() {
  if (!disp) return;
  const int y0 = 0;
  const int h = TAB_BAR_H_PAPER;
  const int n = display_tabs::getNavTabCount();
  if (n < 1) return;
  const int activeIdx = display_tabs::clampNavTabIndex(s_currentScreen);
  if (activeIdx != s_currentScreen) s_currentScreen = activeIdx;
  const int cellW = SCREEN_WIDTH / n;
  const int rem = SCREEN_WIDTH - n * cellW;
  const int iconScaled = ICON_W * ICON_SCALE;
  const display_tabs::ContentTab activeCt = display_tabs::contentForNavTab(activeIdx);
  for (int i = 0; i < n; i++) {
    const int x0 = i * cellW;
    const int w = cellW + (i == n - 1 ? rem : 0);
    const uint8_t* icon = display_tabs::iconForNavTab(i);
    const int ix = x0 + (w - iconScaled) / 2;
    const int iy = y0 + (h - iconScaled) / 2;
    const bool sel = (display_tabs::contentForNavTab(i) == activeCt);
    if (sel) {
      disp->fillRect(x0, y0, w, h, GxEPD_BLACK);
      drawIconScaled(ix, iy, icon, GxEPD_WHITE);
      disp->drawFastHLine(x0, y0 + h - 1, w, GxEPD_WHITE);
    } else {
      drawIconScaled(ix, iy, icon, GxEPD_BLACK);
    }
  }
}

static void prepareTabLayoutShiftPaper() {
  if (!ui_nav_mode::isTabMode()) {
    s_layoutTabShiftY = 0;
    return;
  }
  /*
   * Вкладки в зоне топбара (0..TAB_H-1); отдельной второй полосы под статусом нет.
   * При видимой полоске разделитель 18–20 под полосой вкладок — +3 к базовому CONTENT_Y.
   */
  s_layoutTabShiftY = ui_tab_bar_idle::tabStripVisible() ? 3 : 0;
}

static void drawChromeTabsOrIdleRowPaper() {
  if (!ui_nav_mode::isTabMode()) {
    drawSubScreenChrome();
    return;
  }
  if (ui_tab_bar_idle::tabStripVisible()) {
    if (!disp) return;
    disp->fillScreen(GxEPD_WHITE);
    disp->setTextColor(GxEPD_BLACK);
    drawTabBarPaper();
    disp->drawFastHLine(0, 18, SCREEN_WIDTH, GxEPD_BLACK);
    disp->drawFastHLine(0, 20, SCREEN_WIDTH, GxEPD_BLACK);
  } else {
    drawSubScreenChromePaper(false);
    drawStatusBarPaperAt(0);
  }
}

static int submenuListY0Paper(bool hasTitle) {
  const int base = hasTitle ? (CONTENT_Y + SUBMENU_TITLE_H_PAPER + 4) : CONTENT_Y;
  return base + s_layoutTabShiftY;
}

static void drawSubmenuTitleBarPaper(const char* title) {
  if (!disp || !title || !title[0]) return;
  const int top = CONTENT_Y + s_layoutTabShiftY;
  disp->fillRect(0, top, SCREEN_WIDTH, SUBMENU_TITLE_H_PAPER, GxEPD_BLACK);
  disp->setTextColor(GxEPD_WHITE);
  drawTruncRaw(4, top + 3, title, MAX_LINE_CHARS);
  disp->setTextColor(GxEPD_BLACK);
}

static void drawSubmenuFooterSepPaper(int hintY) {
  if (!disp) return;
  for (int x = 4; x < SCREEN_WIDTH - 4; x += 4)
    disp->drawPixel(x, hintY - 4, GxEPD_BLACK);
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
  if (!disp) return;
  drawSubScreenChrome();
  const int menuY0 = CONTENT_Y;
  const int nItems = display_tabs::homeMenuCount();
  const int rowStep = (nItems >= 7) ? PAPER_HOME_ROW_STEP_7
                                    : ((nItems >= 6) ? PAPER_HOME_ROW_STEP_6 : PAPER_HOME_ROW_STEP_5);
  const int bottomPad = 4;
  int showMax = (SCREEN_HEIGHT - menuY0 - bottomPad) / rowStep;
  if (showMax < 1) showMax = 1;
  if (showMax > nItems) showMax = nItems;
  int scrollRef = s_homeMenuScrollOff;
  ui_scroll::syncListWindow(s_homeMenuIndex, nItems, showMax, scrollRef);
  s_homeMenuScrollOff = scrollRef;

  constexpr int kIconScaledH = ICON_H * ICON_SCALE;
  if (ui_scroll::canScrollUp(s_homeMenuScrollOff)) {
    disp->fillTriangle(SCREEN_WIDTH - 8, menuY0 + 2, SCREEN_WIDTH - 12, menuY0 + 7, SCREEN_WIDTH - 4, menuY0 + 7, GxEPD_BLACK);
  }
  for (int vis = 0; vis < showMax; vis++) {
    const int i = s_homeMenuScrollOff + vis;
    const uint8_t* icon = display_tabs::iconForContent(display_tabs::homeMenuContentAt(i));
    int y = menuY0 + vis * rowStep;
    const int iconTop = (rowStep >= kIconScaledH) ? (rowStep - kIconScaledH) / 2 : 2;
    const int textOff = (rowStep > 8) ? (rowStep - 8) / 2 : 4;
    if (i == s_homeMenuIndex) {
      disp->fillRect(MENU_SEL_X, y, MENU_SEL_W, rowStep, GxEPD_BLACK);
      disp->setTextColor(GxEPD_WHITE);
      drawIconScaled(4, y + iconTop, icon, GxEPD_WHITE);
      drawTruncRaw(4 + ICON_W * ICON_SCALE + 6, y + textOff, homeMenuLabelForSlot(i), MAX_LINE_CHARS - 4);
    } else {
      disp->setTextColor(GxEPD_BLACK);
      drawIconScaled(4, y + iconTop, icon, GxEPD_BLACK);
      drawTruncRaw(4 + ICON_W * ICON_SCALE + 6, y + textOff, homeMenuLabelForSlot(i), MAX_LINE_CHARS - 4);
    }
  }
  if (ui_scroll::canScrollDown(s_homeMenuScrollOff, nItems, showMax)) {
    const int triY = menuY0 + (showMax - 1) * rowStep + rowStep - 4;
    disp->fillTriangle(SCREEN_WIDTH - 8, triY, SCREEN_WIDTH - 12, triY - 4, SCREEN_WIDTH - 4, triY - 4, GxEPD_BLACK);
  }
  disp->setTextColor(GxEPD_BLACK);
}

static void drawContentMain() {
  if (!disp) return;
  char buf[40];
  const int listY0Raw = submenuListY0Paper(false);
  const int minY = PAPER_MSG_MIN_BASELINE_Y + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < minY) ? minY : listY0Raw;
  const int lh = LINE_H;
  const uint8_t* nid = node::getId();
  char idHex[20];
  snprintf(idHex, sizeof(idHex), "%02X%02X%02X%02X%02X%02X%02X%02X",
      nid[0], nid[1], nid[2], nid[3], nid[4], nid[5], nid[6], nid[7]);

  char nick[33];
  node::getNickname(nick, sizeof(nick));

  const int bandH = lh + 2;
  disp->fillRect(0, listY0 - 1, SCREEN_WIDTH, bandH, GxEPD_BLACK);
  disp->setTextColor(GxEPD_WHITE);
  if (nick[0]) {
    drawTruncUtf8(CONTENT_X, listY0, nick, MAX_LINE_CHARS - 2);
  } else {
    drawTruncRaw(CONTENT_X, listY0, idHex, MAX_LINE_CHARS);
  }
  disp->setTextColor(GxEPD_BLACK);

  int line = 1;
  if (nick[0]) {
    snprintf(buf, sizeof(buf), "%s %s", locale::getForDisplay("id"), idHex);
    drawTruncRaw(CONTENT_X, listY0 + lh * line, buf, MAX_LINE_CHARS);
    line++;
  }

  int n = neighbors::getCount();
  int avgRssi = neighbors::getAverageRssi();
  snprintf(buf, sizeof(buf), "%s: %d", locale::getForDisplay("neighbors"), n);
  const int yNb = listY0 + lh * line;
  drawTruncRaw(CONTENT_X, yNb, buf, MAX_LINE_CHARS);
  if (n > 0) {
    drawSignalBarsPaper(SCREEN_WIDTH - 24, yNb, ui_topbar::rssiToBars(avgRssi));
  }
}

static void drawContentInfo() {
  if (!disp) return;
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
  const int listY0Raw = submenuListY0Paper(false) + PAPER_MSG_LIST_TOP_PAD_PX;
  const int minY = PAPER_MSG_MIN_BASELINE_Y + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < minY) ? minY : listY0Raw;
  const int lh = LINE_H;
  const bool tabNoBack = ui_nav_mode::isTabMode();
  const int bottomPad = tabNoBack ? 2 : 6;
  const int yBack = SCREEN_HEIGHT - bottomPad - lh - 1;
  int maxNumLines = (yBack - listY0 - PAPER_MSG_BACK_GAP_PX) / lh;
  if (maxNumLines < 1) maxNumLines = 1;

  char buf[32];
  int n = neighbors::getCount();
  disp->setTextColor(GxEPD_BLACK);
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
    snprintf(buf, sizeof(buf), "%c%c%c%c  %d dBm", hex[0], hex[1], hex[2], hex[3], rssi);
    drawTruncRaw(CONTENT_X, listY0 + (1 + i) * lh, buf, MAX_LINE_CHARS);
  }
  if (extraRow) {
    snprintf(buf, sizeof(buf), "+%d more", n - maxShow);
    drawTruncRaw(CONTENT_X, listY0 + (1 + maxShow) * lh, buf, MAX_LINE_CHARS);
  }

  if (!tabNoBack) {
    disp->fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, GxEPD_BLACK);
    disp->setTextColor(GxEPD_WHITE);
    drawTruncUtf8(CONTENT_X, yBack, locale::getForDisplay("menu_back"), MAX_LINE_CHARS);
    disp->setTextColor(GxEPD_BLACK);
  }
}

static void drawContentNet() {
  if (!disp) return;
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
  const int lh = LINE_H;
  const int bottomPad = 6;
  const int gapMid = ui_nav_mode::isTabMode() ? 1 : 4;
  const int yBack = SCREEN_HEIGHT - bottomPad - lh - 1;
  const int listY0Raw = submenuListY0Paper(false) + PAPER_MSG_LIST_TOP_PAD_PX;
  const int minY = PAPER_MSG_MIN_BASELINE_Y + s_layoutTabShiftY;
  const int yModeRow = (listY0Raw < minY) ? minY : listY0Raw;
  const int yStat0 = yModeRow + lh + gapMid;
  const bool haveMid = (yStat0 + 2 * lh <= yBack - 2);
  const int yStatCompact = yModeRow + lh + 1;
  const bool haveMidCompact = ui_nav_mode::isTabMode() && !haveMid && (yStatCompact + 2 * lh <= yBack - 2);
  const int yDrawStat = haveMid ? yStat0 : (haveMidCompact ? yStatCompact : yStat0);

  const bool isBle = (radio_mode::current() == radio_mode::BLE);
  char buf[40];
  const bool netSel = !ui_nav_mode::isTabMode() || s_tabDrillIn;
  disp->setTextColor(GxEPD_BLACK);

  const char* modeLine = locale::getForDisplay(isBle ? "net_mode_line_ble" : "net_mode_line_wifi");
  if (netSel && s_netMenuIndex == 0) {
    disp->fillRect(0, yModeRow - 1, SCREEN_WIDTH, lh + 1, GxEPD_BLACK);
    disp->setTextColor(GxEPD_WHITE);
  } else {
    disp->setTextColor(GxEPD_BLACK);
  }
  drawTruncUtf8(CONTENT_X, yModeRow + 4, modeLine, MAX_LINE_CHARS);
  disp->setTextColor(GxEPD_BLACK);

  if (haveMid || haveMidCompact) {
    if (isBle) {
      char advName[24];
      ble::getAdvertisingName(advName, sizeof(advName));
      drawTruncUtf8(CONTENT_X, yDrawStat + 0 * lh, advName, MAX_LINE_CHARS);
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
      disp->fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, GxEPD_BLACK);
      disp->setTextColor(GxEPD_WHITE);
    } else {
      disp->fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, GxEPD_WHITE);
      disp->setTextColor(GxEPD_BLACK);
    }
    drawTruncUtf8(CONTENT_X, yBack, locale::getForDisplay("menu_back"), MAX_LINE_CHARS);
    disp->setTextColor(GxEPD_BLACK);
  }
}

static int sysDisplaySubMenuCountPp() {
  return ui_nav_mode::isTabMode() ? 5 : 4;
}
static int sysDisplaySubBackIdxPp() { return sysDisplaySubMenuCountPp() - 1; }
static void clampSysDisplaySubMenuIndexPp() {
  int c = sysDisplaySubMenuCountPp();
  if (s_sysMenuIndex >= c) s_sysMenuIndex = c - 1;
}

static void displaySubFillLabelPaper(int idx, char* buf, size_t bufSz) {
  const bool tabs = ui_nav_mode::isTabMode();
  const int backIdx = sysDisplaySubBackIdxPp();
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

static void sysMenuFillLabelPaper(int idx, char* buf, size_t bufSz) {
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

static int sysMainMenuIndexToExecSelPp(int idx) {
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
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
  const int lh = LINE_H;
  const int rowStep = lh + 2;
  const int bottomPad = 4;
  const int listY0Raw = submenuListY0Paper(false) + PAPER_MSG_LIST_TOP_PAD_PX;
  const int minY = PAPER_MSG_MIN_BASELINE_Y + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < minY) ? minY : listY0Raw;
  int showMax = (SCREEN_HEIGHT - listY0 - bottomPad) / rowStep;
  if (showMax < 1) showMax = 1;

  char buf[40];
  disp->setTextColor(GxEPD_BLACK);

  if (s_sysInDisplaySubmenu) {
    const int kItems = sysDisplaySubMenuCountPp();
    const int backIdx = sysDisplaySubBackIdxPp();
    if (showMax > kItems) showMax = kItems;
    int scrollRef = s_sysScrollOff;
    ui_scroll::syncListWindow(s_sysMenuIndex, kItems, showMax, scrollRef);
    s_sysScrollOff = scrollRef;
    if (s_sysScrollOff > 0) {
      disp->fillTriangle(SCREEN_WIDTH - 8, listY0 + 2, SCREEN_WIDTH - 12, listY0 + 7, SCREEN_WIDTH - 4, listY0 + 7, GxEPD_BLACK);
    }
    for (int vis = 0; vis < showMax; vis++) {
      int idx = s_sysScrollOff + vis;
      if (idx >= kItems) break;
      int y = listY0 + vis * rowStep;
      const bool rowSel = (s_sysMenuIndex == idx);
      if (idx == backIdx) {
        const char* backLbl = locale::getForDisplay("menu_back");
        if (rowSel) {
          disp->fillRect(0, y, SCREEN_WIDTH, rowStep, GxEPD_BLACK);
          disp->setTextColor(GxEPD_WHITE);
        } else {
          disp->setTextColor(GxEPD_BLACK);
        }
        drawTruncUtf8(CONTENT_X, y + 4, backLbl, MAX_LINE_CHARS - 4);
      } else {
        displaySubFillLabelPaper(idx, buf, sizeof(buf));
        if (rowSel) {
          disp->fillRect(0, y, SCREEN_WIDTH, rowStep, GxEPD_BLACK);
          disp->setTextColor(GxEPD_WHITE);
        } else {
          disp->setTextColor(GxEPD_BLACK);
        }
        drawTruncUtf8(CONTENT_X, y + 4, buf, MAX_LINE_CHARS - 4);
      }
    }
    disp->setTextColor(GxEPD_BLACK);
    if (s_sysScrollOff + showMax < kItems) {
      const int triY = listY0 + (showMax - 1) * rowStep + rowStep - 4;
      disp->fillTriangle(SCREEN_WIDTH - 8, triY, SCREEN_WIDTH - 12, triY - 4, SCREEN_WIDTH - 4, triY - 4, GxEPD_BLACK);
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
    disp->fillTriangle(SCREEN_WIDTH - 8, listY0 + 2, SCREEN_WIDTH - 12, listY0 + 7, SCREEN_WIDTH - 4, listY0 + 7, GxEPD_BLACK);
  }
  for (int vis = 0; vis < showMax; vis++) {
    int idx = s_sysScrollOff + vis;
    if (idx >= sysListCount) break;
    int y = listY0 + vis * rowStep;
    const bool rowSel = !sysTabBrowse && (s_sysMenuIndex == idx);
    if (idx == 6) {
      const char* backLbl = locale::getForDisplay("menu_back");
      if (rowSel) {
        disp->fillRect(0, y, SCREEN_WIDTH, rowStep, GxEPD_BLACK);
        disp->setTextColor(GxEPD_WHITE);
      } else {
        disp->setTextColor(GxEPD_BLACK);
      }
      drawTruncUtf8(CONTENT_X, y + 4, backLbl, MAX_LINE_CHARS - 4);
    } else {
      sysMenuFillLabelPaper(idx, buf, sizeof(buf));
      if (rowSel) {
        disp->fillRect(0, y, SCREEN_WIDTH, rowStep, GxEPD_BLACK);
        disp->setTextColor(GxEPD_WHITE);
        drawIconScaled(4, y + 2, ui_icons::sysMenuIcon(idx), GxEPD_WHITE);
        drawTruncUtf8(4 + ICON_W * ICON_SCALE + 6, y + 4, buf, MAX_LINE_CHARS - 4);
      } else {
        disp->setTextColor(GxEPD_BLACK);
        drawIconScaled(4, y + 2, ui_icons::sysMenuIcon(idx), GxEPD_BLACK);
        drawTruncUtf8(4 + ICON_W * ICON_SCALE + 6, y + 4, buf, MAX_LINE_CHARS - 4);
      }
    }
  }
  disp->setTextColor(GxEPD_BLACK);
  if (s_sysScrollOff + showMax < sysListCount) {
    const int triY = listY0 + (showMax - 1) * rowStep + rowStep - 4;
    disp->fillTriangle(SCREEN_WIDTH - 8, triY, SCREEN_WIDTH - 12, triY - 4, SCREEN_WIDTH - 4, triY - 4, GxEPD_BLACK);
  }
}

static void drawContentMsg() {
  if (!disp) return;
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
  const int listY0Raw = submenuListY0Paper(false) + PAPER_MSG_LIST_TOP_PAD_PX;
  const int minY = PAPER_MSG_MIN_BASELINE_Y + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < minY) ? minY : listY0Raw;
  const int lh = LINE_H;
  disp->setTextColor(GxEPD_BLACK);
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
    disp->fillTriangle(SCREEN_WIDTH - 8, listY0 + 1 * lh, SCREEN_WIDTH - 12, listY0 + 1 * lh + 5, SCREEN_WIDTH - 4, listY0 + 1 * lh + 5, GxEPD_BLACK);
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
    disp->fillTriangle(SCREEN_WIDTH - 8, listY0 + 3 * lh + lh - 4, SCREEN_WIDTH - 12, listY0 + 3 * lh - 4, SCREEN_WIDTH - 4, listY0 + 3 * lh - 4, GxEPD_BLACK);
  }

  if (!ui_nav_mode::isTabMode()) {
    const int yBack = listY0 + 4 * lh + PAPER_MSG_BACK_GAP_PX;
    disp->fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, GxEPD_BLACK);
    disp->setTextColor(GxEPD_WHITE);
    drawTruncUtf8(CONTENT_X, yBack, locale::getForDisplay("menu_back"), MAX_LINE_CHARS);
    disp->setTextColor(GxEPD_BLACK);
  }
}

static void drawGpsBackFooterPaper(bool highlight) {
  if (!disp) return;
  const int lh = LINE_H;
  const int bottomPad = 6;
  const int yBack = SCREEN_HEIGHT - bottomPad - lh - 1;
  if (highlight) {
    disp->fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, GxEPD_BLACK);
    disp->setTextColor(GxEPD_WHITE);
  } else {
    disp->fillRect(0, yBack - 1, SCREEN_WIDTH, lh + 1, GxEPD_WHITE);
    disp->setTextColor(GxEPD_BLACK);
  }
  drawTruncUtf8(CONTENT_X, yBack, locale::getForDisplay("menu_back"), MAX_LINE_CHARS);
  disp->setTextColor(GxEPD_BLACK);
}

static void drawContentGps() {
  if (!disp) return;
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
  char buf[40];
  const int lh = LINE_H;
  const int listY0Raw = submenuListY0Paper(false) + PAPER_MSG_LIST_TOP_PAD_PX;
  const int minY = PAPER_MSG_MIN_BASELINE_Y + s_layoutTabShiftY;
  const int listY0 = (listY0Raw < minY) ? minY : listY0Raw;
  const int bottomPad = 6;
  const int yBack = SCREEN_HEIGHT - bottomPad - lh - 1;
  const bool tabDrill = ui_nav_mode::isTabMode() && s_tabDrillIn;
  const bool gpsSel = !ui_nav_mode::isTabMode() || s_tabDrillIn;
  const bool showGpsToggleRow = gps::isPresent() && (tabDrill || !ui_nav_mode::isTabMode());
  const bool showBackFooter = !ui_nav_mode::isTabMode() || tabDrill;
  const int yInfoEnd = showBackFooter ? (yBack - PAPER_MSG_BACK_GAP_PX) : (SCREEN_HEIGHT - 2);
  int y = listY0;

  if (showGpsToggleRow) {
    const bool on = gps::isEnabled();
    snprintf(buf, sizeof(buf), "%s", locale::getForDisplay(on ? "gps_toggle_on_off" : "gps_toggle_off_on"));
    if (s_gpsMenuIndex == 0) {
      disp->fillRect(0, y - 1, SCREEN_WIDTH, lh + 1, GxEPD_BLACK);
      disp->setTextColor(GxEPD_WHITE);
    } else {
      disp->setTextColor(GxEPD_BLACK);
    }
    drawTruncUtf8(CONTENT_X, y + 4, buf, MAX_LINE_CHARS - 4);
    disp->setTextColor(GxEPD_BLACK);
    y += lh;
  }

  if (!gps::isPresent() && gps::hasPhoneSync()) {
    if (y + lh <= yInfoEnd + 1) {
      drawTruncUtf8(CONTENT_X, y + 4, locale::getForDisplay("gps_phone"), MAX_LINE_CHARS - 4);
      y += lh;
    }
    if (showBackFooter) {
      const bool hiBack = gpsSel && !gps::isPresent() && (s_gpsMenuIndex == 0);
      drawGpsBackFooterPaper(hiBack);
    }
    return;
  }
  if (!gps::isPresent()) {
    if (y + lh <= yInfoEnd + 1) {
      drawTruncUtf8(CONTENT_X, y + 4, locale::getForDisplay("gps_not_present"), MAX_LINE_CHARS - 4);
      y += lh;
    }
    if (y + lh <= yInfoEnd + 1) {
      drawTruncRaw(CONTENT_X, y + 4, "BLE: gps rx,tx,en", MAX_LINE_CHARS - 4);
      y += lh;
    }
    if (showBackFooter) {
      const bool hiBack = gpsSel && !gps::isPresent() && (s_gpsMenuIndex == 0);
      drawGpsBackFooterPaper(hiBack);
    }
    return;
  }

  if (y + lh <= yInfoEnd + 1) {
    if (gpsSel && gps::isPresent() && s_gpsMenuIndex == 0 && !showGpsToggleRow) {
      disp->fillRect(0, y - 1, SCREEN_WIDTH, lh + 1, GxEPD_BLACK);
      disp->setTextColor(GxEPD_WHITE);
    } else {
      disp->setTextColor(GxEPD_BLACK);
    }
    drawTruncUtf8(CONTENT_X, y + 4, gps::isEnabled() ? locale::getForDisplay("gps_on") : locale::getForDisplay("gps_off"),
        MAX_LINE_CHARS - 4);
    disp->setTextColor(GxEPD_BLACK);
    y += lh;
  }
  if (y + lh <= yInfoEnd + 1) {
    drawTruncUtf8(CONTENT_X, y + 4, gps::hasFix() ? locale::getForDisplay("gps_fix") : locale::getForDisplay("gps_no_fix"),
        MAX_LINE_CHARS - 4);
    y += lh;
  }
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
    if (line2[0] && y + lh <= yInfoEnd + 1) {
      drawTruncRaw(CONTENT_X, y + 4, line2, MAX_LINE_CHARS - 4);
    }
  }
  if (showBackFooter) {
    const bool hiBack = gpsSel && gps::isPresent() && (s_gpsMenuIndex == 1);
    drawGpsBackFooterPaper(hiBack);
  }
}

static void drawContentPower() {
  if (!disp) return;
  disp->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
  const int listY0Raw = submenuListY0Paper(false) + PAPER_MSG_LIST_TOP_PAD_PX;
  const int minY = PAPER_MSG_MIN_BASELINE_Y + s_layoutTabShiftY;
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
      disp->fillRect(0, y, SCREEN_WIDTH, lh, GxEPD_BLACK);
      disp->setTextColor(GxEPD_WHITE);
    } else {
      disp->setTextColor(GxEPD_BLACK);
    }
    drawTruncUtf8(CONTENT_X, y + 4, items[i], MAX_LINE_CHARS - 4);
  }
}

static void drawScreenContent(int tab) {
  if (ui_nav_mode::isTabMode()) {
    tab = display_tabs::clampNavTabIndex(tab);
    s_currentScreen = tab;
  }
  prepareTabLayoutShiftPaper();
  if (ui_nav_mode::isTabMode()) {
    drawChromeTabsOrIdleRowPaper();
    display_tabs::ContentTab ct = display_tabs::contentForNavTab(tab);
    if (ct != display_tabs::CT_MAIN && ct != display_tabs::CT_MSG && ct != display_tabs::CT_INFO &&
        ct != display_tabs::CT_NET && ct != display_tabs::CT_SYS && ct != display_tabs::CT_GPS && ct != display_tabs::CT_POWER) {
      drawSubmenuTitleBarPaper(ui_section::sectionTitleForContent(ct));
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
    return;
  }
  drawSubScreenChrome();
  {
    display_tabs::ContentTab ct = display_tabs::contentForTab(tab);
    if (ct != display_tabs::CT_MAIN && ct != display_tabs::CT_MSG && ct != display_tabs::CT_INFO &&
        ct != display_tabs::CT_NET && ct != display_tabs::CT_SYS && ct != display_tabs::CT_GPS) {
      drawSubmenuTitleBarPaper(ui_section::sectionTitleForContent(ct));
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
  display_tabs::ContentTab ct = contentTabAtIndex(tab);
  if (!forceUpdate && hash == s_previousImageHash) {
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
  display_tabs::ContentTab ct = contentTabAtIndex(tab);
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
  s_msgScrollStart = 0;
  if (contentTabAtIndex(s_currentScreen) == display_tabs::CT_MSG) s_needRedrawMsg = true;
}

void displayShowScreen(int screen) {
  s_lastActivityTime = millis();
  int nTabs = tabCountForUi();
  if (screen >= nTabs) screen = nTabs - 1;
  const int prevScr = s_currentScreen;
  const bool sysToSys = (screen != prevScr) && contentTabAtIndex(prevScr) == display_tabs::CT_SYS &&
      contentTabAtIndex(screen) == display_tabs::CT_SYS;
  if (!ui_nav_mode::isTabMode() && screen == 0 && s_currentScreen != 0) {
    s_homeMenuIndex = 0;
    s_homeMenuScrollOff = 0;
  }
  if (!ui_nav_mode::isTabMode() && screen == 0 && s_homeMenuIndex >= display_tabs::homeMenuCount()) s_homeMenuIndex = 0;
  if (screen != s_currentScreen && contentTabAtIndex(screen) == display_tabs::CT_NET) s_netMenuIndex = 0;
  if (screen != s_currentScreen && contentTabAtIndex(screen) == display_tabs::CT_SYS && !sysToSys) {
    s_sysMenuIndex = 0;
    s_sysScrollOff = 0;
  }
  if (screen != s_currentScreen) {
    if (!sysToSys) {
      s_tabDrillIn = false;
      s_sysInDisplaySubmenu = false;
    }
    s_powerMenuIndex = 0;
    s_gpsMenuIndex = 0;
  }
  onPaperTabSwitch(s_currentScreen, screen);
  s_currentScreen = screen;
  if (screen != prevScr && sysToSys && !s_sysInDisplaySubmenu && s_sysMenuIndex == 6) {
    s_sysMenuIndex = 0;
    s_sysScrollOff = 0;
  }
  s_previousRunMs = 0;
  bool forceFull = powersave::isEnabled();  // режим экономии: full → hibernate после каждой вкладки
  (void)drawScreen(s_currentScreen, forceFull);
}

void displayShowScreenForceFull(int screen) {
  int nTabs = tabCountForUi();
  if (screen >= nTabs) screen = nTabs - 1;
  const int prevScrFf = s_currentScreen;
  const bool sysToSysFf = (screen != prevScrFf) && contentTabAtIndex(prevScrFf) == display_tabs::CT_SYS &&
      contentTabAtIndex(screen) == display_tabs::CT_SYS;
  if (!ui_nav_mode::isTabMode() && screen == 0 && s_currentScreen != 0) {
    s_homeMenuIndex = 0;
    s_homeMenuScrollOff = 0;
  }
  if (!ui_nav_mode::isTabMode() && screen == 0 && s_homeMenuIndex >= display_tabs::homeMenuCount()) s_homeMenuIndex = 0;
  if (screen != s_currentScreen && contentTabAtIndex(screen) == display_tabs::CT_NET) s_netMenuIndex = 0;
  if (screen != s_currentScreen && contentTabAtIndex(screen) == display_tabs::CT_SYS && !sysToSysFf) {
    s_sysMenuIndex = 0;
    s_sysScrollOff = 0;
  }
  if (screen != s_currentScreen) {
    if (!sysToSysFf) {
      s_tabDrillIn = false;
      s_sysInDisplaySubmenu = false;
    }
    s_powerMenuIndex = 0;
    s_gpsMenuIndex = 0;
  }
  onPaperTabSwitch(s_currentScreen, screen);
  s_currentScreen = screen;
  if (screen != prevScrFf && sysToSysFf && !s_sysInDisplaySubmenu && s_sysMenuIndex == 6) {
    s_sysMenuIndex = 0;
    s_sysScrollOff = 0;
  }
  s_previousRunMs = 0;
  (void)drawScreen(s_currentScreen, true);  // full refresh — против ghosting при смене вкладки
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
    if (ct == display_tabs::CT_SYS && s_sysInDisplaySubmenu) {
      s_sysMenuIndex = (s_sysMenuIndex + 1) % sysDisplaySubMenuCountPp();
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
      s_sysMenuIndex = (s_sysMenuIndex + 1) % sysDisplaySubMenuCountPp();
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

/** Подменю: заголовок, список с иконками. Выход только по SHORT/LONG, без таймаута (ожидание чанками до 30 с). */
static int displayShowPopupMenu(const char* title, const char* items[], int count, int initialSel, int mode,
    const uint8_t* const* rowIcons, bool lastRowNoIcon, bool noRowIcons) {
  (void)mode;
  if (!disp || count <= 0) return -1;
  delay(200);
  prepareTabLayoutShiftPaper();
  s_fastRefreshCount = 0;
  s_msgPartialStreak = 0;
  if (initialSel < 0) initialSel = 0;
  if (initialSel >= count) initialSel = count - 1;
  int selected = initialSel;
  int scrollOff = 0;
  const bool hasTitle = (title && title[0]);
  const int menuY0 = submenuListY0Paper(hasTitle);
  const int rowStep = MENU_LIST_ROW_H_PP;
  const int txOff = MENU_LIST_TEXT_OFF_PP;
  const int icOff = MENU_LIST_ICON_OFF_PP;
  const int hintY = SCREEN_HEIGHT - 12;
  const int maxVisible = (hintY - 2 - menuY0) / rowStep;
  const int showMax = (maxVisible >= 1) ? maxVisible : 1;

  while (1) {
    s_lastActivityTime = millis();
    ui_scroll::syncListWindow(selected, count, showMax, scrollOff);

    yield();
    ensureCooldownBeforeDisplay();
    prepareTabLayoutShiftPaper();
    drawChromeTabsOrIdleRowPaper();
    if (hasTitle) drawSubmenuTitleBarPaper(title);
    disp->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
    int show = count - scrollOff;
    if (show > showMax) show = showMax;
    for (int i = 0; i < show; i++) {
      int idx = scrollOff + i;
      int y = menuY0 + i * rowStep;
      const bool noIcon = noRowIcons || (lastRowNoIcon && (idx == count - 1));
      if (noIcon) {
        if (idx == selected) {
          disp->fillRect(MENU_SEL_X, y, MENU_SEL_W, rowStep, GxEPD_BLACK);
          disp->setTextColor(GxEPD_WHITE);
        } else {
          disp->setTextColor(GxEPD_BLACK);
        }
        drawTruncRaw(4, y + txOff, items[idx], MAX_LINE_CHARS - 2);
        continue;
      }
      const uint8_t* bullet = rowIcons ? rowIcons[idx] : display_tabs::ICON_MAIN;
      if (idx == selected) {
        disp->fillRect(MENU_SEL_X, y, MENU_SEL_W, rowStep, GxEPD_BLACK);
        disp->setTextColor(GxEPD_WHITE);
        drawIconScaled(4, y + icOff, bullet, GxEPD_WHITE);
        drawTruncRaw(4 + ICON_W * ICON_SCALE + 6, y + txOff, items[idx], MAX_LINE_CHARS - 4);
      } else {
        disp->setTextColor(GxEPD_BLACK);
        drawIconScaled(4, y + icOff, bullet, GxEPD_BLACK);
        drawTruncRaw(4 + ICON_W * ICON_SCALE + 6, y + txOff, items[idx], MAX_LINE_CHARS - 4);
      }
    }
    disp->setTextColor(GxEPD_BLACK);
    if (scrollOff > 0)
      disp->fillTriangle(SCREEN_WIDTH - 8, menuY0, SCREEN_WIDTH - 12, menuY0 + 5, SCREEN_WIDTH - 4, menuY0 + 5, GxEPD_BLACK);
    if (scrollOff + showMax < count)
      disp->fillTriangle(SCREEN_WIDTH - 8, hintY - 8, SCREEN_WIDTH - 12, hintY - 13, SCREEN_WIDTH - 4, hintY - 13, GxEPD_BLACK);

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

    for (;;) {
      yield();
      int pt = waitButtonPressWithType(30000);
      if (pt == PRESS_SHORT) {
        selected = (selected + 1) % count;
        for (int i = 0; i < 4; i++) { delay(50); yield(); }
        break;
      }
      if (pt == PRESS_LONG) return selected;
    }
  }
}

/** Меню питания: как popup без заголовка, по центру по вертикали; только SHORT/LONG, без таймаута. Ожидание кнопки — чанками 30 с (лимит waitButtonPressWithType). */
static int displayShowCenteredPowerMenuPaper() {
  const char* items[] = {
    locale::getForDisplay("menu_power_off"),
    locale::getForDisplay("menu_power_sleep"),
    locale::getForDisplay("menu_back"),
  };
  const int count = 3;
  const int txOff = MENU_LIST_TEXT_OFF_PP;
  prepareTabLayoutShiftPaper();
  int rowStep;
  int menuY0;
  if (ui_nav_mode::isTabMode()) {
    rowStep = LINE_H;
    const int listY0Raw = submenuListY0Paper(false) + PAPER_MSG_LIST_TOP_PAD_PX;
    const int minY = PAPER_MSG_MIN_BASELINE_Y + s_layoutTabShiftY;
    menuY0 = (listY0Raw < minY) ? minY : listY0Raw;
  } else {
    rowStep = MENU_LIST_ROW_H_PP;
    const int totalH = count * rowStep;
    menuY0 = (SCREEN_HEIGHT - totalH) / 2;
    const int minY = CONTENT_Y + 2 + s_layoutTabShiftY;
    if (menuY0 < minY) menuY0 = minY;
  }
  if (!disp) return -1;
  delay(200);
  s_fastRefreshCount = 0;
  s_msgPartialStreak = 0;
  int selected = 0;
  if (ui_nav_mode::isTabMode() && s_powerMenuIndex >= 0 && s_powerMenuIndex < count) selected = s_powerMenuIndex;

  for (;;) {
    yield();
    ensureCooldownBeforeDisplay();
    prepareTabLayoutShiftPaper();
    if (ui_nav_mode::isTabMode()) {
      const int listY0Raw = submenuListY0Paper(false) + PAPER_MSG_LIST_TOP_PAD_PX;
      const int minY = PAPER_MSG_MIN_BASELINE_Y + s_layoutTabShiftY;
      menuY0 = (listY0Raw < minY) ? minY : listY0Raw;
    }
    drawChromeTabsOrIdleRowPaper();
    disp->setTextSize((uint8_t)ui_typography::bodyTextSizePaper());
    for (int i = 0; i < count; i++) {
      int y = menuY0 + i * rowStep;
      if (i == selected) {
        disp->fillRect(MENU_SEL_X, y, MENU_SEL_W, rowStep, GxEPD_BLACK);
        disp->setTextColor(GxEPD_WHITE);
      } else {
        disp->setTextColor(GxEPD_BLACK);
      }
      drawTruncRaw(4, y + txOff, items[i], MAX_LINE_CHARS - 2);
    }
    disp->setTextColor(GxEPD_BLACK);

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

    for (;;) {
      int pt = waitButtonPressWithType(30000);
      if (pt == PRESS_SHORT) {
        selected = (selected + 1) % count;
        for (int i = 0; i < 4; i++) { delay(50); yield(); }
        break;
      }
      if (pt == PRESS_LONG) return selected;
    }
  }
}

static void displayShowPowerMenu() {
  int sel = displayShowCenteredPowerMenuPaper();
  if (sel == 0) powersave::deepSleep();
  else if (sel == 1) displaySleep();
}

static void hook_modem_pp() { displayShowModemPicker(); }
static void hook_scan_pp() { displayRunModemScan(); }
static void hook_region_pp() { displayShowRegionPicker(); }
static void hook_lang_pp() { displayShowLanguagePicker(); }
static void hook_selftest_pp() { selftest::run(nullptr); }

static const UiDisplayHooks s_ui_hooks_pp = {hook_modem_pp, hook_scan_pp, hook_region_pp, hook_lang_pp, hook_selftest_pp};

void displayOnLongPress(int screen) {
  (void)screen;
  s_lastActivityTime = millis();
  s_menuActive = true;
  ensureCooldownBeforeDisplay();
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
    (void)drawScreen(s_currentScreen, powersave::isEnabled());
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
      s_previousImageHash = 0;
      s_fastRefreshCount = 0;
      s_msgPartialStreak = 0;
      (void)drawScreen(s_currentScreen, true);
      return;
    }
    if (s_tabDrillIn && (ct == display_tabs::CT_MAIN || ct == display_tabs::CT_MSG || ct == display_tabs::CT_INFO)) {
      s_tabDrillIn = false;
      s_menuActive = false;
      (void)drawScreen(s_currentScreen, powersave::isEnabled());
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
      (void)drawScreen(s_currentScreen, powersave::isEnabled());
      return;
    }
    if (ct == display_tabs::CT_POWER && s_tabDrillIn) {
      if (s_powerMenuIndex == 2) {
        s_tabDrillIn = false;
        s_powerMenuIndex = 0;
        s_menuActive = false;
        s_previousImageHash = 0;
        s_fastRefreshCount = 0;
        s_msgPartialStreak = 0;
        (void)drawScreen(s_currentScreen, powersave::isEnabled());
        return;
      }
      displayShowPowerMenu();
      s_menuActive = false;
      (void)drawScreen(s_currentScreen, powersave::isEnabled());
      return;
    }
  }

  if (ct == display_tabs::CT_MAIN) {
    s_homeMenuIndex = 0;
    s_currentScreen = 0;
    s_menuActive = false;
    s_previousImageHash = 0;
    s_fastRefreshCount = 0;
    s_msgPartialStreak = 0;
    (void)drawScreen(s_currentScreen, true);
    return;
  }

  if (ct == display_tabs::CT_MSG || ct == display_tabs::CT_INFO) {
    s_homeMenuIndex = 0;
    s_currentScreen = 0;
    s_msgScrollStart = 0;
    s_menuActive = false;
    s_previousImageHash = 0;
    s_fastRefreshCount = 0;
    s_msgPartialStreak = 0;
    (void)drawScreen(s_currentScreen, true);
    return;
  }

  if (ct == display_tabs::CT_NET) {
    if (s_netMenuIndex == 0) {
      ui_menu_exec::exec_net_menu(0);
    } else if (ui_nav_mode::isTabMode()) {
      s_netMenuIndex = 0;
      s_msgScrollStart = 0;
      s_tabDrillIn = false;
    } else {
      s_homeMenuIndex = display_tabs::homeMenuSlotForContent(display_tabs::CT_NET);
      s_currentScreen = 0;
      s_msgScrollStart = 0;
    }
    s_menuActive = false;
    s_previousImageHash = 0;
    s_fastRefreshCount = 0;
    s_msgPartialStreak = 0;
    (void)drawScreen(s_currentScreen, true);
    return;
  }

  if (ct == display_tabs::CT_SYS) {
    if (s_sysInDisplaySubmenu) {
      const int bi = sysDisplaySubBackIdxPp();
      if (s_sysMenuIndex == bi) {
        s_sysInDisplaySubmenu = false;
        s_sysMenuIndex = 0;
        s_sysScrollOff = 0;
      } else if (s_sysMenuIndex == 0) {
        s_ui_hooks_pp.show_language_picker();
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
        clampSysDisplaySubMenuIndexPp();
        if (!wasTabs && s_sysMenuIndex == 3) s_sysMenuIndex = 4;
        if (ui_nav_mode::isTabMode() && s_sysInDisplaySubmenu) {
          s_tabDrillIn = true;
        }
      } else if (s_sysMenuIndex == 3 && ui_nav_mode::isTabMode()) {
        ui_display_prefs::cycleTabBarHideIdleSeconds();
      }
      s_menuActive = false;
      s_previousImageHash = 0;
      s_fastRefreshCount = 0;
      s_msgPartialStreak = 0;
      (void)drawScreen(s_currentScreen, true);
      return;
    }
    if (s_sysMenuIndex == 6) {
      if (ui_nav_mode::isTabMode()) {
        s_sysMenuIndex = 0;
        s_sysScrollOff = 0;
        s_msgScrollStart = 0;
        s_tabDrillIn = false;
        s_sysInDisplaySubmenu = false;
      } else {
        s_homeMenuIndex = display_tabs::homeMenuSlotForContent(display_tabs::CT_SYS);
        s_currentScreen = 0;
        s_msgScrollStart = 0;
        s_sysInDisplaySubmenu = false;
      }
    } else if (s_sysMenuIndex == 0) {
      s_sysInDisplaySubmenu = true;
      s_sysMenuIndex = 0;
      s_sysScrollOff = 0;
    } else {
      const int e = sysMainMenuIndexToExecSelPp(s_sysMenuIndex);
      if (e >= 0) ui_menu_exec::exec_sys_menu(e, s_ui_hooks_pp);
    }
    s_menuActive = false;
    s_previousImageHash = 0;
    s_fastRefreshCount = 0;
    s_msgPartialStreak = 0;
    (void)drawScreen(s_currentScreen, true);
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
    s_previousImageHash = 0;
    s_fastRefreshCount = 0;
    s_msgPartialStreak = 0;
    (void)drawScreen(s_currentScreen, true);
    return;
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

  static bool s_prevTabStripPaper = true;
  ui_tab_bar_idle::tick(s_tabDrillIn);
  if (ui_nav_mode::isTabMode()) {
    const bool sh = ui_tab_bar_idle::tabStripVisible();
    if (sh != s_prevTabStripPaper) {
      s_prevTabStripPaper = sh;
      s_previousImageHash = 0;
      (void)drawScreen(s_currentScreen, true);
      return true;
    }
  } else {
    s_prevTabStripPaper = true;
  }

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
        displayHandleShortPress();
        if (prevTab != s_currentScreen) onPaperTabSwitch(prevTab, s_currentScreen);
        bool forceFull = powersave::isEnabled();  // режим экономии: full → hibernate после каждой вкладки
        if (forceFull) drawScreen(s_currentScreen, true);
        else if (!performDisplayUpdate(s_currentScreen, true, true)) drawScreen(s_currentScreen, false);
      } else if (isLong) {
        if (!ui_nav_mode::isTabMode() && display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_HOME) {
          int prevTab = s_currentScreen;
          if (display_tabs::homeMenuIsPowerSlot(s_homeMenuIndex)) {
            displayShowPowerMenu();
          } else {
            s_currentScreen = display_tabs::homeMenuTargetScreen(s_homeMenuIndex);
          }
          if (prevTab != s_currentScreen) onPaperTabSwitch(prevTab, s_currentScreen);
          ensureCooldownBeforeDisplay();
          bool forceFull = powersave::isEnabled();
          if (forceFull) drawScreen(s_currentScreen, true);
          else if (!performDisplayUpdate(s_currentScreen, true, true)) drawScreen(s_currentScreen, false);
        } else {
          displayOnLongPress(s_currentScreen);
        }
      }
      displayNotifyTabChromeActivity();
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
  if (s_needRedrawMsg && contentTabAtIndex(s_currentScreen) == display_tabs::CT_MSG) {
    if (performDisplayUpdate(s_currentScreen, true, true) ||
        drawScreen(s_currentScreen, powersave::isEnabled())) {
      s_needRedrawMsg = false;
    }
    return false;
  }
  if ((now - s_lastDisplayEnd) < EINK_COOLDOWN_HW_MS) return false;

  hibernateIfIdle();  // 30 с неактивности → панель в hibernate

  // Периодика только для вкладок с живыми данными. Lang, Sys, Msg — только по событию
  display_tabs::ContentTab ct = contentTabAtIndex(s_currentScreen);
  const bool tabHasLiveData =
      (ct == display_tabs::CT_HOME || ct == display_tabs::CT_MAIN || ct == display_tabs::CT_MSG || ct == display_tabs::CT_INFO ||
       ct == display_tabs::CT_NET || ct == display_tabs::CT_GPS || ct == display_tabs::CT_POWER);
  if (tabHasLiveData) {
    performDisplayUpdate(s_currentScreen, false);
  }
  return false;
}
