/**
 * RiftLink GPS — NMEA, питание, пины
 */

#include "gps.h"
#if defined(ARDUINO_LILYGO_T_LORA_PAGER)
#include "board/lilygo_tpager.h"
#endif
#if defined(ARDUINO_LILYGO_T_BEAM)
#include "board/lilygo_tbeam.h"
#endif
#include <HardwareSerial.h>
#include <TinyGPSPlus.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <Arduino.h>
#include <cctype>
#include <cstring>
#include "log.h"

/** Сырой поток NMEA в Serial: в platformio.ini build_flags добавь -DRIFTLINK_GPS_NMEA_LOG=1 */
#ifndef RIFTLINK_GPS_NMEA_LOG
#define RIFTLINK_GPS_NMEA_LOG 0
#endif
#if RIFTLINK_GPS_NMEA_LOG
#ifndef RIFTLINK_GPS_NMEA_LOG_MAX
#define RIFTLINK_GPS_NMEA_LOG_MAX 200
#endif
static uint16_t s_gpsNmeaLogLines;
#endif

/** UART-драйверу нужен contiguous internal (DMA); после Wi‑Fi+BLE+async бывает <1KB — иначе abort в uartBegin */
static bool uartDriverHeapOk() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= 2048u &&
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= 2048u;
}

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_GPS_EN "gps_en"
#define NVS_KEY_GPS_RX "gps_rx"
#define NVS_KEY_GPS_TX "gps_tx"
#define NVS_KEY_GPS_PWR "gps_pwr"

// Heltec V3: Meshtastic-совместимые пины
#define DEFAULT_V3_EN 46
#define DEFAULT_V3_RX 48
#define DEFAULT_V3_TX 47

// Heltec V4 + L76K: Meshtastic begin(..., GPS_RX_PIN, GPS_TX_PIN) = (39, 38). Имена в variant — от линий к модулю;
// на части ревизий/внешнем L76K встречается обратная пара — см. probeGps() второй проход.
#define DEFAULT_V4_EN 34
#define DEFAULT_V4_RX 39
#define DEFAULT_V4_TX 38
#define DEFAULT_V4_GPS_STANDBY 40
#define DEFAULT_V4_GPS_RESET 42

// LilyGO T-Lora Pager (wiki): GNSS RX=12, TX=4; питание GNSS — XL9555, не GPIO
#define DEFAULT_TPAGER_RX 12
#define DEFAULT_TPAGER_TX 4

// LilyGO T-Beam V1.1/V1.2: NEO-6M GPS, UART RX=34 (ESP←GPS), TX=12 (ESP→GPS); питание через AXP2101
#define DEFAULT_TBEAM_RX 34
#define DEFAULT_TBEAM_TX 12

static int s_pinRx = -1;
static int s_pinTx = -1;
static int s_pinEn = -1;
static bool s_enabled = false;
static bool s_inited = false;

static HardwareSerial* s_serial = nullptr;
static TinyGPSPlus s_gps;
static int64_t s_phoneUtcMs = 0;
static float s_phoneLat = 0, s_phoneLon = 0;
static int16_t s_phoneAlt = 0;
static uint32_t s_phoneSyncTime = 0;

/**
 * TinyGPS++ кладёт время через parseDecimal→atol(hhmmss). На части libc atol("080000")
 * и похожие строки дают неверное значение → часы/минуты 0. Берём hhmmss из NMEA сами.
 *
 * ZDA (L76K и др.): отдельное предложение с UTC и датой — предпочитаем его как эталон времени.
 * Пока недавно приходят ZDA, не перезаписываем время из RMC/GGA (меньше расхождений между строками).
 */
static char s_nmeaLine[256];
static size_t s_nmeaLineLen;
static bool s_manualTimeValid;
static uint8_t s_manualHour;
static uint8_t s_manualMinute;
static uint32_t s_manualTimeMs;
static uint32_t s_lastZdaMs;
/** Смещение local−UTC в минутах из полей ltzh/ltzm последней ZDA; только RAM, не NVS. */
static int16_t s_zdaLtzOffsetMinutes;
static bool s_loggedZdaUtc;
static bool s_loggedRmcGgaUtc;

/** Сколько мс считать ручной UTC «свежим» (при 1 Гц NMEA и редком loop достаточно запаса) */
static constexpr uint32_t kManualGpsTimeFreshMs = 300000;
/** Если ZDA идут с ~1 Гц, RMC/GGA не трогаем часы это время (мс с последнего ZDA). */
static constexpr uint32_t kZdaSuppressRmcGgaMs = 4000;

