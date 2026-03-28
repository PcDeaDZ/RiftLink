/**
 * RiftLink Region — EU, RU, US
 * docs/CUSTOM_PROTOCOL_PLAN.md §8.1
 * EU/UK: 3 LoRaWAN канала (868.1, 868.3, 868.5 MHz) — ETSI EN 300 220 G1
 *
 * Блокировка ручной смены: частота и мощность задаются только через пресеты
 * (setRegion/setChannel). Нет API для произвольной freq/power — соответствие
 * регуляторике гарантируется.
 */

#include "region/region.h"
#include "radio/radio.h"
#include "kv.h"
#include <Arduino.h>

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_REGION "region"
#define NVS_KEY_EU_CHANNEL "eu_ch"

// EU/UK: 3 LoRaWAN-совместимых канала (ETSI G1, 1% duty cycle)
static const float EU_CHANNELS[] = {868.1f, 868.3f, 868.5f};
static const int EU_N_CHANNELS = 3;

// EU: 868 MHz, 14 dBm (ETSI)
// UK: как EU (UKCA)
// RU: 868.8 MHz, 20 dBm (868.7–869.2)
// US: 915 MHz, 22 dBm (Heltec internal PA max; FCC allows 30 dBm with external PA)
// AU: 915 MHz, 22 dBm (ACMA, 915–928 MHz)
struct Preset {
  const char* code;
  float freq;
  int power;
  bool hasChannels;  // EU/UK — выбор из 3 каналов
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

static float getFreqForPreset(const Preset& p) {
  if (p.hasChannels && s_euChannel >= 0 && s_euChannel < EU_N_CHANNELS) {
    return EU_CHANNELS[s_euChannel];
  }
  return p.freq;
}

namespace region {

void init() {
  if (s_inited) return;

  bool euChLoaded = false;
  {
    char buf[8] = {0};
    size_t len = sizeof(buf) - 1;
    if (riftlink_kv::getBlob(NVS_KEY_REGION, (uint8_t*)buf, &len) && buf[0]) {
      buf[len] = '\0';
      s_isSet = true;
      for (size_t i = 0; i < N_PRESETS; i++) {
        if (strcasecmp(buf, PRESETS[i].code) == 0) {
          s_current = PRESETS[i];
          break;
        }
      }
    }
    int8_t ch = 0;
    if (riftlink_kv::getI8(NVS_KEY_EU_CHANNEL, &ch) && ch >= 0 && ch < EU_N_CHANNELS) {
      s_euChannel = ch;
      euChLoaded = true;
    }
    (void)euChLoaded;
  }
  s_inited = true;
  // Пресет EU в таблице хранит 868.1 как «базу», но для EU/UK реальная частота — EU_CHANNELS[eu_ch] из NVS.
  if (s_current.hasChannels) {
    Serial.printf("[RiftLink] Region init: %s LoRa ch index=%d (%.1f MHz), eu_ch=%s\n",
        s_current.code, s_euChannel, getFreqForPreset(s_current),
        euChLoaded ? "loaded" : "absent (default 0 until setChannel)");
  }
}

bool isSet() {
  return s_isSet;
}

bool setRegion(const char* code) {
  if (!code || !code[0]) return false;

  for (size_t i = 0; i < N_PRESETS; i++) {
    if (strcasecmp(code, PRESETS[i].code) == 0) {
      s_current = PRESETS[i];
      s_isSet = true;

      (void)riftlink_kv::setBlob(NVS_KEY_REGION, (const uint8_t*)s_current.code, strlen(s_current.code) + 1);

      float freq = getFreqForPreset(s_current);
      radio::requestApplyRegion(freq, s_current.power);
      Serial.printf("[RiftLink] Region: %s, %.1f MHz, %d dBm\n",
          s_current.code, freq, s_current.power);
      return true;
    }
  }
  return false;
}

const char* getCode() {
  return s_current.code;
}

float getFreq() {
  return getFreqForPreset(s_current);
}

int getPower() {
  return s_current.power;
}

int getChannelCount() {
  return s_current.hasChannels ? EU_N_CHANNELS : 0;
}

int getChannel() {
  return s_current.hasChannels ? s_euChannel : 0;
}

float getChannelMHz(int idx) {
  if (idx < 0 || idx >= EU_N_CHANNELS) return 0.f;
  return EU_CHANNELS[idx];
}

bool setChannel(int ch) {
  if (!s_current.hasChannels || ch < 0 || ch >= EU_N_CHANNELS) return false;

  s_euChannel = ch;

  (void)riftlink_kv::setI8(NVS_KEY_EU_CHANNEL, (int8_t)ch);

  float freq = EU_CHANNELS[ch];
  radio::requestApplyRegion(freq, s_current.power);
  Serial.printf("[RiftLink] Channel: %d (%.1f MHz)\n", ch, freq);
  return true;
}

#define CHANNEL_HOP_MIN_MS 30000  // не чаще 1 раза в 30 с
static uint32_t s_lastChannelHop = 0;

void switchChannelOnCongestion() {
  if (!s_current.hasChannels || EU_N_CHANNELS < 2) return;
  uint32_t now = (uint32_t)millis();
  if (now - s_lastChannelHop < CHANNEL_HOP_MIN_MS) return;
  s_lastChannelHop = now;
  int next = (s_euChannel + 1) % EU_N_CHANNELS;
  setChannel(next);
}

int getPresetCount() {
  return (int)N_PRESETS;
}

const char* getPresetCode(int i) {
  if (i < 0 || i >= (int)N_PRESETS) return "EU";
  return PRESETS[i].code;
}

}  // namespace region
