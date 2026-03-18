/**
 * RiftLink Region — EU, RU, US
 * docs/CUSTOM_PROTOCOL_PLAN.md §8.1
 * EU/UK: 3 LoRaWAN канала (868.1, 868.3, 868.5 MHz) — ETSI EN 300 220 G1
 *
 * Блокировка ручной смены: частота и мощность задаются только через пресеты
 * (setRegion/setChannel). Нет API для произвольной freq/power — соответствие
 * регуляторике гарантируется.
 */

#include "region.h"
#include "radio/radio.h"
#include "duty_cycle/duty_cycle.h"
#include <Arduino.h>
#include <nvs.h>
#include <nvs_flash.h>

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

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
    char buf[8] = {0};
    size_t len = sizeof(buf);
    if (nvs_get_str(h, NVS_KEY_REGION, buf, &len) == ESP_OK && buf[0]) {
      s_isSet = true;
      for (size_t i = 0; i < N_PRESETS; i++) {
        if (strcasecmp(buf, PRESETS[i].code) == 0) {
          s_current = PRESETS[i];
          break;
        }
      }
    }
    int8_t ch = 0;
    if (nvs_get_i8(h, NVS_KEY_EU_CHANNEL, &ch) == ESP_OK && ch >= 0 && ch < EU_N_CHANNELS) {
      s_euChannel = ch;
    }
    nvs_close(h);
  }
  s_inited = true;
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

      nvs_handle_t h;
      if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_REGION, s_current.code);
        nvs_commit(h);
        nvs_close(h);
      }

      float freq = getFreqForPreset(s_current);
      radio::applyRegion(freq, s_current.power);
      duty_cycle::reset();
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

bool setChannel(int ch) {
  if (!s_current.hasChannels || ch < 0 || ch >= EU_N_CHANNELS) return false;

  s_euChannel = ch;

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_i8(h, NVS_KEY_EU_CHANNEL, (int8_t)ch);
    nvs_commit(h);
    nvs_close(h);
  }

  float freq = EU_CHANNELS[ch];
  radio::applyRegion(freq, s_current.power);
  duty_cycle::reset();
  Serial.printf("[RiftLink] Channel: %d (%.1f MHz)\n", ch, freq);
  return true;
}

int getPresetCount() {
  return (int)N_PRESETS;
}

const char* getPresetCode(int i) {
  if (i < 0 || i >= (int)N_PRESETS) return "EU";
  return PRESETS[i].code;
}

}  // namespace region
