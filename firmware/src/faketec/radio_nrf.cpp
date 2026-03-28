/**
 * RiftLink Radio — nRF52840 + SX1262 (RadioLib), HT-RA62 / Heltec T114.
 */

#include "async_tasks.h"
#include "radio/radio.h"
#include "region/region.h"
#include "duty_cycle/duty_cycle.h"
#include "neighbors/neighbors.h"
#include "log.h"
#include "kv.h"
#include "board_pins.h"

#include <RadioLib.h>
#include <SPI.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "port/rtos_include.h"

#if defined(RIFTLINK_BOARD_HELTEC_T114)
#define LORA_IRQ_PIN LORA_DIO1
#else
#define LORA_IRQ_PIN LORA_DIO1
#endif

#define LORA_BW 125.0f
#define LORA_SF 7
#define LORA_CR 5
#define TCXO_VOLTAGE 1.8f

static const radio::ModemConfig MODEM_PRESETS[] = {
    {7, 250.0f, 5},
    {7, 125.0f, 5},
    {10, 125.0f, 5},
    {12, 125.0f, 8},
};

const radio::ModemConfig& radio::modemPresetConfig(radio::ModemPreset p) {
  if (p >= 0 && p < 4) return MODEM_PRESETS[p];
  return MODEM_PRESETS[radio::MODEM_NORMAL];
}

static const char* MODEM_PRESET_NAMES[] = {"Speed", "Normal", "Range", "MaxRange", "Custom"};
const char* radio::modemPresetName(radio::ModemPreset p) {
  if (p < radio::MODEM_PRESET_COUNT) return MODEM_PRESET_NAMES[p];
  return "?";
}

static inline void radioSendReason(char* buf, size_t buflen, const char* msg) {
  if (buf && buflen > 0) {
    snprintf(buf, buflen, "%s", msg ? msg : "?");
  }
}

static SemaphoreHandle_t s_radioMutex = nullptr;
static std::atomic<bool> s_rxListenActive{false};
static std::atomic<bool> s_arbiterHold{false};
static std::atomic<uint32_t> s_dio1IrqCount{0};
static std::atomic<uint32_t> s_rxLenOversizeDrops{0};
static std::atomic<uint32_t> s_rxShortReads{0};
static std::atomic<uint32_t> s_rxReadErrors{0};

static void onDio1Rise() {
  s_dio1IrqCount.fetch_add(1, std::memory_order_relaxed);
}

#define CAD_SLOT_TIME_MS 4
#define CAD_CW_MIN 8
#define CAD_MAX_RETRIES 5
#define CAD_BEB_MAX 5
#define BEB_DECAY_MS 8000

static std::atomic<uint8_t> s_cadBusyCount{0};
static std::atomic<uint32_t> s_lastCongestionTime{0};

static uint32_t adaptiveCwMax() {
  int n = neighbors::getCount();
  if (n <= 2) return 16;
  if (n <= 4) return 32;
  if (n <= 6) return 64;
  return 128;
}

static Module* s_mod = nullptr;
static SX1262* s_lora = nullptr;
static bool s_radioChipReady = false;

static inline bool chipOk() { return s_lora != nullptr && s_radioChipReady; }

static uint8_t s_meshSf = LORA_SF;
static uint8_t s_hwSf = LORA_SF;
static float s_bw = LORA_BW;
static uint8_t s_cr = LORA_CR;
static radio::ModemPreset s_preset = radio::MODEM_NORMAL;

static const char* KV_MODEM = "modem_preset";
static const char* KV_SF = "lora_sf";
static const char* KV_BW = "lora_bw";
static const char* KV_CR = "lora_cr";

static bool isValidBw(float bw) {
  const float valid[] = {62.5f, 125.0f, 250.0f, 500.0f};
  for (float v : valid) {
    if (fabsf(bw - v) < 0.1f) return true;
  }
  return false;
}

