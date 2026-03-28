/**
 * Дисплей nRF: Heltec T114 — встроенный ST7789 (SPI1); иначе SSD1306 I2C.
 */

#include "display_nrf.h"
#include "board_pins.h"

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
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BL_ON);
  SPI1.setPins(TFT_SPI_MISO, TFT_SPI_SCK, TFT_SPI_MOSI);
  SPI1.begin();
  g_tft.init(135, 240);
  g_tft.setRotation(0);
  g_tft.fillScreen(ST77XX_BLACK);
  g_tft.setTextColor(ST77XX_WHITE);
  g_tft.setTextSize(2);
  g_tft.setCursor(0, 0);
  g_ok = true;
  return true;
}

bool is_ready() {
  return g_ok;
}

void show_boot(const char* line1, const char* line2) {
  if (!g_ok) return;
  g_tft.fillScreen(ST77XX_BLACK);
  g_tft.setCursor(0, 0);
  g_tft.setTextSize(2);
  if (line1) g_tft.println(line1);
  if (line2) g_tft.println(line2);
}

void show_selftest_summary(bool radioOk, bool antennaOk, uint16_t batteryMv, uint32_t heapFree) {
  if (!g_ok) return;
  g_tft.fillScreen(ST77XX_BLACK);
  g_tft.setCursor(0, 0);
  g_tft.setTextSize(2);
  g_tft.println(F("Selftest"));
  g_tft.printf("Radio %s\n", radioOk ? "OK" : "FAIL");
  g_tft.printf("Ant %s\n", antennaOk ? "OK" : "WARN");
  g_tft.printf("Bat %umV\n", (unsigned)batteryMv);
  g_tft.printf("Heap %u\n", (unsigned)heapFree);
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

  g_tft.fillScreen(ST77XX_BLACK);
  g_tft.setCursor(0, 0);
  g_tft.setTextSize(2);
  g_tft.println(F("Last msg"));
  if (g_line_from[0]) g_tft.println(g_line_from);
  if (g_line_text[0]) g_tft.println(g_line_text);
  g_last_dirty = false;
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
  g_disp.println(F("Selftest"));
  g_disp.printf("Radio %s\n", radioOk ? "OK" : "FAIL");
  g_disp.printf("Ant   %s\n", antennaOk ? "OK" : "WARN");
  g_disp.printf("Bat %umV\n", (unsigned)batteryMv);
  g_disp.printf("Heap %u\n", (unsigned)heapFree);
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
  g_disp.println(F("Last msg"));
  if (g_line_from[0]) {
    g_disp.println(g_line_from);
  }
  if (g_line_text[0]) {
    g_disp.println(g_line_text);
  }
  g_disp.display();
  g_last_dirty = false;
}

}  // namespace display_nrf

#endif