/**
 * NMEA 0183: $ + talker(2) + formatter(3) + ',' = ровно 6 символов до первой запятой.
 * Примеры (поле 1 после запятой = UTC hhmmss.ss):
 *   $GPZDA,082710.00,03,07,2025,00,00*67
 *   $GNZDA,082710.00,03,07,2025,00,00*67
 */
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

/** Второе поле NMEA (после типа предложения) — hhmmss[.fff], допускаем пробелы в начале */
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

/** Поля 5–6 ZDA: ltzh, ltzm (NMEA: смещение от UTC к локальному гражданскому времени). */
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
    if (!s_loggedZdaUtc) {
      s_loggedZdaUtc = true;
      Serial.printf("[RiftLink] GPS UTC: ZDA (эталон), %02d:%02d, ltz %+d мин\n", h, mi, (int)s_zdaLtzOffsetMinutes);
    }
  } else if (!s_loggedRmcGgaUtc) {
    s_loggedRmcGgaUtc = true;
    Serial.printf("[RiftLink] GPS UTC: RMC/GGA (fallback), %02d:%02d\n", h, mi);
  }
}

static void feedNmeaLine(char c) {
  /* Только \n без \r — стандарт; только \r — тоже конец строки (иначе буфер не сбрасывается) */
  if (c == '\r' || c == '\n') {
    s_nmeaLine[s_nmeaLineLen] = '\0';
    if (s_nmeaLineLen > 0) {
#if RIFTLINK_GPS_NMEA_LOG
      if (s_gpsNmeaLogLines < RIFTLINK_GPS_NMEA_LOG_MAX && RIFTLINK_SERIAL_TX_HAS_SPACE()) {
        RIFTLINK_LOG_EVENT("[RiftLink] GPS RX: %s\n", s_nmeaLine);
        s_gpsNmeaLogLines++;
      }
#endif
      parseNmeaLineForTime(s_nmeaLine);
    }
    s_nmeaLineLen = 0;
    return;
  }
  if (s_nmeaLineLen < sizeof(s_nmeaLine) - 1) s_nmeaLine[s_nmeaLineLen++] = static_cast<char>(c);
}

static bool manualTimeFresh() {
  return s_manualTimeValid && (millis() - s_manualTimeMs) < kManualGpsTimeFreshMs;
}

static void applyPower(bool on) {
#if defined(ARDUINO_LILYGO_T_LORA_PAGER)
  lilygoTpagerSetGnssPower(on);
  s_enabled = on;
  return;
#elif defined(ARDUINO_LILYGO_T_BEAM)
  lilygoTbeamSetGpspower(on);
  s_enabled = on;
  return;
#elif defined(ARDUINO_heltec_wifi_lora_32_V4)
  if (s_pinEn < 0) return;
  /* L76K: RESET отпущен, STANDBY = wake (variant: low=sleep, high=wake), затем EN (active LOW) */
  pinMode(DEFAULT_V4_GPS_RESET, OUTPUT);
  digitalWrite(DEFAULT_V4_GPS_RESET, HIGH);
  pinMode(DEFAULT_V4_GPS_STANDBY, OUTPUT);
  digitalWrite(DEFAULT_V4_GPS_STANDBY, on ? HIGH : LOW);
  pinMode(s_pinEn, OUTPUT);
  digitalWrite(s_pinEn, on ? LOW : HIGH);
  s_enabled = on;
#else
  if (s_pinEn < 0) return;
  pinMode(s_pinEn, OUTPUT);
  digitalWrite(s_pinEn, on ? HIGH : LOW);
  s_enabled = on;
#endif
}

static bool loadConfig() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

  bool hasAny = false;
  int8_t rx = -1, tx = -1, en = -1;
  if (nvs_get_i8(h, NVS_KEY_GPS_RX, &rx) == ESP_OK) { s_pinRx = rx; hasAny = true; }
  if (nvs_get_i8(h, NVS_KEY_GPS_TX, &tx) == ESP_OK) { s_pinTx = tx; hasAny = true; }
  if (nvs_get_i8(h, NVS_KEY_GPS_EN, &en) == ESP_OK) { s_pinEn = en; hasAny = true; }

  uint8_t pwr = 0;
  if (nvs_get_u8(h, NVS_KEY_GPS_PWR, &pwr) == ESP_OK) {
    s_enabled = (pwr != 0);
  } else if (hasAny && s_pinRx >= 0 && s_pinTx >= 0) {
    /* Пины в NVS есть, ключа gps_pwr нет — иначе applyPower(false) и UART молчит */
    s_enabled = true;
  }

  nvs_close(h);
  return hasAny;
}