static void loadModemFromKv(uint8_t& sf, float& bw, uint8_t& cr, radio::ModemPreset& preset) {
  sf = LORA_SF;
  bw = LORA_BW;
  cr = LORA_CR;
  preset = radio::MODEM_NORMAL;

  uint8_t p = 0xFF;
  {
    size_t n = 1;
    uint8_t b = 0xFF;
    if (riftlink_kv::getBlob(KV_MODEM, &b, &n) && n == 1) p = b;
  }
  if (p < 4) {
    preset = (radio::ModemPreset)p;
    auto& cfg = MODEM_PRESETS[p];
    sf = cfg.sf;
    bw = cfg.bw;
    cr = cfg.cr;
  } else if (p == (uint8_t)radio::MODEM_CUSTOM) {
    preset = radio::MODEM_CUSTOM;
    uint8_t sfv = LORA_SF;
    {
      size_t n = 1;
      uint8_t b = LORA_SF;
      if (riftlink_kv::getBlob(KV_SF, &b, &n) && n == 1) sfv = b;
    }
    sf = (sfv >= 7 && sfv <= 12) ? sfv : LORA_SF;
    uint16_t bwx10 = 1250;
    {
      uint8_t raw[2];
      size_t n = 2;
      if (riftlink_kv::getBlob(KV_BW, raw, &n) && n == 2) {
        memcpy(&bwx10, raw, 2);
      }
    }
    bw = (float)bwx10 / 10.0f;
    if (!isValidBw(bw)) bw = LORA_BW;
    uint8_t crv = LORA_CR;
    {
      size_t n = 1;
      uint8_t b = LORA_CR;
      if (riftlink_kv::getBlob(KV_CR, &b, &n) && n == 1) crv = b;
    }
    cr = (crv >= 5 && crv <= 8) ? crv : LORA_CR;
  } else {
    uint8_t sfv = LORA_SF;
    size_t n = 1;
    uint8_t b = LORA_SF;
    if (riftlink_kv::getBlob(KV_SF, &b, &n) && n == 1) sfv = b;
    if (sfv >= 7 && sfv <= 12 && sfv != LORA_SF) {
      preset = radio::MODEM_CUSTOM;
      sf = sfv;
      bw = LORA_BW;
      cr = LORA_CR;
    }
  }
}

static void saveModemToKv(radio::ModemPreset preset, uint8_t sf, float bw, uint8_t cr) {
  uint8_t p = (uint8_t)preset;
  (void)riftlink_kv::setBlob(KV_MODEM, &p, 1);
  if (preset == radio::MODEM_CUSTOM) {
    (void)riftlink_kv::setBlob(KV_SF, &sf, 1);
    uint16_t bwx10 = (uint16_t)(bw * 10.0f + 0.5f);
    uint8_t bwbuf[2];
    memcpy(bwbuf, &bwx10, 2);
    (void)riftlink_kv::setBlob(KV_BW, bwbuf, 2);
    (void)riftlink_kv::setBlob(KV_CR, &cr, 1);
  }
}

static void applyModemToChip(uint8_t sf, float bw, uint8_t cr) {
  if (!chipOk()) return;
  uint16_t preamble = (sf >= 10) ? 16 : 8;
  s_lora->setSpreadingFactor(sf);
  s_lora->setBandwidth(bw);
  s_lora->setCodingRate(cr);
  s_lora->setPreambleLength(preamble);
  s_hwSf = sf;
}

namespace radio {

bool init() {
  s_radioChipReady = false;
  /* Один variant.h на FakeTech и Heltec T114: дефолтный SPI — пины V5. T114 — другая шина LoRa (board_pins.h). */
  SPI.setPins(LORA_MISO, LORA_SCK, LORA_MOSI);
  SPI.begin();

#if LORA_RXEN != 255
  pinMode(LORA_RXEN, OUTPUT);
  digitalWrite(LORA_RXEN, HIGH);
#endif

  s_mod = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
  s_lora = new SX1262(s_mod);

  float freq = region::getFreq();
  int power = region::getPower();

  uint8_t initSf;
  float initBw;
  uint8_t initCr;
  loadModemFromKv(initSf, initBw, initCr, s_preset);

  uint16_t preamble = (initSf >= 10) ? 16 : 8;
  int16_t st = s_lora->begin(freq, initBw, initSf, initCr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, power, preamble,
      TCXO_VOLTAGE, false);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("[RiftLink] Radio init failed: code=%d\n", (int)st);
    return false;
  }

#if defined(LORA_DIO2_RF_SWITCH) && LORA_DIO2_RF_SWITCH
  s_lora->setDio2AsRfSwitch(true);
#endif

  s_lora->setCRC(2);
  s_lora->setSyncWord(0x12);
  pinMode(LORA_IRQ_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(LORA_IRQ_PIN), onDio1Rise, RISING);

  s_meshSf = initSf;
  s_hwSf = initSf;
  s_bw = initBw;
  s_cr = initCr;
  Serial.printf("[RiftLink] Modem: %s SF%u BW%.0f CR4/%u\n", modemPresetName(s_preset), initSf, initBw, initCr);

  s_radioMutex = xSemaphoreCreateMutex();
  if (!s_radioMutex) return false;
  s_radioChipReady = true;
  return true;
}

