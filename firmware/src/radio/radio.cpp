/**
 * RiftLink Radio Layer — LoRa SX1262 (RadioLib)
 * Heltec V3/V4: NSS=8, RST=12, DIO1=14, BUSY=13, SPI: SCK=9, MISO=11, MOSI=10
 * Heltec V4: FEM (GC1109) — GPIO7 питание, GPIO2 enable, GPIO46 PA mode
 * LilyGO T-Lora Pager: NSS=36, RST=47, DIO1=14, BUSY=48, SPI: SCK=35, MISO=33, MOSI=34 (общий с TFT)
 * LilyGO T-Beam (SX1262): как Arduino variant tbeam + Meshtastic variants/esp32/tbeam/variant.h
 *   SPI SCK=5 MISO=19 MOSI=27 CS=18 RST=23 DIO1=33 BUSY=32; I2C 21/22
 * LilyGO T-Beam (SX1276/SX1278 RFM9x, при RIFTLINK_T_BEAM_LORA_SX127X): те же SPI/CS/RST, DIO0=26 DIO1=33, без BUSY
 */

#include "radio.h"
#include "region/region.h"
#include "duty_cycle/duty_cycle.h"
#include "neighbors/neighbors.h"
#include "log.h"
#include "async_queues.h"
#include "async_tasks.h"
#include <RadioLib.h>
#include <SPI.h>
#include <esp_random.h>
#include <nvs.h>
#include <atomic>
#include <cmath>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>

static bool s_asyncMode = false;

static inline void radioSendReason(char* buf, size_t buflen, const char* msg) {
  if (buf && buflen > 0) {
    snprintf(buf, buflen, "%s", msg ? msg : "?");
  }
}
static SemaphoreHandle_t s_radioMutex = nullptr;
/** Планировщик/loop в окне RX (чип слушает эфир) — CAD не должен делать standby поверх RX. */
static std::atomic<bool> s_rxListenActive{false};
static std::atomic<bool> s_arbiterHold{false};
static std::atomic<uint32_t> s_dio1IrqCount{0};
static std::atomic<uint32_t> s_rxLenOversizeDrops{0};
static std::atomic<uint32_t> s_rxShortReads{0};
static std::atomic<uint32_t> s_rxReadErrors{0};

static void IRAM_ATTR onDio1Rise() {
  s_dio1IrqCount.fetch_add(1, std::memory_order_relaxed);
}

// LoRa SX1262 pins per board variant
#if defined(ARDUINO_LILYGO_T_LORA_PAGER)
#define LORA_NSS   36
#define LORA_RST   47
#define LORA_DIO1  14
#define LORA_BUSY  48
#define LORA_SCK   35
#define LORA_MISO  33
#define LORA_MOSI  34
#elif defined(ARDUINO_LILYGO_T_BEAM) && defined(RIFTLINK_T_BEAM_LORA_SX127X)
#define LORA_NSS   18
#define LORA_RST   23
#define LORA_DIO0  26
#define LORA_DIO1  33
#define LORA_SCK   5
#define LORA_MISO  19
#define LORA_MOSI  27
#elif defined(ARDUINO_LILYGO_T_BEAM)
#define LORA_NSS   18
#define LORA_RST   23
#define LORA_DIO1  33
#define LORA_DIO0  26
#define LORA_BUSY  32
#define LORA_SCK   5
#define LORA_MISO  19
#define LORA_MOSI  27
#else
// Heltec WiFi LoRa 32 V3/V4 (Meshtastic variant.h)
#define LORA_NSS   8
#define LORA_RST   12
#define LORA_DIO1  14
#define LORA_BUSY  13
#define LORA_SCK   9
#define LORA_MISO  11
#define LORA_MOSI  10
#endif

#ifdef ARDUINO_heltec_wifi_lora_32_V4
  #define LORA_PA_POWER  7   // VFEM_Ctrl — питание FEM
  #define LORA_PA_EN     2   // CSD — chip enable (HIGH=on)
  #define LORA_PA_TX_EN  46  // CPS — GC1109 PA mode (LOW=bypass/RX)
#endif

#if defined(ARDUINO_LILYGO_T_BEAM) && defined(RIFTLINK_T_BEAM_LORA_SX127X)
#define LORA_IRQ_PIN LORA_DIO0
#elif !defined(ARDUINO_LILYGO_T_BEAM)
#define LORA_IRQ_PIN LORA_DIO1
#endif

#if defined(ARDUINO_LILYGO_T_BEAM) && !defined(RIFTLINK_T_BEAM_LORA_SX127X)
#define RIFTLINK_T_BEAM_LORA_AUTO 1
#endif
// RFM95 / T-Beam 868 MHz: SX1276 в RadioLib — диапазон 862–1020 МГц; SX1278-класс без него — EU868 даёт -12.
#if defined(ARDUINO_LILYGO_T_BEAM) && defined(RIFTLINK_T_BEAM_LORA_SX127X)
typedef SX1276 LoRaChip;
#elif !defined(RIFTLINK_T_BEAM_LORA_AUTO)
typedef SX1262 LoRaChip;
#endif

#define LORA_BW    125.0f
#define LORA_SF    7
#define LORA_CR    5
#if !defined(ARDUINO_LILYGO_T_BEAM)
#define TCXO_VOLTAGE 1.8f  // SX1262 TCXO 1.8V (Heltec V3/V4, LilyGO T-Pager); T-Beam — см. kTcxo[] в init()
#endif