static void setDefaults() {
#if defined(ARDUINO_LILYGO_T_LORA_PAGER)
  s_pinRx = DEFAULT_TPAGER_RX;
  s_pinTx = DEFAULT_TPAGER_TX;
  s_pinEn = -1;
#elif defined(ARDUINO_LILYGO_T_BEAM)
  s_pinRx = DEFAULT_TBEAM_RX;
  s_pinTx = DEFAULT_TBEAM_TX;
  s_pinEn = -1;  // питание GPS через AXP2101, не GPIO
#elif defined(ARDUINO_heltec_wifi_lora_32_V4)
  s_pinRx = DEFAULT_V4_RX;
  s_pinTx = DEFAULT_V4_TX;
  s_pinEn = DEFAULT_V4_EN;
#else
  s_pinRx = DEFAULT_V3_RX;
  s_pinTx = DEFAULT_V3_TX;
  s_pinEn = DEFAULT_V3_EN;
#endif
}

static bool waitForUartActivity(HardwareSerial* serial, uint32_t probeMs) {
  uint32_t start = millis();
  while (millis() - start < probeMs) {
    if (serial->available() > 0) return true;
    delay(10);
    yield();
  }
  return false;
}

/** При буте — пробуем пины, слушаем NMEA. Не сохраняем в NVS. */
static bool probeGps() {
  if (!uartDriverHeapOk()) {
#if defined(ARDUINO_heltec_wifi_lora_32_V4)
    Serial.println("[RiftLink] GPS: UART пропущен — мало internal heap (как при loadConfig).");
#endif
    return false;
  }
  const uint32_t probeMs =
#if defined(ARDUINO_heltec_wifi_lora_32_V4)
      2500;
#else
      1500;
#endif

  applyPower(true);
#if defined(ARDUINO_heltec_wifi_lora_32_V4)
  delay(400);
#endif
  s_serial = new HardwareSerial(1);
  s_serial->begin(9600, SERIAL_8N1, s_pinRx, s_pinTx);
  bool gotData = waitForUartActivity(s_serial, probeMs);

#if defined(ARDUINO_heltec_wifi_lora_32_V4)
  /* Дефолт (39,38) как Meshtastic; если тишина — вторая попытка (38,39) для другой разводки/ревизии. */
  if (!gotData && s_pinRx == DEFAULT_V4_RX && s_pinTx == DEFAULT_V4_TX) {
    Serial.println("[RiftLink] GPS: первая попытка RX=39 TX=38 без данных — пробуем RX=38 TX=39");
    s_serial->end();
    delete s_serial;
    s_serial = nullptr;
    s_pinRx = DEFAULT_V4_TX;
    s_pinTx = DEFAULT_V4_RX;
    applyPower(true);
    delay(400);
    s_serial = new HardwareSerial(1);
    s_serial->begin(9600, SERIAL_8N1, s_pinRx, s_pinTx);
    gotData = waitForUartActivity(s_serial, probeMs);
    if (gotData) {
      Serial.println("[RiftLink] GPS: UART OK с RX=38 TX=39 (вторая разводка)");
    }
  }
#endif

  if (!gotData) {
#if defined(ARDUINO_heltec_wifi_lora_32_V4)
    Serial.println("[RiftLink] GPS: probe timeout — нет байт на UART (NVS gps_rx/tx, проводка L76K, питание EN=34)");
#endif
    s_serial->end();
    delete s_serial;
    s_serial = nullptr;
    applyPower(false);
    s_pinRx = s_pinTx = s_pinEn = -1;
    return false;
  }
  s_enabled = true;
  return true;
}

