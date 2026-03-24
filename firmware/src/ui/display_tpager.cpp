/**
 * RiftLink Display — LilyGO T-Lora Pager, ST7796 SPI (480×222), LovyanGFX
 * Общая SPI с SX1262 — перед отрисовкой standby LoRa + mutex (как E-Ink).
 */

#include "display.h"
#include "display_tabs.h"
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
/** Энкодер (wiki): A=40, B=41 — смена вкладки */
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

#define TAB_H 28
#define CONTENT_Y 34
#define CONTENT_H (SCREEN_HEIGHT - CONTENT_Y - 6)
#define ICON_W 8
#define ICON_H 8
#define LINE_H 16
#define MAX_LINE_CHARS 52
#define CONTENT_X 8

static constexpr uint32_t COL_BG = 0x000000u;
static constexpr uint32_t COL_FG = 0xFFFFu;
static constexpr uint32_t COL_DIM = 0x8410u;

#define SHORT_PRESS_MS  350
#define LONG_PRESS_MS   500
#define MIN_PRESS_MS    30
#define DISPLAY_SLEEP_MS 30000

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

static std::atomic<int32_t> s_encAccum{0};
static volatile uint8_t s_encLastState = 0;
static const int8_t kEncQuadTable[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

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
  int y = CONTENT_Y + 4 + line * LINE_H;
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
    gfx.setRotation(0);
    gfx.fillScreen(COL_BG);
    gfx.setTextColor(COL_FG, COL_BG);
    gfx.setTextSize(1);
  });
  s_gfxReady = true;
  s_lastActivityTime = millis();
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