bool takeMutex(TickType_t timeout) {
  if (!s_radioMutex) return true;
  if (s_arbiterHold.load(std::memory_order_relaxed)) return false;
  return xSemaphoreTake(s_radioMutex, timeout) == pdTRUE;
}

void releaseMutex() {
  if (s_radioMutex) xSemaphoreGive(s_radioMutex);
}

void setArbiterHold(bool on) {
  s_arbiterHold.store(on, std::memory_order_relaxed);
}

bool isArbiterHold() {
  return s_arbiterHold.load(std::memory_order_relaxed);
}

void setRxListenActive(bool on) {
  s_rxListenActive.store(on, std::memory_order_relaxed);
}

void standbyChipUnderMutex() {
  if (!chipOk()) return;
  s_lora->standby();
}

uint32_t getTimeOnAir(size_t len) {
  if (!chipOk()) return 0;
  return (uint32_t)s_lora->getTimeOnAir(len);
}

bool isChannelFree() {
  if (!chipOk()) return true;
  if (s_rxListenActive.load(std::memory_order_relaxed)) return false;
  if (!takeMutex(pdMS_TO_TICKS(50))) return false;
  s_lora->standby();
  int16_t cad = s_lora->scanChannel();
  releaseMutex();
  return (cad == RADIOLIB_CHANNEL_FREE);
}

void notifyCongestion() {
  uint8_t c = s_cadBusyCount.load(std::memory_order_relaxed);
  if (c < 255) s_cadBusyCount.store(c + 1, std::memory_order_relaxed);
  s_lastCongestionTime.store((uint32_t)millis(), std::memory_order_relaxed);
}

uint8_t getCongestionLevel() {
  return s_cadBusyCount.load(std::memory_order_relaxed);
}

void setAsyncMode(bool) {
  /* nRF: синхронный TX из queueTxPacket/async stubs — очередь ESP не используется. */
}

bool sendDirectInternal(const uint8_t* data, size_t len, char* reasonBuf, size_t reasonLen, bool skipCad) {
  s_dio1IrqCount.store(0, std::memory_order_relaxed);
  const size_t kMaxRadioPayload = RADIOLIB_SX126X_MAX_PACKET_LENGTH;
  if (!chipOk() || len > kMaxRadioPayload) {
    const char* cause = !s_radioChipReady ? "radio_init_failed" : "pkt_too_long";
    radioSendReason(reasonBuf, reasonLen, cause);
    RIFTLINK_DIAG("RADIO", "event=TX_RESULT ok=0 cause=%s len=%u", cause, (unsigned)len);
    return false;
  }

  uint32_t now = (uint32_t)millis();
  uint8_t c = s_cadBusyCount.load(std::memory_order_relaxed);
  if (c > 0 && (now - s_lastCongestionTime.load(std::memory_order_relaxed)) >= BEB_DECAY_MS) {
    s_cadBusyCount.store(c - 1, std::memory_order_relaxed);
    s_lastCongestionTime.store(now, std::memory_order_relaxed);
  }

  uint32_t toa = getTimeOnAir(len);
  if (!duty_cycle::canSend(toa)) {
    Serial.println("[RiftLink] Duty cycle limit (EU 1%) — TX skipped");
    radioSendReason(reasonBuf, reasonLen, "duty_cycle");
    return false;
  }

  s_lora->standby();
  if (!skipCad) {
    for (int attempt = 0; attempt < CAD_MAX_RETRIES; attempt++) {
      int16_t cad = s_lora->scanChannel();
      if (cad == RADIOLIB_CHANNEL_FREE) break;
      RIFTLINK_DIAG("RADIO", "event=CAD_BUSY attempt=%d sf=%u len=%u", attempt + 1, (unsigned)getSpreadingFactor(),
          (unsigned)len);
      if (attempt < CAD_MAX_RETRIES - 1) {
        uint8_t cc = s_cadBusyCount.load(std::memory_order_relaxed);
        uint32_t cw = CAD_CW_MIN * (1u << (cc < CAD_BEB_MAX ? cc : CAD_BEB_MAX));
        uint32_t cwMax = adaptiveCwMax();
        if (cw > cwMax) cw = cwMax;
        if (cc < 255) {
          s_cadBusyCount.store(cc + 1, std::memory_order_relaxed);
          s_lastCongestionTime.store((uint32_t)millis(), std::memory_order_relaxed);
        }
        uint32_t backoff = (uint32_t)((uint32_t)random() % cw) * CAD_SLOT_TIME_MS;
        if (backoff > 0) {
          queueDeferredSend(data, len, getSpreadingFactor(), backoff);
          radioSendReason(reasonBuf, reasonLen, "cad_defer");
          RIFTLINK_DIAG("RADIO", "event=CAD_DEFER backoff_ms=%lu cw=%lu sf=%u len=%u", (unsigned long)backoff,
              (unsigned long)cw, (unsigned)getSpreadingFactor(), (unsigned)len);
          return false;
        }
      }
    }
  }

  int16_t st = s_lora->transmit(const_cast<uint8_t*>(data), len);
  if (st != RADIOLIB_ERR_NONE) {
    if (st == -705) {
      s_lora->standby();
      delay(5);
      st = s_lora->transmit(const_cast<uint8_t*>(data), len);
    }
    if (st != RADIOLIB_ERR_NONE) {
      Serial.printf("[RiftLink] TX failed: %d\n", (int)st);
      if (reasonBuf && reasonLen > 0) snprintf(reasonBuf, reasonLen, "tx_err_%d", (int)st);
      return false;
    }
  }
  duty_cycle::recordSend(toa);
  s_cadBusyCount.store(0, std::memory_order_relaxed);
  return true;
}

