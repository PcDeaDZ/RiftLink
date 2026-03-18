/**
 * RiftLink Radio Layer — LoRa SX1262 (RadioLib)
 * Heltec V3/V4: NSS=8, RST=12, DIO1=14, BUSY=13, SPI: SCK=9, MISO=11, MOSI=10
 * Heltec V4: FEM (GC1109) — GPIO7 питание, GPIO2 enable, GPIO46 PA mode
 */

#include "radio.h"
#include "region/region.h"
#include "duty_cycle/duty_cycle.h"
#include "async_queues.h"
#include "async_tasks.h"
#include <RadioLib.h>
#include <SPI.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static bool s_asyncMode = false;
static SemaphoreHandle_t s_radioMutex = nullptr;

// Heltec WiFi LoRa 32 V3/V4 pins (Meshtastic variant.h)
#define LORA_NSS   8
#define LORA_RST   12
#define LORA_DIO1  14
#define LORA_BUSY  13
#define LORA_SCK   9
#define LORA_MISO  11
#define LORA_MOSI  10

#ifdef ARDUINO_heltec_wifi_lora_32_V4
  #define LORA_PA_POWER  7   // VFEM_Ctrl — питание FEM
  #define LORA_PA_EN     2   // CSD — chip enable (HIGH=on)
  #define LORA_PA_TX_EN  46  // CPS — GC1109 PA mode (LOW=bypass/RX)
#endif

#define LORA_BW    125.0
#define LORA_SF    7
#define LORA_CR    5
#define TCXO_VOLTAGE 1.8f   // SX1262 TCXO 1.8V (V3/V4)

// CSMA/CA (как Meshtastic): CAD перед TX, random backoff при занятом канале
#define CAD_SLOT_TIME_MS  4
#define CAD_CW_MAX        8
#define CAD_MAX_RETRIES   5

static Module* mod = nullptr;
static SX1262* lora = nullptr;
static uint8_t s_currentSf = LORA_SF;

namespace radio {

bool init() {
  // SPI: ESP32 default (18,19,23) != Heltec (9,11,10). Явно задаём пины.
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);

#ifdef ARDUINO_heltec_wifi_lora_32_V4
  // Heltec V4: включить FEM (GC1109) до инициализации радио
  pinMode(LORA_PA_POWER, OUTPUT);
  digitalWrite(LORA_PA_POWER, HIGH);
  pinMode(LORA_PA_EN, OUTPUT);
  digitalWrite(LORA_PA_EN, HIGH);
  pinMode(LORA_PA_TX_EN, OUTPUT);
  digitalWrite(LORA_PA_TX_EN, LOW);   // RX mode (bypass)
  delay(100);  // FEM warmup
#endif

  mod = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
  lora = new SX1262(mod);

  float freq = region::getFreq();
  int power = region::getPower();

  // Preamble 16 (как Meshtastic) — больше времени на синхронизацию RX, меньше потерь при «просыпании»
  int16_t st = lora->begin(freq, LORA_BW, LORA_SF, LORA_CR, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, power, 16, TCXO_VOLTAGE, false);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("[RiftLink] Radio init failed: code=%d (проверьте антенну и питание)\n", st);
    return false;
  }

#ifdef ARDUINO_heltec_wifi_lora_32_V4
  lora->setDio2AsRfSwitch(true);  // DIO2 управляет RF switch FEM
#endif

  lora->setCRC(2);  // CRC 2 bytes
  lora->setSyncWord(0x12);  // Private network
  s_radioMutex = xSemaphoreCreateMutex();
  if (!s_radioMutex) return false;
  return true;
}

bool takeMutex(TickType_t timeout) {
  if (!s_radioMutex) return true;
  return xSemaphoreTake(s_radioMutex, timeout) == pdTRUE;
}

void releaseMutex() {
  if (s_radioMutex) xSemaphoreGive(s_radioMutex);
}

uint32_t getTimeOnAir(size_t len) {
  if (!lora) return 0;
  return (uint32_t)lora->getTimeOnAir(len);
}

void setAsyncMode(bool on) { s_asyncMode = on; }