void displayShowBootScreen() {
  if (!s_gfxReady) return;
  s_showingBootScreen = true;
  syncDraw([]() {
    gfx.fillScreen(COL_BG);
    gfx.setTextColor(COL_FG, COL_BG);
    gfx.setTextSize(2);
    gfx.setCursor(120, 80);
    gfx.print("RiftLink");
    gfx.setTextSize(1);
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
  if (!s_gfxReady) return false;
  delay(200);
  int pickLang = locale::getLang();
  uint32_t lastPress = millis();
  const uint32_t CONFIRM_MS = 2500;

  while (1) {
    syncDraw([&]() {
      gfx.fillScreen(COL_BG);
      gfx.setTextSize(1);
      gfx.setTextColor(COL_FG, COL_BG);
      drawTruncRaw(8, 8, locale::getForLang("lang_picker_title", pickLang), 40);
      gfx.setCursor(160, 90);
      gfx.print(pickLang == LANG_EN ? "[EN]" : " EN ");
      gfx.print(" ");
      gfx.print(pickLang == LANG_RU ? "[RU]" : " RU ");
      drawTruncRaw(8, 160, locale::getForLang("short_long_hint", pickLang), 40);
    });

    while (millis() - lastPress < CONFIRM_MS) {
      int pt = waitButtonPressWithType(CONFIRM_MS - (millis() - lastPress));
      if (pt == PRESS_SHORT) {
        pickLang = pickLang == LANG_EN ? LANG_RU : LANG_EN;
        lastPress = millis();
        break;
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

  uint32_t lastPress = millis();
  const uint32_t CONFIRM_MS = 2500;

  while (1) {
    syncDraw([&]() {
      gfx.fillScreen(COL_BG);
      gfx.setTextSize(1);
      gfx.setTextColor(COL_FG, COL_BG);
      drawTruncRaw(8, 8, locale::getForDisplay("select_country"), 40);
      gfx.setCursor(8, 50);
      for (int i = 0; i < nPresets; i++) {
        const char* code = region::getPresetCode(i);
        if (i == pickIdx) {
          gfx.print("[");
          gfx.print(code);
          gfx.print("]");
        } else {
          gfx.print(" ");
          gfx.print(code);
          gfx.print(" ");
        }
      }
      drawTruncRaw(8, 120, locale::getForDisplay("short_long_hint"), 40);
    });

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
  if (!s_gfxReady) return;
  delay(200);

  int pickIdx = (int)radio::getModemPreset();
  if (pickIdx < 0 || pickIdx > 4) pickIdx = 1;

  const char* names[] = {"Speed", "Normal", "Range", "MaxRange", "Custom"};
  const char* desc[]  = {"SF7 BW250", "SF7 BW125", "SF10 BW125", "SF12 BW125", ""};

  uint32_t lastPress = millis();
  const uint32_t CONFIRM_MS = 3000;

  while (1) {
    syncDraw([&]() {
      gfx.fillScreen(COL_BG);
      gfx.setTextSize(1);
      gfx.setTextColor(COL_FG, COL_BG);
      drawTruncRaw(8, 8, "Modem Preset", 40);
      gfx.setCursor(8, 40);
      for (int i = 0; i < 5; i++) {
        if (i == pickIdx) { gfx.print("["); gfx.print(names[i]); gfx.print("]"); }
        else { gfx.print(" "); gfx.print(names[i][0]); }
      }
      if (pickIdx < 4) drawTruncRaw(8, 80, desc[pickIdx], 40);
      else {
        char buf[24];
        snprintf(buf, sizeof(buf), "SF%u BW%.0f CR%u",
            radio::getSpreadingFactor(), radio::getBandwidth(), radio::getCodingRate());
        drawTruncRaw(8, 80, buf, 40);
      }
      drawTruncRaw(8, 180, locale::getForDisplay("short_long_hint"), 40);
    });

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
  if (!s_gfxReady) return;
  syncDraw([]() {
    gfx.fillScreen(COL_BG);
    gfx.setTextSize(1);
    gfx.setTextColor(COL_FG, COL_BG);
    drawTruncRaw(8, 8, locale::getForDisplay("scanning"), 40);
    drawTruncRaw(8, 40, "~36s ...", 40);
  });

  static selftest::ScanResult res[6];
  int found = selftest::modemScan(res, 6);

  syncDraw([&]() {
    gfx.fillScreen(COL_BG);
    if (found == 0) {
      drawTruncRaw(8, 8, locale::getForDisplay("scan_empty"), 40);
    } else {
      drawTruncRaw(8, 8, locale::getForDisplay("scan_found"), 40);
      for (int i = 0; i < found && i < 4; i++) {
        char buf[28];
        snprintf(buf, sizeof(buf), "SF%u BW%.0f %ddBm", res[i].sf, res[i].bw, res[i].rssi);
        drawTruncRaw(8, 40 + i * 20, buf, 40);
      }
    }
  });
  for (int i = 0; i < 50; i++) { delay(100); yield(); }
}

static void drawFrame(int activeTab) {
  if (!s_gfxReady) return;
  gfx.fillScreen(COL_BG);
  int nTabs = display_tabs::getTabCount();
  int tabW = SCREEN_WIDTH / nTabs;

  for (int i = 0; i < nTabs; i++) {
    int x = i * tabW;
    int iconX = x + (tabW - ICON_W) / 2;
    int iconY = 6;
    const uint8_t* icon = display_tabs::getIconForTab(i);
    if (i == activeTab) {
      gfx.fillRoundRect(x + 2, 2, tabW - 4, TAB_H - 4, 4, COL_FG);
      gfx.drawBitmap(iconX, iconY, icon, ICON_W, ICON_H, COL_BG);
    } else {
      gfx.drawBitmap(iconX, iconY, icon, ICON_W, ICON_H, COL_FG);
    }
    if (i < nTabs - 1) {
      gfx.drawLine(x + tabW, 4, x + tabW, TAB_H - 4, COL_DIM);
    }
  }
  gfx.setTextColor(COL_FG, COL_BG);
  gfx.drawLine(0, TAB_H, SCREEN_WIDTH - 1, TAB_H, COL_FG);
  gfx.drawRect(0, CONTENT_Y, SCREEN_WIDTH, CONTENT_H, COL_FG);
}

static void drawBatteryIcon(int x, int y, int pct, bool charging) {
  gfx.drawRect(x, y, 14, 7, COL_FG);
  gfx.fillRect(x + 14, y + 2, 2, 3, COL_FG);
  if (pct > 0) {
    int fill = (pct * 10) / 100;
    if (fill < 1 && pct > 0) fill = 1;
    if (fill > 10) fill = 10;
    gfx.fillRect(x + 2, y + 2, fill, 3, COL_FG);
  }
  if (charging) {
    gfx.drawLine(x + 8, y + 1, x + 6, y + 3, COL_FG);
    gfx.drawLine(x + 6, y + 3, x + 9, y + 3, COL_FG);
    gfx.drawLine(x + 9, y + 3, x + 7, y + 5, COL_FG);
  }
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

static int rssiToBars(int rssi) {
  if (rssi >= -75) return 4;
  if (rssi >= -85) return 3;
  if (rssi >= -95) return 2;
  if (rssi >= -105) return 1;
  return 0;
}

static void drawContentMain() {
  char buf[32];
  char nick[33];
  node::getNickname(nick, sizeof(nick));
  if (nick[0]) {
    drawTruncUtf8(CONTENT_X, CONTENT_Y + 4, nick, MAX_LINE_CHARS);
  } else {
    const uint8_t* id = node::getId();
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", id[0], id[1], id[2], id[3]);
    drawTruncRaw(CONTENT_X, CONTENT_Y + 4, buf, MAX_LINE_CHARS);
  }
  if (gps::hasTime()) {
    char clk[8];
    snprintf(clk, sizeof(clk), "%02d:%02d", gps::getHour(), gps::getMinute());
    drawTruncRaw(SCREEN_WIDTH - 56, CONTENT_Y + 4, clk, 8);
  }

  radio::ModemPreset mp = radio::getModemPreset();
  if (mp < radio::MODEM_CUSTOM)
    snprintf(buf, sizeof(buf), "%.0fMHz %s", region::getFreq(), radio::modemPresetName(mp));
  else
    snprintf(buf, sizeof(buf), "%.0fMHz SF%u BW%.0f", region::getFreq(), (unsigned)radio::getSpreadingFactor(), radio::getBandwidth());
  drawTruncRaw(CONTENT_X, CONTENT_Y + 4 + LINE_H, buf, MAX_LINE_CHARS);

  int n = neighbors::getCount();
  int avgRssi = neighbors::getAverageRssi();
  snprintf(buf, sizeof(buf), "%s %d", locale::getForDisplay("neighbors"), n);
  drawTruncRaw(CONTENT_X, CONTENT_Y + 4 + 2 * LINE_H, buf, MAX_LINE_CHARS);
  if (n > 0) drawSignalBars(SCREEN_WIDTH - 40, CONTENT_Y + 4 + 2 * LINE_H, rssiToBars(avgRssi));

  int pct = telemetry::batteryPercent();
  bool chg = telemetry::isCharging();
  int batY = CONTENT_Y + CONTENT_H - 18;
  drawBatteryIcon(SCREEN_WIDTH - 48, batY, pct >= 0 ? pct : 0, chg);
  if (chg) snprintf(buf, sizeof(buf), "%s", locale::getForDisplay("charging"));
  else if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
  else snprintf(buf, sizeof(buf), "--");
  drawTruncRaw(SCREEN_WIDTH - 40, batY, buf, 6);
}

static void drawContentInfo() {
  char buf[32];
  int n = neighbors::getCount();
  snprintf(buf, sizeof(buf), "%s: %d", locale::getForDisplay("peers"), n);
  drawTruncRaw(CONTENT_X, CONTENT_Y + 4, buf, MAX_LINE_CHARS);

  int maxShow = (n > 6) ? 6 : n;
  for (int i = 0; i < maxShow; i++) {
    char hex[17];
    neighbors::getIdHex(i, hex);
    int rssi = neighbors::getRssi(i);
    snprintf(buf, sizeof(buf), "%c%c%c%c %ddBm", hex[0], hex[1], hex[2], hex[3], rssi);
    drawTruncRaw(CONTENT_X, CONTENT_Y + 4 + (1 + i) * LINE_H, buf, 24);
    drawSignalBars(SCREEN_WIDTH - 40, CONTENT_Y + 4 + (1 + i) * LINE_H, rssiToBars(rssi));
  }
  if (n > 6) {
    snprintf(buf, sizeof(buf), "+%d more", n - 6);
    drawTruncRaw(CONTENT_X, CONTENT_Y + 4 + 7 * LINE_H, buf, MAX_LINE_CHARS);
  }
}

static void drawContentNet() {
  char buf[28];
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
  char buf[28];
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
  drawContentLine(0, locale::getForDisplay("from"));
  drawContentLine(1, s_lastMsgFrom[0] ? s_lastMsgFrom : "-");
  drawContentLine(2, s_lastMsgText[0] ? s_lastMsgText : locale::getForDisplay("no_messages"), true);
}

static void drawContentGps() {
  char buf[40];
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
    if (sat > 0 && course >= 0) snprintf(buf, sizeof(buf), "%u sat %0.0f deg %s", (unsigned)sat, course, card);
    else if (sat > 0) snprintf(buf, sizeof(buf), "%u sat", (unsigned)sat);
    else if (course >= 0) snprintf(buf, sizeof(buf), "%0.0f deg %s", course, card);
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
  syncDraw([&]() {
    drawFrame(tab);
    switch (display_tabs::contentForTab(tab)) {
      case display_tabs::CT_MAIN: drawContentMain(); break;
      case display_tabs::CT_MSG:  drawContentMsg(); break;
      case display_tabs::CT_INFO: drawContentInfo(); break;
      case display_tabs::CT_NET:  drawContentNet(); break;
      case display_tabs::CT_SYS:  drawContentSys(); break;
      case display_tabs::CT_GPS:  drawContentGps(); break;
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
  if (display_tabs::contentForTab(s_currentScreen) == display_tabs::CT_MSG) s_needRedrawMsg = true;
}

void displayShowScreen(int screen) {
  int nTabs = display_tabs::getTabCount();
  s_currentScreen = (screen < nTabs) ? screen : (nTabs - 1);
  s_lastActivityTime = millis();
  drawScreen(s_currentScreen);
}

void displayShowScreenForceFull(int screen) {
  displayShowScreen(screen);
}

int displayGetCurrentScreen() { return s_currentScreen; }

int displayGetNextScreen(int current) {
  return (current + 1) % display_tabs::getTabCount();
}

static int displayShowPopupMenu(const char* items[], int count) {
  if (!s_gfxReady || count <= 0) return -1;
  delay(200);
  int selected = 0;
  int scrollOff = 0;
  uint32_t lastPress = millis();
  const uint32_t MENU_TIMEOUT_MS = 10000;
  const int maxVisible = (CONTENT_H - 8) / LINE_H;

  while (1) {
    if (selected < scrollOff) scrollOff = selected;
    if (selected >= scrollOff + maxVisible) scrollOff = selected - maxVisible + 1;

    syncDraw([&]() {
      drawFrame(s_currentScreen);
      gfx.setTextSize(1);
      int show = count - scrollOff;
      if (show > maxVisible) show = maxVisible;
      for (int i = 0; i < show; i++) {
        int idx = scrollOff + i;
        int y = CONTENT_Y + 6 + i * LINE_H;
        if (idx == selected) {
          gfx.fillRect(2, y - 2, SCREEN_WIDTH - 4, LINE_H, COL_FG);
          gfx.setTextColor(COL_BG, COL_FG);
        } else {
          gfx.setTextColor(COL_FG, COL_BG);
        }
        drawTruncRaw(CONTENT_X + 4, y, items[idx], MAX_LINE_CHARS - 1);
      }
      gfx.setTextColor(COL_FG, COL_BG);
      if (scrollOff > 0)
        gfx.fillTriangle(SCREEN_WIDTH - 10, CONTENT_Y + 6, SCREEN_WIDTH - 18, CONTENT_Y + 14, SCREEN_WIDTH - 2, CONTENT_Y + 14, COL_FG);
      if (scrollOff + maxVisible < count)
        gfx.fillTriangle(SCREEN_WIDTH - 10, CONTENT_Y + CONTENT_H - 8, SCREEN_WIDTH - 18, CONTENT_Y + CONTENT_H - 16, SCREEN_WIDTH - 2, CONTENT_Y + CONTENT_H - 16, COL_FG);
    });

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
    gfx.fillScreen(COL_BG);
    gfx.setTextSize(1);
    gfx.setTextColor(COL_FG, COL_BG);
    gfx.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_FG);
    drawTruncRaw(16, 80, line1, MAX_LINE_CHARS);
    if (line2) drawTruncRaw(16, 110, line2, MAX_LINE_CHARS);
  });
  uint32_t start = millis();
  while (millis() - start < durationMs) {
    delay(50);
    yield();
  }
}

bool displayUpdate() {
  if (!s_gfxReady) return false;

  uint32_t now = millis();

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

  if (!s_menuActive && !s_showingBootScreen && !s_buttonPolledExternally) {
    int32_t acc = s_encAccum.exchange(0);
    if (acc != 0) {
      int nTabs = display_tabs::getTabCount();
      int steps = 0;
      if (acc >= 4) steps = (int)(acc / 4);
      else if (acc <= -4) steps = -(int)((-acc) / 4);
      else if (acc > 0) steps = 1;
      else steps = -1;
      if (steps != 0) {
        s_currentScreen = ((s_currentScreen + steps) % nTabs + nTabs) % nTabs;
        s_lastActivityTime = now;
        drawScreen(s_currentScreen);
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
        s_currentScreen = displayGetNextScreen(s_currentScreen);
        drawScreen(s_currentScreen);
      } else if (isLong) displayOnLongPress(s_currentScreen);
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
    display_tabs::ContentTab ct = display_tabs::contentForTab(s_currentScreen);
    if (ct == display_tabs::CT_MAIN || ct == display_tabs::CT_INFO || ct == display_tabs::CT_NET || ct == display_tabs::CT_GPS) drawScreen(s_currentScreen);
  }
  return false;
}