bool send(const uint8_t* data, size_t len, uint8_t txSf, bool priority, char* reasonBuf, size_t reasonLen) {
  (void)txSf;
  (void)priority;
  if (reasonBuf && reasonLen > 0) reasonBuf[0] = '\0';
  if (!takeMutex(pdMS_TO_TICKS(2000))) {
    radioSendReason(reasonBuf, reasonLen, "mutex");
    return false;
  }
  bool ok = sendDirectInternal(data, len, reasonBuf, reasonLen, false);
  releaseMutex();
  return ok;
}

bool sendDirect(const uint8_t* data, size_t len, char* reasonBuf, size_t reasonLen) {
  return send(data, len, 0, true, reasonBuf, reasonLen);
}

int receive(uint8_t* buf, size_t maxLen) {
  if (!chipOk()) return -1;
  int16_t st = s_lora->receive(buf, maxLen);
  if (st == RADIOLIB_ERR_RX_TIMEOUT) return 0;
  if (st < 0) return -1;
  return (int)st;
}

bool startReceiveWithTimeout(uint32_t timeoutMs) {
  if (!chipOk()) return false;
  uint32_t units = (uint32_t)((uint64_t)timeoutMs * 1000 / 16);
  if (units > 0xFFFFF) units = 0xFFFFF;
  int16_t st = s_lora->startReceive(units);
  return (st == RADIOLIB_ERR_NONE);
}

static int normalizeReadLength(size_t requestedLen, int16_t readStatus) {
  if (readStatus < 0) return -1;
  if (readStatus == RADIOLIB_ERR_NONE) return (int)requestedLen;
  int normalized = (int)readStatus;
  if (normalized <= 0) return -1;
  if ((size_t)normalized > requestedLen) normalized = (int)requestedLen;
  return normalized;
}

int receiveAsync(uint8_t* buf, size_t maxLen) {
  if (!chipOk() || !buf || maxLen == 0) return -1;
  size_t len = s_lora->getPacketLength();
  if (len == 0 || len > maxLen) {
    if (len > maxLen) {
      s_rxLenOversizeDrops.fetch_add(1, std::memory_order_relaxed);
    }
    s_lora->standby();
    return 0;
  }
  int16_t st = s_lora->readData(buf, len);
  int readLen = normalizeReadLength(len, st);
  if (st == RADIOLIB_ERR_RX_TIMEOUT) return 0;
  if (readLen < 0) {
    s_rxReadErrors.fetch_add(1, std::memory_order_relaxed);
    return -1;
  }
  if ((size_t)readLen < len) {
    s_rxShortReads.fetch_add(1, std::memory_order_relaxed);
  }
  s_lora->standby();
  return readLen;
}

bool isRxPacketReadyUnderMutex() {
  if (!chipOk()) return false;
  return s_lora->getPacketLength() > 0;
}

int readReceivedPacketUnderMutex(uint8_t* buf, size_t maxLen) {
  if (!chipOk() || !buf || maxLen == 0) return -1;
  size_t len = s_lora->getPacketLength();
  if (len == 0) return 0;
  if (len > maxLen) {
    s_rxLenOversizeDrops.fetch_add(1, std::memory_order_relaxed);
    s_lora->standby();
    return -1;
  }
  int16_t st = s_lora->readData(buf, len);
  int readLen = normalizeReadLength(len, st);
  if (st == RADIOLIB_ERR_RX_TIMEOUT) return 0;
  if (readLen < 0) {
    s_rxReadErrors.fetch_add(1, std::memory_order_relaxed);
    return -1;
  }
  return readLen;
}