namespace gps {

void init() {
  if (s_inited) return;
#if RIFTLINK_GPS_NMEA_LOG
  s_gpsNmeaLogLines = 0;
#endif
  s_loggedZdaUtc = false;
  s_loggedRmcGgaUtc = false;
  s_lastZdaMs = 0;
  s_zdaLtzOffsetMinutes = 0;
  s_pinRx = s_pinTx = s_pinEn = -1;
  s_enabled = false;
  bool pinsFromNvs = false;

  if (loadConfig()) {
    pinsFromNvs = true;
    if (!uartDriverHeapOk()) {
      Serial.println("[RiftLink] GPS: UART пропущен — мало internal heap (после Wi‑Fi/BLE). Перезагрузка или отключите лишнее.");
      s_inited = true;
      return;
    }
    // Пользователь настроил через BLE (gps rx,tx,en)
    applyPower(s_enabled);
#if defined(ARDUINO_heltec_wifi_lora_32_V4)
    delay(80);
#endif
    s_serial = new HardwareSerial(1);
    s_serial->begin(9600, SERIAL_8N1, s_pinRx, s_pinTx);
#if defined(ARDUINO_heltec_wifi_lora_32_V4)
    /*
     * probeGps() с перестановкой RX/TX вызывается только без NVS. Если в NVS лежат
     * старые/чужие пины (43/44/45 и т.д.), UART молчит — пробуем swap здесь же.
     */
    if (s_enabled && s_pinRx >= 0 && s_pinTx >= 0 && s_serial) {
      if (!waitForUartActivity(s_serial, 2500)) {
        Serial.println("[RiftLink] GPS: с пинами из NVS нет данных — пробуем RX/TX наоборот");
        const int rx0 = s_pinRx, tx0 = s_pinTx;
        s_serial->end();
        delete s_serial;
        s_serial = nullptr;
        s_pinRx = tx0;
        s_pinTx = rx0;
        applyPower(s_enabled);
        delay(80);
        s_serial = new HardwareSerial(1);
        s_serial->begin(9600, SERIAL_8N1, s_pinRx, s_pinTx);
        if (waitForUartActivity(s_serial, 2500)) {
          Serial.println("[RiftLink] GPS: UART OK после перестановки — обновляю NVS");
          saveConfig();
        } else {
          s_serial->end();
          delete s_serial;
          s_serial = nullptr;
          s_pinRx = rx0;
          s_pinTx = tx0;
          applyPower(s_enabled);
          delay(80);
          s_serial = new HardwareSerial(1);
          s_serial->begin(9600, SERIAL_8N1, s_pinRx, s_pinTx);
          Serial.println("[RiftLink] GPS: перестановка не помогла — оставлены пины из NVS (сотри ключи или gps pins …)");
        }
      }
    }
#endif
  } else {
    setDefaults();
    if (!probeGps()) {
      s_inited = true;
      return;  // нет GPS — вкладку не рисуем
    }
  }
  Serial.printf("[RiftLink] GPS пины: rx=%d tx=%d en=%d, источник=%s, pwr=%s\n",
      s_pinRx, s_pinTx, s_pinEn, pinsFromNvs ? "NVS" : "дефолт(probe)",
      s_enabled ? "on" : "off");
  s_inited = true;
}

void update() {
  if (!s_serial || !s_enabled) return;

  while (s_serial->available() > 0) {
    char c = (char)s_serial->read();
    feedNmeaLine(c);
    if (s_gps.encode(c)) {
      // NMEA обработан
    }
  }
}

bool isPresent() {
  return s_pinRx >= 0 && s_pinTx >= 0;
}

bool isEnabled() {
  return s_enabled;
}

void setEnabled(bool on) {
  s_enabled = on;
  applyPower(on);
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, NVS_KEY_GPS_PWR, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
  }
}

void toggle() {
  setEnabled(!s_enabled);
}

bool hasFix() {
  if (!s_gps.location.isValid()) return false;
  /* isUpdated() в TinyGPS++ истинен только один проход после NMEA — на экране «фикс» дёргается.
   * Достаточно свежей валидной позиции по age(); 0xFFFFFFFF — нет данных (см. TinyGPSPlus). */
  const unsigned long age = s_gps.location.age();
  if (age == 0xFFFFFFFFUL) return false;
  return age < 30000UL;
}

float getLat() {
  return s_gps.location.lat();
}

float getLon() {
  return s_gps.location.lng();
}

int16_t getAlt() {
  return s_gps.altitude.isValid() ? (int16_t)s_gps.altitude.meters() : 0;
}

uint32_t getSatellites() {
  return s_gps.satellites.isValid() ? s_gps.satellites.value() : 0;
}

float getCourseDeg() {
  return s_gps.course.isValid() ? (float)s_gps.course.deg() : -1.0f;
}