bool sendDirectInternal(const uint8_t* data, size_t len) {
  if (!lora || len > RADIOLIB_SX126X_MAX_PACKET_LENGTH) return false;

  uint32_t toa = getTimeOnAir(len);
  if (!duty_cycle::canSend(toa)) {
    Serial.println("[RiftLink] Duty cycle limit (EU 1%) — TX skipped, попробуйте позже");
    return false;
  }

  // CSMA/CA: CAD перед TX, random backoff при занятом канале (как Meshtastic)
  lora->standby();
  for (int attempt = 0; attempt < CAD_MAX_RETRIES; attempt++) {
    int16_t cad = lora->scanChannel();
    if (cad == RADIOLIB_CHANNEL_FREE) break;
    if (attempt < CAD_MAX_RETRIES - 1) {
      uint32_t backoff = (esp_random() % CAD_CW_MAX) * CAD_SLOT_TIME_MS;
      if (backoff > 0) {
        delay(backoff);
        yield();
      }
    }
  }

  int16_t st = lora->transmit(const_cast<uint8_t*>(data), len);
  if (st != RADIOLIB_ERR_NONE) {
    // -705 = SPI_CMD_TIMEOUT — радио могло зависнуть после RX, пробуем standby и повторить
    if (st == -705) {
      lora->standby();
      delay(5);
      st = lora->transmit(const_cast<uint8_t*>(data), len);
    }
    if (st != RADIOLIB_ERR_NONE) {
      Serial.printf("[RiftLink] TX failed: %d\n", st);
      return false;
    }
  }
  duty_cycle::recordSend(toa);
  return true;
}

bool sendDirect(const uint8_t* data, size_t len) {
  if (!takeMutex(pdMS_TO_TICKS(500))) return false;
  bool ok = sendDirectInternal(data, len);
  releaseMutex();
  return ok;
}

bool send(const uint8_t* data, size_t len, uint8_t txSf, bool priority) {
  if (s_asyncMode && sendQueue) {
    return queueSend(data, len, txSf, priority);
  }
  if (txSf >= 7 && txSf <= 12) {
    uint8_t prev = getSpreadingFactor();
    setSpreadingFactor(txSf);
    bool ok = sendDirect(data, len);
    setSpreadingFactor(prev);
    return ok;
  }
  return sendDirect(data, len);
}

int receive(uint8_t* buf, size_t maxLen) {
  if (!lora) return -1;

  int16_t st = lora->receive(buf, maxLen);
  if (st == RADIOLIB_ERR_RX_TIMEOUT) return 0;
  if (st < 0) return -1;
  return (int)st;
}

// Timeout в SX126x: единицы 15.625 us. 1 sec = 64000.
bool startReceiveWithTimeout(uint32_t timeoutMs) {
  if (!lora) return false;
  uint32_t units = (uint32_t)((uint64_t)timeoutMs * 1000 / 16);  // 15.625 us
  if (units > 0xFFFFF) units = 0xFFFFF;
  int16_t st = lora->startReceive(units);
  return (st == RADIOLIB_ERR_NONE);
}

int receiveAsync(uint8_t* buf, size_t maxLen) {
  if (!lora || !buf || maxLen == 0) return -1;
  size_t len = lora->getPacketLength();
  if (len == 0 || len > maxLen) {
    // Таймаут RX — перевести в standby, иначе следующий TX (selftest и т.д.) падает при BLE.
    lora->standby();
    return 0;
  }
  int16_t st = lora->readData(buf, len);
  if (st == RADIOLIB_ERR_RX_TIMEOUT) return 0;
  if (st < 0) return -1;
  lora->standby();  // гарантировать standby после RX — иначе следующий TX может дать -705
  return (int)len;
}

int getLastRssi() {
  if (!lora) return 0;
  float rssi = lora->getRSSI(true);
  if (rssi >= -150 && rssi <= 0) return (int)rssi;
  return 0;
}

void applyRegion(float freq, int power) {
  if (!lora) return;
  lora->setFrequency(freq);
  lora->setOutputPower(power);
}

void setSpreadingFactor(uint8_t sf) {
  if (!lora || sf < 5 || sf > 12) return;
  lora->setSpreadingFactor(sf);
  s_currentSf = sf;
}

uint8_t getSpreadingFactor() {
  return s_currentSf;
}

}  // namespace radio
