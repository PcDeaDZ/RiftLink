/**
 * GPS для nRF52840:
 * - Heltec T114: UART1 + L76K (NMEA, TinyGPS++), пины как Meshtastic / board_pins.h, KV как NVS.
 * - FakeTech и пр.: без UART GNSS — только BLE phone sync и KV.
 */

#include "gps/gps.h"
#include "kv.h"
#include "nrf_wdt_feed.h"

#include <Arduino.h>
#include <cctype>
#include <cstring>
#include <TinyGPSPlus.h>

#if defined(RIFTLINK_BOARD_HELTEC_T114)
#include "board_pins.h"
#endif

namespace gps {

static constexpr const char* KV_GPS_RX = "gps_rx";
static constexpr const char* KV_GPS_TX = "gps_tx";
static constexpr const char* KV_GPS_EN = "gps_en";
static constexpr const char* KV_GPS_PWR = "gps_pwr";

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

#if defined(RIFTLINK_BOARD_HELTEC_T114)

static TinyGPSPlus s_gps;

static char s_nmeaLine[256];
static size_t s_nmeaLineLen;
static bool s_manualTimeValid;
static uint8_t s_manualHour;
static uint8_t s_manualMinute;
static uint32_t s_manualTimeMs;
static uint32_t s_lastZdaMs;
static int16_t s_zdaLtzOffsetMinutes;

static constexpr uint32_t kManualGpsTimeFreshMs = 300000;
static constexpr uint32_t kZdaSuppressRmcGgaMs = 4000;

static bool sentenceIsZda(const char* p, const char* c1) {
  if ((size_t)(c1 - p) != 6) return false;
  return strncmp(p + 1, "GPZDA", 5) == 0 || strncmp(p + 1, "GNZDA", 5) == 0 ||
         strncmp(p + 1, "GLZDA", 5) == 0 || strncmp(p + 1, "GQZDA", 5) == 0;
}

static bool sentenceIsRmcOrGga(const char* p, const char* c1) {
  for (const char* q = p; q + 3 <= c1; ++q) {
    if (strncmp(q, "RMC", 3) == 0 || strncmp(q, "GGA", 3) == 0) return true;
  }
  return false;
}

static bool zdaRecentEnoughToSuppressRmcGga() {
  return s_lastZdaMs != 0 && (millis() - s_lastZdaMs) < kZdaSuppressRmcGgaMs;
}

static bool copyTimeFieldHhmmss(const char* c1, const char* c2, int* outH, int* outM, int* outS) {
  const char* tb = c1 + 1;
  while (tb < c2 && isspace(static_cast<unsigned char>(*tb))) tb++;
  if (static_cast<size_t>(c2 - tb) < 6) return false;
  for (int j = 0; j < 6; j++) {
    if (tb[j] < '0' || tb[j] > '9') return false;
  }
  int h = (tb[0] - '0') * 10 + (tb[1] - '0');
  int mi = (tb[2] - '0') * 10 + (tb[3] - '0');
  int s = (tb[4] - '0') * 10 + (tb[5] - '0');
  if (h > 23 || mi > 59 || s > 59) return false;
  *outH = h;
  *outM = mi;
  *outS = s;
  return true;
}

static void parseZdaLtzFromLine(const char* line) {
  const char* p = line;
  for (int i = 0; i < 5; i++) {
    p = strchr(p, ',');
    if (!p) return;
    p++;
  }
  int ltzh = 0;
  int ltzm = 0;
  {
    const char* q = p;
    while (*q && isspace(static_cast<unsigned char>(*q))) q++;
    int sign = 1;
    if (*q == '-') {
      sign = -1;
      q++;
    } else if (*q == '+')
      q++;
    while (*q >= '0' && *q <= '9') {
      ltzh = ltzh * 10 + (*q - '0');
      q++;
      if (ltzh > 13) break;
    }
    ltzh *= sign;
  }
  p = strchr(p, ',');
  if (!p) return;
  p++;
  {
    const char* q = p;
    while (*q && isspace(static_cast<unsigned char>(*q))) q++;
    int sign = 1;
    if (*q == '-') {
      sign = -1;
      q++;
    } else if (*q == '+')
      q++;
    while (*q >= '0' && *q <= '9') {
      ltzm = ltzm * 10 + (*q - '0');
      q++;
      if (ltzm > 59) break;
    }
    ltzm *= sign;
  }
  if (ltzm < -59 || ltzm > 59) ltzm = 0;
  s_zdaLtzOffsetMinutes = static_cast<int16_t>(ltzh * 60 + ltzm);
}

static void parseNmeaLineForTime(const char* line) {
  if (!line) return;
  const char* p = line;
  while (*p && isspace(static_cast<unsigned char>(*p))) p++;
  if (*p != '$') return;
  const char* c1 = strchr(p, ',');
  if (!c1) return;
  const bool isZda = sentenceIsZda(p, c1);
  const bool isRmcGga = sentenceIsRmcOrGga(p, c1);
  if (!isZda && !isRmcGga) return;
  if (isRmcGga && zdaRecentEnoughToSuppressRmcGga()) return;

  const char* c2 = strchr(c1 + 1, ',');
  if (!c2) return;
  int h = 0, mi = 0, sec = 0;
  if (!copyTimeFieldHhmmss(c1, c2, &h, &mi, &sec)) return;
  s_manualHour = static_cast<uint8_t>(h);
  s_manualMinute = static_cast<uint8_t>(mi);
  s_manualTimeValid = true;
  s_manualTimeMs = millis();
  if (isZda) {
    parseZdaLtzFromLine(line);
    s_lastZdaMs = millis();
  }
}

static void feedNmeaLine(char c) {
  if (c == '\r' || c == '\n') {
    s_nmeaLine[s_nmeaLineLen] = '\0';
    if (s_nmeaLineLen > 0) parseNmeaLineForTime(s_nmeaLine);
    s_nmeaLineLen = 0;
    return;
  }
  if (s_nmeaLineLen < sizeof(s_nmeaLine) - 1) s_nmeaLine[s_nmeaLineLen++] = static_cast<char>(c);
}

static bool manualTimeFresh() {
  return s_manualTimeValid && (millis() - s_manualTimeMs) < kManualGpsTimeFreshMs;
}

static void applyPowerT114(bool on) {
  pinMode(T114_VEXT_EN_PIN, OUTPUT);
  digitalWrite(T114_VEXT_EN_PIN, on ? T114_VEXT_ON : LOW);
  if (s_pinEn >= 0) {
    pinMode(s_pinEn, OUTPUT);
    digitalWrite(s_pinEn, on ? HIGH : LOW);
  }
}

static void uartEnd() {
  Serial1.end();
}

static void uartBegin() {
  if (s_pinRx < 0 || s_pinTx < 0) return;
  Serial1.setPins(static_cast<uint8_t>(s_pinRx), static_cast<uint8_t>(s_pinTx));
  Serial1.begin(9600);
}

static bool waitUartActivity(uint32_t probeMs) {
  const uint32_t start = millis();
  while (millis() - start < probeMs) {
    if (Serial1.available() > 0) return true;
    delay(10);
  }
  return false;
}

static bool gpsTimeUsableForDisplay() {
  if (!s_gps.time.isValid() || s_gps.time.age() >= 120000) return false;
  if (s_gps.location.isValid()) {
    if (s_gps.time.hour() == 0 && s_gps.time.minute() == 0 && s_gps.time.second() == 0) return false;
    return true;
  }
  if (s_gps.date.isValid() && s_gps.date.age() < 120000) {
    if (s_gps.time.hour() == 0 && s_gps.time.minute() == 0 && s_gps.time.second() == 0) return false;
    return true;
  }
  if (s_gps.time.hour() == 0 && s_gps.time.minute() == 0 && s_gps.time.second() == 0) return false;
  return true;
}

#endif  // RIFTLINK_BOARD_HELTEC_T114

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

void init() {
  if (s_inited) return;
  bool hadRx = false, hadTx = false, hadEn = false;
  int8_t v = -1;
  if (riftlink_kv::getI8(KV_GPS_RX, &v) && v >= -1) {
    s_pinRx = v;
    hadRx = true;
  }
  v = -1;
  if (riftlink_kv::getI8(KV_GPS_TX, &v) && v >= -1) {
    s_pinTx = v;
    hadTx = true;
  }
  v = -1;
  if (riftlink_kv::getI8(KV_GPS_EN, &v) && v >= -1) {
    s_pinEn = v;
    hadEn = true;
  }
  const bool hadAnyPins = hadRx || hadTx || hadEn;
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  if (s_pinRx < 0 || s_pinTx < 0) {
    s_pinRx = T114_GPS_RX_PIN;
    s_pinTx = T114_GPS_TX_PIN;
    if (s_pinEn < 0) s_pinEn = T114_GPS_STANDBY_PIN;
  }
#endif
  int8_t pwr = 0;
  const bool hadPwrKey = riftlink_kv::getI8(KV_GPS_PWR, &pwr);
  if (hadPwrKey) {
    s_enabled = (pwr != 0);
  } else {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
    if (hadAnyPins && s_pinRx >= 0 && s_pinTx >= 0) {
      s_enabled = true;
    } else if (!hadAnyPins) {
      s_enabled = true;
    }
#else
    if (hadAnyPins && s_pinRx >= 0 && s_pinTx >= 0) s_enabled = true;
#endif
  }

#if defined(RIFTLINK_BOARD_HELTEC_T114)
  s_nmeaLineLen = 0;
  s_manualTimeValid = false;
  s_lastZdaMs = 0;
  s_zdaLtzOffsetMinutes = 0;

  applyPowerT114(s_enabled);
  if (s_enabled) delay(200);

  if (s_enabled && s_pinRx >= 0 && s_pinTx >= 0) {
    uartBegin();
    if (!waitUartActivity(3000)) {
      const int rx0 = s_pinRx;
      const int tx0 = s_pinTx;
      s_pinRx = tx0;
      s_pinTx = rx0;
      uartEnd();
      uartBegin();
      if (waitUartActivity(2000)) {
        saveConfig();
      } else {
        s_pinRx = rx0;
        s_pinTx = tx0;
        uartEnd();
        uartBegin();
      }
    }
  }
#endif

  s_inited = true;
}

void update() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  if (!s_enabled || s_pinRx < 0 || s_pinTx < 0) return;
  
