/**
 * RiftLink Duty Cycle — EU 1% = 36 сек/час
 */

#include "duty_cycle.h"
#include "region/region.h"
#include <Arduino.h>

#define PERIOD_MS      (3600UL * 1000UL)   // 1 час
#define EU_LIMIT_MS    36000UL              // 1% от часа

static uint32_t s_usedMs = 0;
static uint32_t s_periodStart = 0;

namespace duty_cycle {

bool canSend(uint32_t durationUs) {
  uint32_t limitMs = 0;
  const char* r = region::getCode();
  if (strcmp(r, "EU") == 0 || strcmp(r, "UK") == 0) {
    limitMs = EU_LIMIT_MS;
  }
  if (limitMs == 0) return true;  // RU, US — без ограничения

  uint32_t now = millis();
  if (s_periodStart == 0 || (now - s_periodStart) >= PERIOD_MS) {
    s_periodStart = now;
    s_usedMs = 0;
  }

  uint32_t addMs = (durationUs + 999) / 1000;
  return (s_usedMs + addMs) <= limitMs;
}

void recordSend(uint32_t durationUs) {
  const char* r = region::getCode();
  if (strcmp(r, "EU") != 0 && strcmp(r, "UK") != 0) return;

  uint32_t addMs = (durationUs + 999) / 1000;
  s_usedMs += addMs;
}

void reset() {
  s_periodStart = 0;
  s_usedMs = 0;
}

}  // namespace duty_cycle
