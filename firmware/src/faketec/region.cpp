/**
 * FakeTech Region — EU, RU, US
 */

#include "region.h"
#include "storage.h"
#include <Arduino.h>
#include <string.h>

static const float EU_CHANNELS[] = {868.1f, 868.3f, 868.5f};
static const int EU_N_CHANNELS = 3;

struct Preset {
  const char* code;
  float freq;
  int power;
  bool hasChannels;
};

static const Preset PRESETS[] = {
  {"EU", 868.1f, 14, true},
  {"UK", 868.1f, 14, true},
  {"RU", 868.8f, 20, false},
  {"US", 915.0f, 22, false},
  {"AU", 915.0f, 22, false},
};
static const size_t N_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

static Preset s_current = {"EU", 868.1f, 14, true};
static int s_euChannel = 0;
static bool s_inited = false;
static bool s_isSet = false;

#define STORAGE_KEY_REGION "region"
#define STORAGE_KEY_EU_CH "eu_ch"

namespace region {

void init() {
  if (s_inited) return;

  char buf[8] = {0};
  if (storage::getStr(STORAGE_KEY_REGION, buf, sizeof(buf)) && buf[0]) {
    s_isSet = true;
    for (size_t i = 0; i < N_PRESETS; i++) {
      if (strcasecmp(buf, PRESETS[i].code) == 0) {
        s_current = PRESETS[i];
        break;
      }
    }
  }
  int8_t ch = 0;
  if (storage::getI8(STORAGE_KEY_EU_CH, &ch) && ch >= 0 && ch < EU_N_CHANNELS) {
    s_euChannel = ch;
  }
  s_inited = true;
}

bool isSet() { return s_isSet; }

bool setRegion(const char* code) {
  for (size_t i = 0; i < N_PRESETS; i++) {
    if (strcasecmp(code, PRESETS[i].code) == 0) {
      s_current = PRESETS[i];
      s_isSet = true;
      storage::setStr(STORAGE_KEY_REGION, s_current.code);
      return true;
    }
  }
  return false;
}

const char* getCode() { return s_current.code; }

float getFreq() {
  if (s_current.hasChannels && s_euChannel >= 0 && s_euChannel < EU_N_CHANNELS) {
    return EU_CHANNELS[s_euChannel];
  }
  return s_current.freq;
}

int getPower() { return s_current.power; }

int getChannelCount() {
  return s_current.hasChannels ? EU_N_CHANNELS : 0;
}

int getChannel() { return s_euChannel; }

bool setChannel(int ch) {
  if (!s_current.hasChannels || ch < 0 || ch >= EU_N_CHANNELS) return false;
  s_euChannel = ch;
  storage::setI8(STORAGE_KEY_EU_CH, (int8_t)ch);
  return true;
}

void switchChannelOnCongestion() {
  if (s_current.hasChannels) {
    s_euChannel = (s_euChannel + 1) % EU_N_CHANNELS;
    storage::setI8(STORAGE_KEY_EU_CH, (int8_t)s_euChannel);
  }
}

int getPresetCount() { return N_PRESETS; }

const char* getPresetCode(int i) {
  if (i >= 0 && (size_t)i < N_PRESETS) return PRESETS[i].code;
  return "EU";
}

}  // namespace region
