/**
 * Дисплей nRF: Heltec T114 — встроенный ST7789 (SPI1); иначе SSD1306 I2C.
 */

#include "display_nrf.h"
#include "board_pins.h"
#include "locale/locale.h"

#include <Arduino.h>
#include <cstring>

#if defined(RIFTLINK_BOARD_HELTEC_T114)
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

namespace display_nrf {

namespace {

Adafruit_ST7789 g_tft(&SPI1, TFT_SPI_CS, TFT_SPI_DC, TFT_SPI_RST);

bool g_ok = false;
bool g_last_dirty = false;
uint32_t g_last_poll_ms = 0;
char g_line_from[20] = "";
char g_line_text[48] = "";

}  // namespace

bool init() {
  g_ok = false;
  // Heltec T114: VTFT_CTRL включает питание матрицы (Meshtastic: LOW = on). Иначе подсветка/шина могут «моргать», картинки нет.
  pinMode(TFT_VTFT_CTRL, OUTPUT);
  digitalWrite(TFT_VTFT_CTRL, TFT_VTFT_PWR_ON);
  delay(20);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BL_ON);
  delay(5);
  SPI1.setPins(TFT_SPI_MISO, TFT_SPI_SCK, TFT_SPI_MOSI);
  SPI1.begin();
  // 1.14" ST7789: Adafruit ожидает init(135, 240) для CASET/RASET (см. Adafruit_ST7789.cpp).
  g_tft.init(135, 240);
  g_tft.setRotation(0);
  // Белый на чёрном — перерисовка символов без «просветов»; ниже кадры батчим startWrite/endWrite (меньше артефактов на ST7789).
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(2);
  g_tft.startWrite();
  g_tft.fillScreen(ST77XX_BLACK);
  g_tft.endWrite();
  g_tft.setCursor(0, 0);
  g_ok = true;
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
  g_tft.fillScreen(ST77XX_BLACK);
  g_tft.setCursor(0, 0);
  if (line1) g_tft.println(line1);
  if (line2) g_tft.println(line2);
  g_tft.endWrite();
}

