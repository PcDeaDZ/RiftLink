/**
 * GPS для nRF52840: без модуля GNSS на плате — только BLE phone sync и KV (как NVS на ESP).
 */

#include "gps/gps.h"
#include "kv.h"

#include <Arduino.h>

namespace gps {

static bool s_inited = false;
static int s_pinRx = -1;
static int s_pinTx = -1;
static int s_pinEn = -1;
static bool s_enabled = false;

static int64_t s_phoneUtcMs = 0;
static float s_phoneLat = 0.0f;
static float s_phoneLon = 0.0f;
static int16_t s_phoneAlt = 0;
static uint32_t s_phoneSyncTime = 0;

static constexpr const char* KV_GPS_RX = "gps_rx";
static constexpr const char* KV_GPS_TX = "gps_tx";
static constexpr const char* KV_GPS_EN = "gps_en";
static constexpr const char* KV_GPS_PWR = "gps_pwr";

void init() {
  if (s_inited) return;
  int8_t v = -1;
  if (riftlink_kv::getI8(KV_GPS_RX, &v) && v >= -1) s_pinRx = v;
  v = -1;
  if (riftlink_kv::getI8(KV_GPS_TX, &v) && v >= -1) s_pinTx = v;
  v = -1;
  if (riftlink_kv::getI8(KV_GPS_EN, &v) && v >= -1) s_pinEn = v;
  int8_t pwr = 0;
  if (riftlink_kv::getI8(KV_GPS_PWR, &pwr)) s_enabled = (pwr != 0);
  s_inited = true;
}

void update() {}

bool isPresent() {
  return false;
}

bool isEnabled() {
  return s_enabled;
}

void setEnabled(bool on) {
  s_enabled = on;
  (void)riftlink_kv::setI8(KV_GPS_PWR, on ? 1 : 0);
}

void toggle() {
  setEnabled(!s_enabled);
}

bool hasFix() {
  return hasPhoneSync();
}

float getLat() {
  return hasPhoneSync() ? s_phoneLat : 0.0f;
}

float getLon() {
  return hasPhoneSync() ? s_phoneLon : 0.0f;
}

int16_t getAlt() {
  return hasPhoneSync() ? s_phoneAlt : 0;
}

uint32_t getSatellites() {
  return 0;
}

float getCourseDeg() {
  return -1.0f;
}

const char* getCourseCardinal() {
  return "";
}

void setPins(int rx, int tx, int en) {
  s_pinRx = rx;
  s_pinTx = tx;
  s_pinEn = en;
}

void getPins(int* rx, int* tx, int* en) {
  if (rx) *rx = s_pinRx;
  if (tx) *tx = s_pinTx;
  if (en) *en = s_pinEn;
}

static int8_t pinToI8(int p) {
  if (p < -1) return -1;
  if (p > 127) return 127;
  return static_cast<int8_t>(p);
}

void saveConfig() {
  (void)riftlink_kv::setI8(KV_GPS_RX, pinToI8(s_pinRx));
  (void)riftlink_kv::setI8(KV_GPS_TX, pinToI8(s_pinTx));
  (void)riftlink_kv::setI8(KV_GPS_EN, pinToI8(s_pinEn));
  (void)riftlink_kv::setI8(KV_GPS_PWR, s_enabled ? 1 : 0);
}

void setPhoneSync(int64_t utcMs, float lat, float lon, int16_t alt) {
  s_phoneUtcMs = utcMs;
  s_phoneLat = lat;
  s_phoneLon = lon;
  s_phoneAlt = alt;
  s_phoneSyncTime = millis();
}

bool hasPhoneSync() {
  return s_phoneSyncTime != 0 && (millis() - s_phoneSyncTime) < 60000;
}

bool hasTime() {
  return hasPhoneSync();
}

bool hasEpochTime() {
  return hasPhoneSync();
}

bool getEpochSec(uint32_t* outEpochSec) {
  if (!outEpochSec || !hasPhoneSync()) return false;
  uint64_t now = static_cast<uint64_t>(s_phoneUtcMs) + (millis() - s_phoneSyncTime);
  *outEpochSec = static_cast<uint32_t>(now / 1000ULL);
  return true;
}

int getHour() {
  if (!hasPhoneSync()) return -1;
  uint64_t now = static_cast<uint64_t>(s_phoneUtcMs) + (millis() - s_phoneSyncTime);
  return static_cast<int>((now / 3600000ULL) % 24);
}

int getMinute() {
  if (!hasPhoneSync()) return -1;
  uint64_t now = static_cast<uint64_t>(s_phoneUtcMs) + (millis() - s_phoneSyncTime);
  return static_cast<int>((now / 60000ULL) % 60);
}

int getLocalHour() {
  return getHour();
}

int getLocalMinute() {
  return getMinute();
}

}  // namespace gps