  // Ограничиваем количество обрабатываемых байт за один вызов — иначе watchdog reset
  // при длинном потоке NMEA (например, при подключении GPS после простоя)
  constexpr uint8_t kMaxBytesPerUpdate = 64;
  uint8_t count = 0;
  while (Serial1.available() > 0 && count < kMaxBytesPerUpdate) {
    const char c = static_cast<char>(Serial1.read());
    feedNmeaLine(c);
    (void)s_gps.encode(c);
    count++;
    // Feed watchdog каждые 16 байт — иначе reset при длинном потоке
    if ((count & 0xFu) == 0) riftlink_wdt_feed();
  }
#endif
}

bool isPresent() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  return s_pinRx >= 0 && s_pinTx >= 0;
#else
  return false;
#endif
}

bool isEnabled() {
  return s_enabled;
}

void setEnabled(bool on) {
  s_enabled = on;
  (void)riftlink_kv::setI8(KV_GPS_PWR, on ? 1 : 0);
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  uartEnd();
  applyPowerT114(on);
  if (on) delay(200);
  if (on && s_pinRx >= 0 && s_pinTx >= 0) uartBegin();
#endif
}

void toggle() {
  setEnabled(!s_enabled);
}

bool hasFix() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  if (!s_gps.location.isValid()) return false;
  const unsigned long age = s_gps.location.age();
  if (age == 0xFFFFFFFFUL) return false;
  return age < 30000UL;