static const radio::ModemConfig MODEM_PRESETS[] = {
  /* SPEED     */ { 7,  250.0f, 5},
  /* NORMAL    */ { 7,  125.0f, 5},
  /* RANGE     */ {10,  125.0f, 5},
  /* MAX_RANGE */ {12,  125.0f, 8},
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

// CSMA/CA: CAD перед TX, Binary Exponential Backoff (BEB) при занятом канале
// См. docs/plans/CHANNEL_ACCESS_ANALYSIS.md
#define CAD_SLOT_TIME_MS  4
#define CAD_CW_MIN        8
#define CAD_MAX_RETRIES   5
#define CAD_BEB_MAX       5
#define BEB_DECAY_MS      8000

static uint32_t adaptiveCwMax() {
  int n = neighbors::getCount();
  if (n <= 2) return 16;
  if (n <= 4) return 32;
  if (n <= 6) return 64;
  return 128;
}

static Module* mod = nullptr;
static std::atomic<uint8_t> s_cadBusyCount{0};  // BEB: растёт при busy/NACK/undelivered, сброс при успешной TX
static std::atomic<uint32_t> s_lastCongestionTime{0};  // для decay BEB
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
static SX1262* lora1262 = nullptr;
static SX1276* lora1276 = nullptr;
static bool s_tbeamIsSx127x = false;
#else
static LoRaChip* lora = nullptr;
#endif
/** true только после успешного begin(); иначе объект RadioLib есть, но чип не отвечает — не вызывать TX/SPI. */
static bool s_radioChipReady = false;

namespace {
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
inline bool chipOk() {
  return (s_tbeamIsSx127x ? lora1276 != nullptr : lora1262 != nullptr) && s_radioChipReady;
}
#else
inline bool chipOk() { return lora != nullptr && s_radioChipReady; }
#endif
}  // namespace

#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
namespace {
void rlStandby() {
  if (s_tbeamIsSx127x) {
    lora1276->standby();
  } else {
    lora1262->standby();
  }
}
uint32_t rlGetTimeOnAir(size_t len) {
  return s_tbeamIsSx127x ? lora1276->getTimeOnAir(len) : lora1262->getTimeOnAir(len);
}
int16_t rlScanChannel() {
  return s_tbeamIsSx127x ? lora1276->scanChannel() : lora1262->scanChannel();
}
int16_t rlTransmit(const uint8_t* data, size_t len) {
  uint8_t* p = const_cast<uint8_t*>(data);
  return s_tbeamIsSx127x ? lora1276->transmit(p, len) : lora1262->transmit(p, len);
}
int16_t rlReceive(uint8_t* buf, size_t maxLen) {
  return s_tbeamIsSx127x ? lora1276->receive(buf, maxLen) : lora1262->receive(buf, maxLen);
}
size_t rlGetPacketLength() {
  return s_tbeamIsSx127x ? lora1276->getPacketLength() : lora1262->getPacketLength();
}
int16_t rlReadData(uint8_t* buf, size_t len) {
  return s_tbeamIsSx127x ? lora1276->readData(buf, len) : lora1262->readData(buf, len);
}
float rlGetRssiLastPkt() {
  return s_tbeamIsSx127x ? lora1276->getRSSI(true) : lora1262->getRSSI(true);
}
void rlSetFreqPower(float freq, int power) {
  if (s_tbeamIsSx127x) {
    lora1276->setFrequency(freq);
    lora1276->setOutputPower((int8_t)power);
  } else {
    lora1262->setFrequency(freq);
    lora1262->setOutputPower(power);
  }
}
void rlApplyModem(uint8_t sf, float bw, uint8_t cr, uint16_t preamble) {
  if (s_tbeamIsSx127x) {
    lora1276->setSpreadingFactor(sf);
    lora1276->setBandwidth(bw);
    lora1276->setCodingRate(cr);
    lora1276->setPreambleLength(preamble);
  } else {
    lora1262->setSpreadingFactor(sf);
    lora1262->setBandwidth(bw);
    lora1262->setCodingRate(cr);
    lora1262->setPreambleLength(preamble);
  }
}
void rlApplySfPreamble(uint8_t sf, uint16_t preamble) {
  if (s_tbeamIsSx127x) {
    lora1276->setSpreadingFactor(sf);
    lora1276->setPreambleLength(preamble);
  } else {
    lora1262->setSpreadingFactor(sf);
    lora1262->setPreambleLength(preamble);
  }
}
}  // namespace
#endif
static uint8_t s_meshSf = LORA_SF;
static uint8_t s_hwSf = LORA_SF;
static float   s_bw = LORA_BW;
static uint8_t s_cr = LORA_CR;
static radio::ModemPreset s_preset = radio::MODEM_NORMAL;

static const char* NVS_NAMESPACE = "riftlink";
static const char* NVS_KEY_SF = "lora_sf";
static const char* NVS_KEY_BW = "lora_bw";
static const char* NVS_KEY_CR = "lora_cr";
static const char* NVS_KEY_MODEM = "modem_preset";
/** T-Beam авто LoRa: 0/отсутствует — полный probe; 1 — последний раз был SX1262; 2 — RFM9x (быстрый старт без SX1262). */
static const char* NVS_KEY_TBEAM_RF = "tbeam_rf";

static bool isValidBw(float bw) {
  const float valid[] = {62.5f, 125.0f, 250.0f, 500.0f};
  for (auto v : valid) if (fabsf(bw - v) < 0.1f) return true;
  return false;
}

static void loadModemFromNvs(uint8_t& sf, float& bw, uint8_t& cr, radio::ModemPreset& preset) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
    sf = LORA_SF; bw = LORA_BW; cr = LORA_CR; preset = radio::MODEM_NORMAL;
    return;
  }
  uint8_t p = 0xFF;
  nvs_get_u8(h, NVS_KEY_MODEM, &p);
  if (p < 4) {
    preset = (radio::ModemPreset)p;
    auto& cfg = MODEM_PRESETS[p];
    sf = cfg.sf; bw = cfg.bw; cr = cfg.cr;
  } else if (p == radio::MODEM_CUSTOM) {
    preset = radio::MODEM_CUSTOM;
    uint8_t sfv = LORA_SF;
    nvs_get_u8(h, NVS_KEY_SF, &sfv);
    sf = (sfv >= 7 && sfv <= 12) ? sfv : LORA_SF;
    uint16_t bwx10 = 1250;
    nvs_get_u16(h, NVS_KEY_BW, &bwx10);
    bw = (float)bwx10 / 10.0f;
    if (!isValidBw(bw)) bw = LORA_BW;
    uint8_t crv = LORA_CR;
    nvs_get_u8(h, NVS_KEY_CR, &crv);
    cr = (crv >= 5 && crv <= 8) ? crv : LORA_CR;
  } else {
    // Legacy: старая прошивка с только lora_sf
    uint8_t sfv = LORA_SF;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_SF, &sfv);
    if (err == ESP_OK && sfv >= 7 && sfv <= 12 && sfv != LORA_SF) {
      preset = radio::MODEM_CUSTOM; sf = sfv; bw = LORA_BW; cr = LORA_CR;
    } else {
      preset = radio::MODEM_NORMAL; sf = LORA_SF; bw = LORA_BW; cr = LORA_CR;
    }
  }
  nvs_close(h);
}