void show_selftest_summary(bool radioOk, bool antennaOk, uint16_t batteryMv, uint32_t heapFree) {
  if (!g_ok) return;
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(2);
  g_tft.startWrite();
  g_tft.fillScreen(ST77XX_BLACK);
  g_tft.setCursor(0, 0);
  g_tft.println(locale::getForDisplay("menu_selftest"));
  g_tft.printf("%s\n", radioOk ? locale::getForDisplay("radio_ok") : locale::getForDisplay("radio_fail"));
  g_tft.printf("%s\n", antennaOk ? locale::getForDisplay("selftest_ant_ok") : locale::getForDisplay("selftest_ant_warn"));
  g_tft.printf("%s %umV\n", locale::getForDisplay("battery"), (unsigned)batteryMv);
  g_tft.printf("%s %u\n", locale::getForDisplay("selftest_heap"), (unsigned)heapFree);
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

void show_status_screen(const char* line1, const char* line2, const char* line3, const char* line4) {
  if (!g_ok) return;
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(2);
  g_tft.startWrite();
  g_tft.fillScreen(ST77XX_BLACK);
  g_tft.setCursor(0, 0);
  if (line1) g_tft.println(line1);
  if (line2) g_tft.println(line2);
  if (line3) g_tft.println(line3);
  if (line4) g_tft.println(line4);
  g_tft.endWrite();
}

void poll() {
  if (!g_ok || !g_last_dirty) return;
  uint32_t now = millis();
  // Реже полный кадр — при активном mesh меньше вспышек чёрного.
  if ((uint32_t)(now - g_last_poll_ms) < 750) return;
  g_last_poll_ms = now;

  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setTextSize(2);
  g_tft.startWrite();
  g_tft.fillScreen(ST77XX_BLACK);
  g_tft.setCursor(0, 0);
  g_tft.println(locale::getForDisplay("last_msg_title"));
  if (g_line_from[0]) g_tft.println(g_line_from);
  if (g_line_text[0]) g_tft.println(g_line_text);
  g_tft.endWrite();
  g_last_dirty = false;
}

static constexpr int kMenuRowT114 = 12;
static constexpr int kMenuTitleH = 14;

void show_menu_list(const char* title, const char* const* labels, int count, int selected, int scroll) {
  if (!g_ok || !labels || count < 1) return;
  if (scroll < 0) scroll = 0;
  if (scroll > count - 1) scroll = count - 1;
  const int maxRows = (240 - kMenuTitleH) / kMenuRowT114;
  if (scroll > selected) scroll = selected;
  if (scroll < selected - maxRows + 1) scroll = selected - maxRows + 1;
  if (scroll < 0) scroll = 0;

  g_tft.setTextWrap(false);
  g_tft.startWrite();
  g_tft.fillScreen(ST77XX_BLACK);
  g_tft.setTextSize(1);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setCursor(0, 2);
  if (title) g_tft.print(title);
  g_tft.drawFastHLine(0, kMenuTitleH - 1, 135, ST77XX_WHITE);

  int y = kMenuTitleH;
  for (int row = 0; row < maxRows; row++) {
    int idx = scroll + row;
    if (idx >= count) break;
    const bool sel = (idx == selected);
    if (sel) {
      g_tft.fillRect(0, y, 135, kMenuRowT114, ST77XX_WHITE);
      g_tft.setTextColor(ST77XX_BLACK, ST77XX_WHITE);
    } else {
      g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    }
    g_tft.setCursor(2, y + 2);
    if (labels[idx]) g_tft.print(labels[idx]);
    y += kMenuRowT114;
  }
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.endWrite();
}

void show_fullscreen_text(const char* title, const char* body) {
  if (!g_ok) return;
  g_tft.startWrite();
  g_tft.fillScreen(ST77XX_BLACK);
  g_tft.setTextSize(1);
  g_tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  g_tft.setCursor(0, 2);
  if (title) g_tft.println(title);
  g_tft.drawFastHLine(0, 12, 135, ST77XX_WHITE);
  int y = 16;
  if (body && body[0]) {
    const char* p = body;
    char line[28];
    while (*p && y < 228) {
      size_t n = 0;
      while (*p && *p != '\n' && n < sizeof(line) - 1) line[n++] = *p++;
      line[n] = 0;
      if (*p == '\n') p++;
      g_tft.setCursor(0, y);
      g_tft.print(line);
      y += 10;
    }
  }
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

void show_status_screen(const char* line1, const char* line2, const char* line3, const char* line4) {
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

void show_menu_list(const char* title, const char* const* labels, int count, int selected, int scroll) {
  if (!g_ok || !labels || count < 1) return;
  if (scroll < 0) scroll = 0;
  const int maxRows = (kScreenH - 10) / kMenuRowOled;
  if (scroll > selected) scroll = selected;
  if (scroll < selected - maxRows + 1) scroll = selected - maxRows + 1;
  if (scroll < 0) scroll = 0;

  g_disp.clearDisplay();
  g_disp.setTextSize(1);
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.setCursor(0, 0);
  if (title) g_disp.println(title);
  int y = 10;
  for (int row = 0; row < maxRows; row++) {
    int idx = scroll + row;
    if (idx >= count) break;
    const bool sel = (idx == selected);
    if (sel) g_disp.fillRect(0, y, kScreenW, kMenuRowOled, SSD1306_WHITE);
    g_disp.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
    g_disp.setCursor(1, y + 1);
    if (labels[idx]) g_disp.print(labels[idx]);
    y += kMenuRowOled;
  }
  g_disp.setTextColor(SSD1306_WHITE);
  g_disp.display();
}

void show_fullscreen_text(const char* title, const char* body) {
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

}  // namespace display_nrf

#endif
