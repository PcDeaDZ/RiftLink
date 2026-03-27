/**
 * FakeTech Display — OLED автоопределение (SSD1306)
 */

#include "display.h"
#include "board.h"
#include "log.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

static Adafruit_SSD1306* oled = nullptr;
static bool s_present = false;

static bool probeI2C(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

namespace display {

bool init() {
#if defined(RIFTLINK_SKIP_DISPLAY)
  s_present = false;
  RIFTLINK_DIAG("DISPLAY", "event=OLED skip=1 reason=RIFTLINK_SKIP_DISPLAY");
  return true;
#endif
  Wire.setPins(OLED_SDA, OLED_SCL);
  Wire.begin();
  Wire.setClock(100000);
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setTimeout(40);
#endif

  if (!probeI2C(OLED_ADDR)) {
    RIFTLINK_DIAG("DISPLAY", "event=OLED_PROBE ok=0 addr=0x%02X", (unsigned)OLED_ADDR);
    s_present = false;
    return true;
  }

  oled = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire);
  if (!oled->begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    RIFTLINK_DIAG("DISPLAY", "event=OLED_BEGIN ok=0 addr=0x%02X", (unsigned)OLED_ADDR);
    oled = nullptr;
    s_present = false;
    return true;
  }

  oled->clearDisplay();
  oled->setTextSize(1);
  oled->setTextColor(SSD1306_WHITE);
  oled->display();
  s_present = true;
  RIFTLINK_DIAG("DISPLAY", "event=OLED_BEGIN ok=1 addr=0x%02X w=%d h=%d",
      (unsigned)OLED_ADDR, SCREEN_WIDTH, SCREEN_HEIGHT);
  return true;
}

bool isPresent() {
  return s_present;
}

void clear() {
  if (oled) oled->clearDisplay();
}

void setCursor(int c, int r) {
  if (oled) oled->setCursor(c, r * 8);
}

void print(const char* s) {
  if (oled) oled->print(s);
}

void print(int v) {
  if (oled) oled->print(v);
}

void show() {
  if (oled) oled->display();
}

}  // namespace display