static void saveModemToNvs(radio::ModemPreset preset, uint8_t sf, float bw, uint8_t cr) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, NVS_KEY_MODEM, (uint8_t)preset);
  if (preset == radio::MODEM_CUSTOM) {
    nvs_set_u8(h, NVS_KEY_SF, sf);
    nvs_set_u16(h, NVS_KEY_BW, (uint16_t)(bw * 10.0f + 0.5f));
    nvs_set_u8(h, NVS_KEY_CR, cr);
  }
  nvs_commit(h);
  nvs_close(h);
}

static void applyModemToChip(uint8_t sf, float bw, uint8_t cr) {
  if (!chipOk()) return;
  uint16_t preamble = (sf >= 10) ? 16 : 8;
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  rlApplyModem(sf, bw, cr, preamble);
#else
  lora->setSpreadingFactor(sf);
  lora->setBandwidth(bw);
  lora->setCodingRate(cr);
  lora->setPreambleLength(preamble);
#endif
  s_hwSf = sf;
}

namespace radio {

bool init() {
  s_radioChipReady = false;
  // SPI: явно задаём пины LoRa. Arduino 3.x: SPI.end() сбрасывает _spi, иначе begin()
  // не переконфигурирует пины (V4 RX не работал без этого).
  // T-Lora Pager: общая SPI с ST7796 — не делаем SPI.end(), иначе ломается TFT после displayInit.
  // T-Beam ESP32: SPI default (18,19,23) совпадает с LoRa — SPI.end() безопасен.
#if !defined(ARDUINO_LILYGO_T_LORA_PAGER)
  SPI.end();
  delay(10);
#endif
#if defined(ARDUINO_LILYGO_T_BEAM)
  // Meshtastic src/main.cpp initSPI(): CS high, then SPI.begin(SCK,MISO,MOSI,CS), SPI.setFrequency(4e6)
  pinMode(LORA_NSS, OUTPUT);
  digitalWrite(LORA_NSS, HIGH);
#endif
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
#if defined(ARDUINO_LILYGO_T_BEAM)
  // 4 MHz как в Meshtastic; при CHIP_NOT_FOUND иногда надёжнее 2 MHz (дефолт RadioLib) — см. kSpiLoraTbeam ниже.
  SPI.setFrequency(2000000);
#endif

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

#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  // mod / lora1262 / lora1276 создаются в блоке begin ниже (NVS fast path или полный probe).
#elif defined(ARDUINO_LILYGO_T_BEAM) && defined(RIFTLINK_T_BEAM_LORA_SX127X)
  static const SPISettings kSpiLoraTbeam(2000000, MSBFIRST, SPI_MODE0);
  mod = new Module(LORA_NSS, LORA_DIO0, LORA_RST, LORA_DIO1, SPI, kSpiLoraTbeam);
#else
  mod = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
#endif
#if !defined(RIFTLINK_T_BEAM_LORA_AUTO)
  lora = new LoRaChip(mod);
#endif

  float freq = region::getFreq();
  int power = region::getPower();

  // Загрузка моdem-конфига из NVS (пресет или custom SF/BW/CR)
  uint8_t initSf; float initBw; uint8_t initCr;
  loadModemFromNvs(initSf, initBw, initCr, s_preset);

  uint16_t preamble = (initSf >= 10) ? 16 : 8;
  int16_t st = RADIOLIB_ERR_UNKNOWN;
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  {
    static const SPISettings kSpiLoraTbeam(2000000, MSBFIRST, SPI_MODE0);
    s_tbeamIsSx127x = false;

    uint8_t tbeamRfNvs = 0;
    {
      nvs_handle_t h;
      if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, NVS_KEY_TBEAM_RF, &tbeamRfNvs);
        nvs_close(h);
      }
    }