const char* getCourseCardinal() {
  return s_gps.course.isValid() ? TinyGPSPlus::cardinal(s_gps.course.deg()) : "";
}

void setPins(int rx, int tx, int en) {
  bool wasEnabled = s_enabled;
  if (s_serial) {
    s_serial->end();
    delete s_serial;
    s_serial = nullptr;
  }
  applyPower(false);

  s_pinRx = rx;
  s_pinTx = tx;
  s_pinEn = en;

  if (rx >= 0 && tx >= 0) {
    s_serial = new HardwareSerial(1);
    s_serial->begin(9600, SERIAL_8N1, s_pinRx, s_pinTx);
    if (s_pinEn >= 0) applyPower(wasEnabled);
  }
}

void getPins(int* rx, int* tx, int* en) {
  if (rx) *rx = s_pinRx;
  if (tx) *tx = s_pinTx;
  if (en) *en = s_pinEn;
}

void saveConfig() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    Serial.printf("[RiftLink] GPS NVS save failed: %s\n", esp_err_to_name(err));
    return;
  }
  nvs_set_i8(h, NVS_KEY_GPS_RX, (int8_t)s_pinRx);
  nvs_set_i8(h, NVS_KEY_GPS_TX, (int8_t)s_pinTx);
  nvs_set_i8(h, NVS_KEY_GPS_EN, (int8_t)s_pinEn);
  nvs_set_u8(h, NVS_KEY_GPS_PWR, s_enabled ? 1 : 0);
  nvs_commit(h);
  nvs_close(h);
}

void setPhoneSync(int64_t utcMs, float lat, float lon, int16_t alt) {
  s_phoneUtcMs = utcMs;
  s_phoneLat = lat;
  s_phoneLon = lon;
  s_phoneAlt = alt;
  s_phoneSyncTime = (uint32_t)millis();
}

bool hasPhoneSync() {
  return s_phoneSyncTime != 0 && (millis() - s_phoneSyncTime) < 60000;
}

/**
 * TinyGPS++ коммитит время из GGA при любом checksum, даже fix=0: пустое поле времени
 * даёт newTime=0 → hour/minute=0 при valid=true (ложные «00:00»).
 * Реальное UTC 00:00 обычно сопровождается валидной позицией или датой из RMC.
 */
static bool gpsTimeUsableForDisplay() {
  if (!s_gps.time.isValid() || s_gps.time.age() >= 120000) return false;
  if (s_gps.location.isValid()) {
    /*
     * TinyGPS+atol часто даёт 00:00:00 при живом фиксе; поле date в RMC коммитится даже при пустом
     * времени — нельзя считать дату «доказательством» полуночи. Без ручного NMEA не показываем 00:00.
     */
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

bool hasTime() {
  if (manualTimeFresh()) return true;
  if (gpsTimeUsableForDisplay()) return true;
  return hasPhoneSync();
}

bool hasEpochTime() {
  return hasPhoneSync();
}

bool getEpochSec(uint32_t* outEpochSec) {
  if (!outEpochSec || !hasPhoneSync()) return false;
  uint64_t now = (uint64_t)s_phoneUtcMs + (millis() - s_phoneSyncTime);
  *outEpochSec = (uint32_t)(now / 1000ULL);
  return true;
}

int getHour() {
  if (manualTimeFresh()) return (int)s_manualHour;
  if (gpsTimeUsableForDisplay()) {
    const int h = s_gps.time.hour();
    if (h == 0 && s_gps.time.minute() == 0 && s_gps.time.second() == 0) return -1;
    return h;
  }
  if (hasPhoneSync()) {
    uint64_t now = (uint64_t)s_phoneUtcMs + (millis() - s_phoneSyncTime);
    return (int)((now / 3600000ULL) % 24);
  }
  return -1;
}

int getMinute() {
  if (manualTimeFresh()) return (int)s_manualMinute;
  if (gpsTimeUsableForDisplay()) {
    if (s_gps.time.hour() == 0 && s_gps.time.minute() == 0 && s_gps.time.second() == 0) return -1;
    return s_gps.time.minute();
  }
  if (hasPhoneSync()) {
    uint64_t now = (uint64_t)s_phoneUtcMs + (millis() - s_phoneSyncTime);
    return (int)((now / 60000ULL) % 60);
  }
  return -1;
}

int getLocalHour() {
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
}

int getLocalMinute() {
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
}

}  // namespace gps
