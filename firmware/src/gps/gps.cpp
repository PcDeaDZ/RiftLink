/**
 * RiftLink GPS — NMEA, питание, пины
 */

#include "gps.h"
#if defined(ARDUINO_LILYGO_T_LORA_PAGER)
#include "board/lilygo_tpager.h"
#endif
#include <HardwareSerial.h>
#include <TinyGPSPlus.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <Arduino.h>
#include <cstring>

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

// Heltec V4: 46 занят LoRa FEM
#define DEFAULT_V4_EN 45
#define DEFAULT_V4_RX 44
#define DEFAULT_V4_TX 43

// LilyGO T-Lora Pager (wiki): GNSS RX=12, TX=4; питание GNSS — XL9555, не GPIO
#define DEFAULT_TPAGER_RX 12
#define DEFAULT_TPAGER_TX 4

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

static void applyPower(bool on) {
#if defined(ARDUINO_LILYGO_T_LORA_PAGER)
  lilygoTpagerSetGnssPower(on);
  s_enabled = on;
  return;
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
  }

  nvs_close(h);
  return hasAny;
}

static void setDefaults() {
#if defined(ARDUINO_LILYGO_T_LORA_PAGER)
  s_pinRx = DEFAULT_TPAGER_RX;
  s_pinTx = DEFAULT_TPAGER_TX;
  s_pinEn = -1;
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

/** При буте — пробуем пины, слушаем NMEA ~1.5 с. Не сохраняем в NVS. */
static bool probeGps() {
  if (!uartDriverHeapOk()) {
    return false;
  }
  s_serial = new HardwareSerial(1);
  s_serial->begin(9600, SERIAL_8N1, s_pinRx, s_pinTx);
  applyPower(true);
  uint32_t start = millis();
  bool gotData = false;
  while (millis() - start < 1500) {
    if (s_serial->available() > 0) {
      gotData = true;
      break;
    }
    delay(10);
    yield();
  }
  if (!gotData) {
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
  s_pinRx = s_pinTx = s_pinEn = -1;
  s_enabled = false;

  if (loadConfig()) {
    if (!uartDriverHeapOk()) {
      Serial.println("[RiftLink] GPS: UART пропущен — мало internal heap (после Wi‑Fi/BLE). Перезагрузка или отключите лишнее.");
      s_inited = true;
      return;
    }
    // Пользователь настроил через BLE (gps rx,tx,en)
    s_serial = new HardwareSerial(1);
    s_serial->begin(9600, SERIAL_8N1, s_pinRx, s_pinTx);
    applyPower(s_enabled);
  } else {
    setDefaults();
    if (!probeGps()) {
      s_inited = true;
      return;  // нет GPS — вкладку не рисуем
    }
  }
  s_inited = true;
}

void update() {
  if (!s_serial || !s_enabled) return;

  while (s_serial->available() > 0) {
    if (s_gps.encode(s_serial->read())) {
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
  return s_gps.location.isValid() && s_gps.location.isUpdated();
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

bool hasTime() {
  if (s_gps.time.isValid() && s_gps.time.age() < 10000) return true;
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
  if (s_gps.time.isValid() && s_gps.time.age() < 10000) return s_gps.time.hour();
  if (hasPhoneSync()) {
    uint64_t now = (uint64_t)s_phoneUtcMs + (millis() - s_phoneSyncTime);
    return (int)((now / 3600000ULL) % 24);
  }
  return -1;
}

int getMinute() {
  if (s_gps.time.isValid() && s_gps.time.age() < 10000) return s_gps.time.minute();
  if (hasPhoneSync()) {
    uint64_t now = (uint64_t)s_phoneUtcMs + (millis() - s_phoneSyncTime);
    return (int)((now / 60000ULL) % 60);
  }
  return -1;
}

}  // namespace gps