    bool tbeamFastOk = false;
    if (tbeamRfNvs == 2u) {
      mod = new Module(LORA_NSS, LORA_DIO0, LORA_RST, LORA_DIO1, SPI, kSpiLoraTbeam);
      lora1276 = new SX1276(mod);
      delay(20);
      st = lora1276->begin(freq, initBw, initSf, initCr, RADIOLIB_SX127X_SYNC_WORD, (int8_t)power, preamble, 0);
      if (st == RADIOLIB_ERR_NONE) {
        s_tbeamIsSx127x = true;
        tbeamFastOk = true;
        Serial.println("[RiftLink] T-Beam: SX1276/RFM9x (быстрый путь NVS, без probe SX1262)");
      } else {
        Serial.printf("[RiftLink] T-Beam: NVS=RFM9x, begin code=%d — полный probe\n", (int)st);
        delete lora1276;
        lora1276 = nullptr;
        delete mod;
        mod = nullptr;
        nvs_handle_t hw;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &hw) == ESP_OK) {
          nvs_erase_key(hw, NVS_KEY_TBEAM_RF);
          nvs_commit(hw);
          nvs_close(hw);
        }
        st = RADIOLIB_ERR_UNKNOWN;
      }
    }

    if (!tbeamFastOk) {
      mod = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, SPI, kSpiLoraTbeam);
      lora1262 = new SX1262(mod);

      const float kTcxo[] = {0.0f, 1.8f};
      delay(25);
      for (size_t i = 0; i < sizeof(kTcxo) / sizeof(kTcxo[0]); i++) {
        if (i > 0) {
          (void)lora1262->reset();
          delay(15);
        }
        st = lora1262->begin(freq, initBw, initSf, initCr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, power, preamble,
            kTcxo[i], false);
        if (st == RADIOLIB_ERR_NONE) {
          if (i > 0) {
            Serial.printf("[RiftLink] Radio: SX1262 ok with tcxo=%.1f V (после XTAL/сброса)\n", (double)kTcxo[i]);
          }
          break;
        }
        Serial.printf("[RiftLink] Radio begin failed: tcxo=%.1f code=%d\n", (double)kTcxo[i], (int)st);
        if (st == RADIOLIB_ERR_CHIP_NOT_FOUND) {
          break;
        }
      }
      if (st == RADIOLIB_ERR_CHIP_NOT_FOUND) {
        delete lora1262;
        lora1262 = nullptr;
        delete mod;
        mod = nullptr;
        mod = new Module(LORA_NSS, LORA_DIO0, LORA_RST, LORA_DIO1, SPI, kSpiLoraTbeam);
        lora1276 = new SX1276(mod);
        delay(20);
        st = lora1276->begin(freq, initBw, initSf, initCr, RADIOLIB_SX127X_SYNC_WORD, (int8_t)power, preamble, 0);
        if (st == RADIOLIB_ERR_NONE) {
          s_tbeamIsSx127x = true;
          Serial.println("[RiftLink] T-Beam: авто — SX1276/RFM9x (после SX1262 CHIP_NOT_FOUND)");
        } else {
          Serial.printf("[RiftLink] Radio init failed: SX1276 code=%d (после SX1262 -2)\n", (int)st);
          delete lora1276;
          lora1276 = nullptr;
          delete mod;
          mod = nullptr;
          return false;
        }
      } else if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("[RiftLink] Radio init failed: SX1262 code=%d\n", (int)st);
        delete lora1262;
        lora1262 = nullptr;
        delete mod;
        mod = nullptr;
        return false;
      }
    }
  }
#elif defined(ARDUINO_LILYGO_T_BEAM) && defined(RIFTLINK_T_BEAM_LORA_SX127X)
  delay(50);
  st = lora->begin(freq, initBw, initSf, initCr, RADIOLIB_SX127X_SYNC_WORD, (int8_t)power, preamble, 0);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf(
        "[RiftLink] Radio init failed: code=%d (SPI/чип SX127x; только RFM9x)\n", (int)st);
    return false;
  }
#else
  st = lora->begin(freq, initBw, initSf, initCr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, power, preamble, TCXO_VOLTAGE,
      false);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf(
        "[RiftLink] Radio init failed: code=%d (SPI/чип; не обязательно антенна — см. UART и ревизию SX1262)\n",
        (int)st);
    return false;
  }
#endif

#ifdef ARDUINO_heltec_wifi_lora_32_V4
  lora->setDio2AsRfSwitch(true);  // DIO2 управляет RF switch FEM
#endif
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  if (s_tbeamIsSx127x) {
    (void)lora1276->setCurrentLimit(140);
    lora1276->setCRC(true);
    Serial.println("[RiftLink] Radio: SX1276 sync 0x12, CRC on");
  } else {
    (void)lora1262->setCurrentLimit(140.0f);
    lora1262->setDio2AsRfSwitch(true);
    if (mod && mod->SPIsetRegValue(0x8B5, 0x01, 0, 0) == RADIOLIB_ERR_NONE) {
      Serial.println("[RiftLink] SX1262: reg 0x8B5 RX patch applied");
    }
    lora1262->setCRC(2);
  }
  if (s_tbeamIsSx127x) {
    lora1276->setSyncWord(0x12);
  } else {
    lora1262->setSyncWord(0x12);
  }
  {
    int irqPin = s_tbeamIsSx127x ? LORA_DIO0 : LORA_DIO1;
    pinMode(irqPin, INPUT);
    attachInterrupt(digitalPinToInterrupt(irqPin), onDio1Rise, RISING);
  }
#elif defined(ARDUINO_LILYGO_T_BEAM) && defined(RIFTLINK_T_BEAM_LORA_SX127X)
  (void)lora->setCurrentLimit(140);
  lora->setCRC(true);
  Serial.println("[RiftLink] Radio: SX1276/RFM9x (sync 0x12, CRC on; EU868 через SX1276 в RadioLib)");
  lora->setSyncWord(0x12);
  pinMode(LORA_IRQ_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(LORA_IRQ_PIN), onDio1Rise, RISING);
