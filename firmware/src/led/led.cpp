/**
 * LED — мигание без блокировки
 */

#include "led.h"
#include <Arduino.h>

static uint8_t s_ledPin = 0;
static uint32_t s_ledOffAt = 0;

void ledInit(uint8_t pin) {
  s_ledPin = pin;
  if (pin) pinMode(pin, OUTPUT);
}

void ledBlink(uint32_t durationMs) {
  if (!s_ledPin || durationMs == 0) return;
  digitalWrite(s_ledPin, HIGH);
  s_ledOffAt = millis() + durationMs;
}

void ledUpdate() {
  if (!s_ledOffAt) return;
  if (millis() >= s_ledOffAt) {
    if (s_ledPin) digitalWrite(s_ledPin, LOW);
    s_ledOffAt = 0;
  }
}
