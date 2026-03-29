/**
 * Компактная строка региона и пресета модема для OLED/T-Pager/Paper:
 * EU0 N, RU N, US M; для Custom — EU C7/125 (SF/BW).
 *
 * Канал без разделителя после кода региона: «[», «:» и др. в пропорциональном
 * шрифте (T‑Pager) дают большой xAdvance — визуально как лишний пробел.
 */
#pragma once

#include <cstddef>
#include <cstdio>
#include "region/region.h"
#include "radio/radio.h"

namespace ui_fmt {

inline void regionModemShort(char* buf, size_t sz) {
  if (!buf || sz < 6) return;
  const char* code = region::getCode();
  radio::ModemPreset mp = radio::getModemPreset();
  if (mp == radio::MODEM_CUSTOM) {
    snprintf(buf, sz, "%s C%u/%.0f", code, (unsigned)radio::getSpreadingFactor(), (double)radio::getBandwidth());
    return;
  }
  char letter = 'N';
  switch (mp) {
    case radio::MODEM_SPEED: letter = 'S'; break;
    case radio::MODEM_NORMAL: letter = 'N'; break;
    case radio::MODEM_RANGE: letter = 'R'; break;
    case radio::MODEM_MAX_RANGE: letter = 'M'; break;
    default: letter = '?'; break;
  }
  if (region::getChannelCount() > 1) {
    snprintf(buf, sz, "%s%u %c", code, (unsigned)region::getChannel(), letter);
  } else {
    snprintf(buf, sz, "%s %c", code, letter);
  }
}

inline void regionModemWithMHz(char* buf, size_t sz) {
  char core[28];
  regionModemShort(core, sizeof(core));
  snprintf(buf, sz, "%.0fMHz %s", (double)region::getFreq(), core);
}

inline void regionModemPowerShort(char* buf, size_t sz) {
  char core[28];
  regionModemShort(core, sizeof(core));
  snprintf(buf, sz, "%s %ddBm", core, region::getPower());
}

}  // namespace ui_fmt