#else
  lora->setCRC(2);  // CRC 2 bytes
  lora->setSyncWord(0x12);  // Private network
  pinMode(LORA_IRQ_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(LORA_IRQ_PIN), onDio1Rise, RISING);
#endif
#if defined(ARDUINO_LILYGO_T_LORA_PAGER) && defined(TPAGER_LORA_DIO2_RF_SWITCH)
  lora->setDio2AsRfSwitch(true);  // только если по схеме SX1262 DIO2 = RF switch
#endif
  s_meshSf = initSf;
  s_hwSf = initSf;
  s_bw = initBw;
  s_cr = initCr;
  Serial.printf("[RiftLink] Modem: %s SF%u BW%.0f CR4/%u\n",
      modemPresetName(s_preset), initSf, initBw, initCr);
  s_radioMutex = xSemaphoreCreateMutex();
  if (!s_radioMutex) return false;
  s_radioChipReady = true;
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
      const uint8_t v = s_tbeamIsSx127x ? 2u : 1u;
      (void)nvs_set_u8(h, NVS_KEY_TBEAM_RF, v);
      (void)nvs_commit(h);
      nvs_close(h);
    }
  }
#endif
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
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  rlStandby();
#else
  lora->standby();
#endif
}

uint32_t getTimeOnAir(size_t len) {
  if (!chipOk()) return 0;
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  return (uint32_t)rlGetTimeOnAir(len);
#else
  return (uint32_t)lora->getTimeOnAir(len);
#endif
}

