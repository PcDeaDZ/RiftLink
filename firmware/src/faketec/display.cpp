/**
 * FakeTech Display — OLED автоопределение (SSD1306)
 */

#include "display.h"
#include "board.h"
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
  Wire.begin();
  Wire.setClock(100000);

  if (!probeI2C(OLED_ADDR)) {
    Serial.println("[RiftLink] No OLED display");
    s_present = false;
    return true;
  }

  oled = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire);
  if (!oled->begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[RiftLink] OLED init failed");
    oled = nullptr;
    s_present = false;
    return true;
  }

  oled->clearDisplay();
  oled->setTextSize(1);
  oled->setTextColor(SSD1306_WHITE);
  oled->display();
  s_present = true;
  Serial.println("[RiftLink] OLED OK");
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