#else
  return hasPhoneSync();
#endif
}

float getLat() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  return s_gps.location.lat();
#else
  return hasPhoneSync() ? s_phoneLat : 0.0f;
#endif
}

float getLon() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  return s_gps.location.lng();
#else
  return hasPhoneSync() ? s_phoneLon : 0.0f;
#endif
}

int16_t getAlt() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  return s_gps.altitude.isValid() ? (int16_t)s_gps.altitude.meters() : 0;
#else
  return hasPhoneSync() ? s_phoneAlt : 0;
#endif
}

uint32_t getSatellites() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  return s_gps.satellites.isValid() ? s_gps.satellites.value() : 0;
#else
  return 0;
#endif
}

float getCourseDeg() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  return s_gps.course.isValid() ? (float)s_gps.course.deg() : -1.0f;
#else
  return -1.0f;
#endif
}

const char* getCourseCardinal() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  return s_gps.course.isValid() ? TinyGPSPlus::cardinal(s_gps.course.deg()) : "";
#else
  return "";
#endif
}

void setPins(int rx, int tx, int en) {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  const bool wasOn = s_enabled;
  uartEnd();
  if (wasOn) applyPowerT114(false);
#endif
  s_pinRx = rx;
  s_pinTx = tx;
  s_pinEn = en;
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  if (rx >= 0 && tx >= 0 && wasOn) {
    applyPowerT114(true);
    delay(200);
    uartBegin();
  }
#endif
}