bool isChannelFree() {
  if (!chipOk()) return true;
  // Не трогать SX1262, пока планировщик/loop в окне RX (иначе standby ломает приём).
  if (s_rxListenActive.load(std::memory_order_relaxed)) return false;
  if (!takeMutex(pdMS_TO_TICKS(50))) return false;  // не блокировать надолго
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  rlStandby();
  int16_t cad = rlScanChannel();
#else
  lora->standby();
  int16_t cad = lora->scanChannel();
#endif
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

void setAsyncMode(bool on) { s_asyncMode = on; }

bool sendDirectInternal(const uint8_t* data, size_t len, char* reasonBuf, size_t reasonLen, bool skipCad) {
  s_dio1IrqCount.store(0, std::memory_order_relaxed);  // не читать старые TX/RX IRQ как "новый RX"
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  const size_t kMaxRadioPayload =
      s_tbeamIsSx127x ? RADIOLIB_SX127X_MAX_PACKET_LENGTH : RADIOLIB_SX126X_MAX_PACKET_LENGTH;
#elif defined(ARDUINO_LILYGO_T_BEAM) && defined(RIFTLINK_T_BEAM_LORA_SX127X)
  const size_t kMaxRadioPayload = RADIOLIB_SX127X_MAX_PACKET_LENGTH;
#else
  const size_t kMaxRadioPayload = RADIOLIB_SX126X_MAX_PACKET_LENGTH;
#endif
  if (!chipOk() || len > kMaxRadioPayload) {
    const char* cause = !s_radioChipReady ? "radio_init_failed" : "pkt_too_long";
    radioSendReason(reasonBuf, reasonLen, cause);
    RIFTLINK_DIAG("RADIO", "event=TX_RESULT ok=0 cause=%s len=%u", cause, (unsigned)len);
    return false;
  }

  // BEB decay: без congestion N с → уменьшить CW
  uint32_t now = (uint32_t)millis();
  uint8_t c = s_cadBusyCount.load(std::memory_order_relaxed);
  if (c > 0 && (now - s_lastCongestionTime.load(std::memory_order_relaxed)) >= BEB_DECAY_MS) {
    s_cadBusyCount.store(c - 1, std::memory_order_relaxed);
    s_lastCongestionTime.store(now, std::memory_order_relaxed);
  }

  uint32_t toa = getTimeOnAir(len);
  if (!duty_cycle::canSend(toa)) {
    Serial.println("[RiftLink] Duty cycle limit (EU 1%) — TX skipped, попробуйте позже");
    radioSendReason(reasonBuf, reasonLen, "duty_cycle");
    RIFTLINK_DIAG("RADIO", "event=TX_RESULT ok=0 cause=duty_cycle len=%u toa_us=%lu sf=%u",
        (unsigned)len, (unsigned long)toa, (unsigned)getSpreadingFactor());
    return false;
  }

  // CSMA/CA + BEB: CAD перед TX (selftest при skipCad пропускает — иначе ложный FAIL на шумном 868 MHz)
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  rlStandby();
#else
  lora->standby();
#endif
  if (!skipCad) {
    for (int attempt = 0; attempt < CAD_MAX_RETRIES; attempt++) {
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
      int16_t cad = rlScanChannel();
#else
      int16_t cad = lora->scanChannel();
#endif
      if (cad == RADIOLIB_CHANNEL_FREE) break;
      RIFTLINK_DIAG("RADIO", "event=CAD_BUSY attempt=%d sf=%u len=%u",
          attempt + 1, (unsigned)getSpreadingFactor(), (unsigned)len);
      if (attempt < CAD_MAX_RETRIES - 1) {
        // BEB: CW = min(CW_MIN * 2^s_cadBusyCount, CW_MAX)
        uint8_t c = s_cadBusyCount.load(std::memory_order_relaxed);
        uint32_t cw = CAD_CW_MIN * (1u << (c < CAD_BEB_MAX ? c : CAD_BEB_MAX));
        uint32_t cwMax = adaptiveCwMax();
        if (cw > cwMax) cw = cwMax;
        if (c < 255) {
          s_cadBusyCount.store(c + 1, std::memory_order_relaxed);
          s_lastCongestionTime.store((uint32_t)millis(), std::memory_order_relaxed);
        }
        uint32_t backoff = (esp_random() % cw) * CAD_SLOT_TIME_MS;
        if (backoff > 0) {
          queueDeferredSend(data, len, getSpreadingFactor(), backoff);
          radioSendReason(reasonBuf, reasonLen, "cad_defer");
          RIFTLINK_DIAG("RADIO", "event=CAD_DEFER backoff_ms=%lu cw=%lu sf=%u len=%u",
              (unsigned long)backoff, (unsigned long)cw, (unsigned)getSpreadingFactor(), (unsigned)len);
          return false;
        }
      }
    }
  }

  RIFTLINK_DIAG("RADIO", "event=CAD_FREE_TX sf=%u len=%u toa_us=%lu skip_cad=%d",
      (unsigned)getSpreadingFactor(), (unsigned)len, (unsigned long)toa, skipCad ? 1 : 0);
  int16_t st = RADIOLIB_ERR_UNKNOWN;
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  st = rlTransmit(const_cast<uint8_t*>(data), len);
#else
  st = lora->transmit(const_cast<uint8_t*>(data), len);
#endif
  if (st != RADIOLIB_ERR_NONE) {
    // -705 = SPI_CMD_TIMEOUT — радио могло зависнуть после RX, пробуем standby и повторить
    if (st == -705) {
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
      rlStandby();
#else
      lora->standby();
#endif
      delay(5);
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
      st = rlTransmit(const_cast<uint8_t*>(data), len);
#else
      st = lora->transmit(const_cast<uint8_t*>(data), len);
#endif
    }
    if (st != RADIOLIB_ERR_NONE) {
      Serial.printf("[RiftLink] TX failed: %d\n", st);
      if (reasonBuf && reasonLen > 0) {
        snprintf(reasonBuf, reasonLen, "tx_err_%d", (int)st);
      }
      RIFTLINK_DIAG("RADIO", "event=TX_RESULT ok=0 cause=tx_err_%d sf=%u len=%u",
          (int)st, (unsigned)getSpreadingFactor(), (unsigned)len);
      return false;
    }
  }
  duty_cycle::recordSend(toa);
  s_cadBusyCount.store(0, std::memory_order_relaxed);  // успешная TX — сброс BEB
  uint8_t op = (len > 2 && data[0] == protocol::SYNC_BYTE) ? data[2] : 0xFF;
  RIFTLINK_DIAG("RADIO", "event=TX_RESULT ok=1 sf=%u len=%u op=0x%02X toa_us=%lu",
      (unsigned)getSpreadingFactor(), (unsigned)len, (unsigned)op, (unsigned long)toa);
  return true;
}

bool send(const uint8_t* data, size_t len, uint8_t txSf, bool priority, char* reasonBuf, size_t reasonLen) {
  // txSf: 7–12 — SF пакета; 0 — mesh SF. Весь TX только через radioCmdQueue → radioSchedulerTask (один владелец LoRa).
  uint8_t sfForPkt = (txSf >= 7 && txSf <= 12) ? txSf : getSpreadingFactor();
  if (reasonBuf && reasonLen > 0) {
    reasonBuf[0] = '\0';
  }
  if (!s_asyncMode || !radioCmdQueue) {
    radioSendReason(reasonBuf, reasonLen, !radioCmdQueue ? "no_tx_queue" : "no_async");
    return false;
  }
  return queueSend(data, len, sfForPkt, priority, reasonBuf, reasonLen);
}

bool sendDirect(const uint8_t* data, size_t len, char* reasonBuf, size_t reasonLen) {
  // Алиас приоритетного enqueue — тот же путь, что radio::send (без обхода через mutex/прямой SPI).
  return send(data, len, 0, true, reasonBuf, reasonLen);
}

int receive(uint8_t* buf, size_t maxLen) {
  if (!chipOk()) return -1;

#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  int16_t st = rlReceive(buf, maxLen);
#else
  int16_t st = lora->receive(buf, maxLen);
#endif
  if (st == RADIOLIB_ERR_RX_TIMEOUT) return 0;
  if (st < 0) return -1;
  return (int)st;
}

// SX126x: единицы 15.625 us. SX127x: таймаут RX single — число символов LoRa (макс. 1023); иначе continuous.
bool startReceiveWithTimeout(uint32_t timeoutMs) {
  if (!chipOk()) return false;
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  constexpr uint32_t kRlIrqRxFlags =
      (1UL << 1) | (1UL << 9) | (1UL << 6) | (1UL << 4) | (1UL << 5);
  constexpr uint32_t kRlIrqRxMask = (1UL << 1);
  float bw_khz = s_bw;
  if (bw_khz < 1.0f) bw_khz = LORA_BW;
  float sym_ms = (float)((uint32_t(1) << s_hwSf)) / bw_khz;
  if (sym_ms < 1e-6f) sym_ms = 1e-3f;
  uint32_t sym = (uint32_t)((float)timeoutMs / sym_ms + 0.5f);
  int16_t st;
  if (sym == 0u && timeoutMs > 0u) sym = 1u;
  if (s_tbeamIsSx127x) {
    if (sym > 1023u) {
      st = lora1276->startReceive(0, kRlIrqRxFlags, kRlIrqRxMask, 0);
    } else {
      st = lora1276->startReceive(sym, kRlIrqRxFlags, kRlIrqRxMask, 0);
    }
  } else {
    uint32_t units = (uint32_t)((uint64_t)timeoutMs * 1000 / 16);
    if (units > 0xFFFFF) units = 0xFFFFF;
    st = lora1262->startReceive(units);
  }
#elif defined(ARDUINO_LILYGO_T_BEAM) && defined(RIFTLINK_T_BEAM_LORA_SX127X)
  // Как RADIOLIB_IRQ_RX_DEFAULT_* в RadioLib PhysicalLayer.h (не все тулчейны подставляют макросы в .cpp).
  constexpr uint32_t kRlIrqRxFlags =
      (1UL << 1) | (1UL << 9) | (1UL << 6) | (1UL << 4) | (1UL << 5);  // RX_DONE, TIMEOUT, CRC_ERR, HEADER_*
  constexpr uint32_t kRlIrqRxMask = (1UL << 1);
  float bw_khz = s_bw;
  if (bw_khz < 1.0f) bw_khz = LORA_BW;
  float sym_ms = (float)((uint32_t(1) << s_hwSf)) / bw_khz;
  if (sym_ms < 1e-6f) sym_ms = 1e-3f;
  uint32_t sym = (uint32_t)((float)timeoutMs / sym_ms + 0.5f);
  int16_t st;
  if (sym == 0u && timeoutMs > 0u) sym = 1u;
  if (sym > 1023u) {
    st = lora->startReceive(0, kRlIrqRxFlags, kRlIrqRxMask, 0);
  } else {
    st = lora->startReceive(sym, kRlIrqRxFlags, kRlIrqRxMask, 0);
  }
#else
  uint32_t units = (uint32_t)((uint64_t)timeoutMs * 1000 / 16);  // 15.625 us
  if (units > 0xFFFFF) units = 0xFFFFF;
  int16_t st = lora->startReceive(units);
#endif
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
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  size_t len = rlGetPacketLength();
#else
  size_t len = lora->getPacketLength();
#endif
  if (len == 0 || len > maxLen) {
    if (len > maxLen) {
      s_rxLenOversizeDrops.fetch_add(1, std::memory_order_relaxed);
      RIFTLINK_DIAG("RADIO", "event=RX_LEN_DROP mode=async reason=oversize chip_len=%u max_len=%u",
          (unsigned)len, (unsigned)maxLen);
    }
    // Таймаут RX — перевести в standby, иначе следующий TX (selftest и т.д.) падает при BLE.
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
    rlStandby();
#else
    lora->standby();
#endif
    return 0;
  }
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  int16_t st = rlReadData(buf, len);
#else
  int16_t st = lora->readData(buf, len);
#endif
  int readLen = normalizeReadLength(len, st);
  if (st == RADIOLIB_ERR_RX_TIMEOUT) return 0;
  if (readLen < 0) {
    s_rxReadErrors.fetch_add(1, std::memory_order_relaxed);
    RIFTLINK_DIAG("RADIO", "event=RX_READ_FAIL mode=async chip_len=%u st=%d",
        (unsigned)len, (int)st);
    return -1;
  }
  if ((size_t)readLen < len) {
    s_rxShortReads.fetch_add(1, std::memory_order_relaxed);
    RIFTLINK_DIAG("RADIO", "event=RX_SHORT_READ mode=async chip_len=%u read_len=%u st=%d",
        (unsigned)len, (unsigned)readLen, (int)st);
  }
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  rlStandby();
#else
  lora->standby();
#endif
  RIFTLINK_DIAG("RADIO", "event=RX_CHAIN stage=radio_read mode=async chip_len=%u read_len=%u st=%d",
      (unsigned)len, (unsigned)readLen, (int)st);
  return readLen;
}

bool isRxPacketReadyUnderMutex() {
  if (!chipOk()) return false;
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  size_t len = rlGetPacketLength();
#else
  size_t len = lora->getPacketLength();
#endif
  return len > 0;
}

int readReceivedPacketUnderMutex(uint8_t* buf, size_t maxLen) {
  if (!chipOk() || !buf || maxLen == 0) return -1;
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  size_t len = rlGetPacketLength();
#else
  size_t len = lora->getPacketLength();
#endif
  if (len == 0) return 0;
  if (len > maxLen) {
    s_rxLenOversizeDrops.fetch_add(1, std::memory_order_relaxed);
    RIFTLINK_DIAG("RADIO", "event=RX_LEN_DROP mode=cont reason=oversize chip_len=%u max_len=%u",
        (unsigned)len, (unsigned)maxLen);
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
    rlStandby();
#else
    lora->standby();
#endif
    return -1;
  }
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  int16_t st = rlReadData(buf, len);
#else
  int16_t st = lora->readData(buf, len);
#endif
  int readLen = normalizeReadLength(len, st);
  if (st == RADIOLIB_ERR_RX_TIMEOUT) return 0;
  if (readLen < 0) {
    s_rxReadErrors.fetch_add(1, std::memory_order_relaxed);
    RIFTLINK_DIAG("RADIO", "event=RX_READ_FAIL mode=cont chip_len=%u st=%d",
        (unsigned)len, (int)st);
    return -1;
  }
  if ((size_t)readLen < len) {
    s_rxShortReads.fetch_add(1, std::memory_order_relaxed);
    RIFTLINK_DIAG("RADIO", "event=RX_SHORT_READ mode=cont chip_len=%u read_len=%u st=%d",
        (unsigned)len, (unsigned)readLen, (int)st);
  }
  RIFTLINK_DIAG("RADIO", "event=RX_CHAIN stage=radio_read mode=cont chip_len=%u read_len=%u st=%d",
      (unsigned)len, (unsigned)readLen, (int)st);
  return readLen;
}

void getRxDiagCounters(uint32_t* oversizeDrops, uint32_t* shortReads, uint32_t* readErrors) {
  if (oversizeDrops) *oversizeDrops = s_rxLenOversizeDrops.load(std::memory_order_relaxed);
  if (shortReads) *shortReads = s_rxShortReads.load(std::memory_order_relaxed);
  if (readErrors) *readErrors = s_rxReadErrors.load(std::memory_order_relaxed);
}

bool consumeIrqEvent() {
  // Coalesce IRQ burst: one logical RX event per scheduler turn.
  // This prevents repeated processing of the same latched packet on noisy/bouncy DIO1.
  uint32_t cnt = s_dio1IrqCount.exchange(0, std::memory_order_relaxed);
  return cnt > 0;
}

int getLastRssi() {
  if (!chipOk()) return 0;
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  float rssi = rlGetRssiLastPkt();
#else
  float rssi = lora->getRSSI(true);
#endif
  if (rssi >= -150 && rssi <= 0) return (int)rssi;
  return 0;
}

void applyRegion(float freq, int power) {
  if (!chipOk()) return;
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  rlSetFreqPower(freq, power);
#else
  lora->setFrequency(freq);
  lora->setOutputPower(power);
#endif
}

void requestApplyRegion(float freq, int power) {
  if (!radioCmdQueue) return;
  RadioCmd cmd;
  cmd.type = RadioCmdType::ApplyRegion;
  cmd.priority = false;
  cmd.u.region.freqHz = (uint32_t)(freq * 1000000.0f + 0.5f);
  cmd.u.region.power = power;
  (void)xQueueSend(radioCmdQueue, &cmd, 0);
}

void setModemPreset(ModemPreset p) {
  if (p >= MODEM_PRESET_COUNT) return;
  if (p < 4) {
    auto& cfg = MODEM_PRESETS[p];
    s_meshSf = cfg.sf; s_bw = cfg.bw; s_cr = cfg.cr;
    applyModemToChip(cfg.sf, cfg.bw, cfg.cr);
  }
  s_preset = p;
  saveModemToNvs(p, s_meshSf, s_bw, s_cr);
  Serial.printf("[RiftLink] Modem preset: %s SF%u BW%.0f CR4/%u\n",
      modemPresetName(p), s_meshSf, s_bw, s_cr);
}

void setCustomModem(uint8_t sf, float bw, uint8_t cr) {
  if (sf < 7 || sf > 12 || !isValidBw(bw) || cr < 5 || cr > 8) return;
  s_meshSf = sf; s_bw = bw; s_cr = cr;
  s_preset = MODEM_CUSTOM;
  applyModemToChip(sf, bw, cr);
  saveModemToNvs(MODEM_CUSTOM, sf, bw, cr);
  Serial.printf("[RiftLink] Modem custom: SF%u BW%.0f CR4/%u\n", sf, bw, cr);
}

bool requestModemPreset(ModemPreset p) {
  if (p >= MODEM_PRESET_COUNT || !radioCmdQueue) return false;
  RadioCmd cmd;
  cmd.type = RadioCmdType::ApplyModem;
  cmd.priority = true;
  cmd.u.modem.preset = (uint8_t)p;
  cmd.u.modem.sf = 0; cmd.u.modem.bw10 = 0; cmd.u.modem.cr = 0;
  return xQueueSendToFront(radioCmdQueue, &cmd, pdMS_TO_TICKS(5)) == pdTRUE;
}

bool requestCustomModem(uint8_t sf, float bw, uint8_t cr) {
  if (sf < 7 || sf > 12 || cr < 5 || cr > 8 || !radioCmdQueue) return false;
  if (!isValidBw(bw)) return false;
  RadioCmd cmd;
  cmd.type = RadioCmdType::ApplyModem;
  cmd.priority = true;
  cmd.u.modem.preset = (uint8_t)MODEM_CUSTOM;
  cmd.u.modem.sf = sf;
  cmd.u.modem.bw10 = (uint16_t)(bw * 10.0f + 0.5f);
  cmd.u.modem.cr = cr;
  return xQueueSendToFront(radioCmdQueue, &cmd, pdMS_TO_TICKS(5)) == pdTRUE;
}

ModemPreset getModemPreset() { return s_preset; }
uint8_t getSpreadingFactor()  { return s_meshSf; }
float   getBandwidth()        { return s_bw; }
uint8_t getCodingRate()       { return s_cr; }

void setSpreadingFactor(uint8_t sf) {
  if (!chipOk() || sf < 7 || sf > 12) return;
  setCustomModem(sf, s_bw, s_cr);
}

void applyHardwareSpreadingFactor(uint8_t sf) {
  if (!chipOk() || sf < 7 || sf > 12) return;
  if (sf == s_hwSf) return;
  uint16_t preamble = (sf >= 10) ? 16 : 8;
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  rlApplySfPreamble(sf, preamble);
#else
  lora->setSpreadingFactor(sf);
  lora->setPreambleLength(preamble);
#endif
  s_hwSf = sf;
}

void applyHardwareModem(uint8_t sf, float bw, uint8_t cr) {
  if (!chipOk()) return;
  if (sf < 7 || sf > 12) return;
  if (!isValidBw(bw) || cr < 5 || cr > 8) return;
  uint16_t preamble = (sf >= 10) ? 16 : 8;
#if defined(RIFTLINK_T_BEAM_LORA_AUTO)
  rlApplyModem(sf, bw, cr, preamble);
#else
  lora->setSpreadingFactor(sf);
  lora->setBandwidth(bw);
  lora->setCodingRate(cr);
  lora->setPreambleLength(preamble);
#endif
  s_hwSf = sf;
}

bool requestSpreadingFactor(uint8_t sf) {
  if (sf < 7 || sf > 12) return false;
  return requestCustomModem(sf, s_bw, s_cr);
}

bool isReady() { return s_radioChipReady; }

}  // namespace radio