void getRxDiagCounters(uint32_t* oversizeDrops, uint32_t* shortReads, uint32_t* readErrors) {
  if (oversizeDrops) *oversizeDrops = s_rxLenOversizeDrops.load(std::memory_order_relaxed);
  if (shortReads) *shortReads = s_rxShortReads.load(std::memory_order_relaxed);
  if (readErrors) *readErrors = s_rxReadErrors.load(std::memory_order_relaxed);
}

bool consumeIrqEvent() {
  uint32_t cnt = s_dio1IrqCount.exchange(0, std::memory_order_relaxed);
  return cnt > 0;
}

int getLastRssi() {
  if (!chipOk()) return 0;
  float rssi = s_lora->getRSSI(true);
  if (rssi >= -150 && rssi <= 0) return (int)rssi;
  return 0;
}

void applyRegion(float freq, int power) {
  if (!chipOk()) return;
  s_lora->setFrequency(freq);
  s_lora->setOutputPower(power);
}

void requestApplyRegion(float freq, int power) {
  if (!takeMutex(pdMS_TO_TICKS(200))) return;
  applyRegion(freq, power);
  releaseMutex();
}

void setModemPreset(ModemPreset p) {
  if (p >= MODEM_PRESET_COUNT) return;
  if (p < 4) {
    auto& cfg = MODEM_PRESETS[p];
    s_meshSf = cfg.sf;
    s_bw = cfg.bw;
    s_cr = cfg.cr;
    applyModemToChip(cfg.sf, cfg.bw, cfg.cr);
  }
  s_preset = p;
  saveModemToKv(p, s_meshSf, s_bw, s_cr);
  Serial.printf("[RiftLink] Modem preset: %s SF%u BW%.0f CR4/%u\n", modemPresetName(p), s_meshSf, s_bw, s_cr);
}

void setCustomModem(uint8_t sf, float bw, uint8_t cr) {
  if (sf < 7 || sf > 12 || !isValidBw(bw) || cr < 5 || cr > 8) return;
  s_meshSf = sf;
  s_bw = bw;
  s_cr = cr;
  s_preset = MODEM_CUSTOM;
  applyModemToChip(sf, bw, cr);
  saveModemToKv(MODEM_CUSTOM, sf, bw, cr);
  Serial.printf("[RiftLink] Modem custom: SF%u BW%.0f CR4/%u\n", sf, bw, cr);
}

bool requestModemPreset(ModemPreset p) {
  if (p >= MODEM_PRESET_COUNT) return false;
  if (!takeMutex(pdMS_TO_TICKS(200))) return false;
  setModemPreset(p);
  releaseMutex();
  return true;
}

bool requestCustomModem(uint8_t sf, float bw, uint8_t cr) {
  if (sf < 7 || sf > 12 || cr < 5 || cr > 8) return false;
  if (!isValidBw(bw)) return false;
  if (!takeMutex(pdMS_TO_TICKS(200))) return false;
  setCustomModem(sf, bw, cr);
  releaseMutex();
  return true;
}

ModemPreset getModemPreset() {
  return s_preset;
}
uint8_t getSpreadingFactor() {
  return s_meshSf;
}
float getBandwidth() {
  return s_bw;
}
uint8_t getCodingRate() {
  return s_cr;
}

void setSpreadingFactor(uint8_t sf) {
  if (!chipOk() || sf < 7 || sf > 12) return;
  setCustomModem(sf, s_bw, s_cr);
}

void applyHardwareSpreadingFactor(uint8_t sf) {
  if (!chipOk() || sf < 7 || sf > 12) return;
  if (sf == s_hwSf) return;
  uint16_t preamble = (sf >= 10) ? 16 : 8;
  s_lora->setSpreadingFactor(sf);
  s_lora->setPreambleLength(preamble);
  s_hwSf = sf;
}

void applyHardwareModem(uint8_t sf, float bw, uint8_t cr) {
  if (!chipOk()) return;
  if (sf < 7 || sf > 12) return;
  if (!isValidBw(bw) || cr < 5 || cr > 8) return;
  uint16_t preamble = (sf >= 10) ? 16 : 8;
  s_lora->setSpreadingFactor(sf);
  s_lora->setBandwidth(bw);
  s_lora->setCodingRate(cr);
  s_lora->setPreambleLength(preamble);
  s_hwSf = sf;
}

bool requestSpreadingFactor(uint8_t sf) {
  if (sf < 7 || sf > 12) return false;
  return requestCustomModem(sf, s_bw, s_cr);
}

bool isReady() {
  return s_radioChipReady;
}

}  // namespace radio