void getPins(int* rx, int* tx, int* en) {
  if (rx) *rx = s_pinRx;
  if (tx) *tx = s_pinTx;
  if (en) *en = s_pinEn;
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
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  if (manualTimeFresh()) return true;
  if (gpsTimeUsableForDisplay()) return true;
#endif
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
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  if (manualTimeFresh()) return (int)s_manualHour;
  if (gpsTimeUsableForDisplay()) {
    const int h = s_gps.time.hour();
    if (h == 0 && s_gps.time.minute() == 0 && s_gps.time.second() == 0) return -1;
    return h;
  }
#endif
  if (hasPhoneSync()) {
    uint64_t now = static_cast<uint64_t>(s_phoneUtcMs) + (millis() - s_phoneSyncTime);
    return static_cast<int>((now / 3600000ULL) % 24);
  }
  return -1;
}

int getMinute() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  if (manualTimeFresh()) return (int)s_manualMinute;
  if (gpsTimeUsableForDisplay()) {
    if (s_gps.time.hour() == 0 && s_gps.time.minute() == 0 && s_gps.time.second() == 0) return -1;
    return s_gps.time.minute();
  }
#endif
  if (hasPhoneSync()) {
    uint64_t now = static_cast<uint64_t>(s_phoneUtcMs) + (millis() - s_phoneSyncTime);
    return static_cast<int>((now / 60000ULL) % 60);
  }
  return -1;
}

int getLocalHour() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  int uh = -1;
  int umi = -1;
  if (manualTimeFresh()) {
    uh = (int)s_manualHour;
    umi = (int)s_manualMinute;
  } else if (gpsTimeUsableForDisplay()) {
    if (s_gps.time.hour() == 0 && s_gps.time.minute() == 0 && s_gps.time.second() == 0) return -1;
    uh = s_gps.time.hour();
    umi = s_gps.time.minute();
  } else if (hasPhoneSync()) {
    return getHour();
  } else {
    return -1;
  }
  if (uh < 0 || umi < 0) return -1;
  int total = uh * 60 + umi + static_cast<int>(s_zdaLtzOffsetMinutes);
  total = ((total % 1440) + 1440) % 1440;
  return total / 60;
#else
  return getHour();
#endif
}

int getLocalMinute() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  int uh = -1;
  int umi = -1;
  if (manualTimeFresh()) {
    uh = (int)s_manualHour;
    umi = (int)s_manualMinute;
  } else if (gpsTimeUsableForDisplay()) {
    if (s_gps.time.hour() == 0 && s_gps.time.minute() == 0 && s_gps.time.second() == 0) return -1;
    uh = s_gps.time.hour();
    umi = s_gps.time.minute();
  } else if (hasPhoneSync()) {
    return getMinute();
  } else {
    return -1;
  }
  if (uh < 0 || umi < 0) return -1;
  int total = uh * 60 + umi + static_cast<int>(s_zdaLtzOffsetMinutes);
  total = ((total % 1440) + 1440) % 1440;
  return total % 60;
#else
  return getMinute();
#endif
}

}  // namespace gps
