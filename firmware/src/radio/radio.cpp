/**
 * RiftLink Radio Layer — LoRa SX1262 (RadioLib)
 * Heltec V3/V4: NSS=8, RST=12, DIO1=14, BUSY=13, SPI: SCK=9, MISO=11, MOSI=10
 * Heltec V4: FEM (GC1109) — GPIO7 питание, GPIO2 enable, GPIO46 PA mode
 */

#include "radio.h"
#include "region/region.h"
#include "duty_cycle/duty_cycle.h"
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

#define LORA_BW    125.0f
#define LORA_SF    7
#define LORA_CR    5
#define TCXO_VOLTAGE 1.8f   // SX1262 TCXO 1.8V (V3/V4)

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
// CW увеличен для плотных сетей — больше слотов, меньше коллизий
#define CAD_SLOT_TIME_MS  4
#define CAD_CW_MIN        8
#define CAD_CW_MAX        128
#define CAD_MAX_RETRIES   5
#define CAD_BEB_MAX       5   // макс. экспонента: CW = min(CW_MIN*2^n, CW_MAX)
#define BEB_DECAY_MS      8000  // без congestion 8 с → CW уменьшается на 1

static Module* mod = nullptr;
static std::atomic<uint8_t> s_cadBusyCount{0};  // BEB: растёт при busy/NACK/undelivered, сброс при успешной TX
static std::atomic<uint32_t> s_lastCongestionTime{0};  // для decay BEB
static SX1262* lora = nullptr;
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
  if (!lora) return;
  lora->setSpreadingFactor(sf);
  lora->setBandwidth(bw);
  lora->setCodingRate(cr);
  s_hwSf = sf;
}

namespace radio {

bool init() {
  // SPI: ESP32 default (18,19,23) != Heltec (9,11,10). Явно задаём пины.
  // Arduino 3.x: SPI.end() сбрасывает _spi, иначе begin() не переконфигурирует пины (V4 RX не работал без этого).
  // Paper: displayInit вызвал SPI.begin(EINK...). V4: WiFi/другое могло затронуть SPI.
  SPI.end();
  delay(10);
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

  // Загрузка моdem-конфига из NVS (пресет или custom SF/BW/CR)
  uint8_t initSf; float initBw; uint8_t initCr;
  loadModemFromNvs(initSf, initBw, initCr, s_preset);

  // Preamble 16 (как Meshtastic) — больше времени на синхронизацию RX, меньше потерь при «просыпании»
  int16_t st = lora->begin(freq, initBw, initSf, initCr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, power, 16, TCXO_VOLTAGE, false);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("[RiftLink] Radio init failed: code=%d (проверьте антенну и питание)\n", st);
    return false;
  }

#ifdef ARDUINO_heltec_wifi_lora_32_V4
  lora->setDio2AsRfSwitch(true);  // DIO2 управляет RF switch FEM
#endif

  lora->setCRC(2);  // CRC 2 bytes
  lora->setSyncWord(0x12);  // Private network
  s_meshSf = initSf;
  s_hwSf = initSf;
  s_bw = initBw;
  s_cr = initCr;
  Serial.printf("[RiftLink] Modem: %s SF%u BW%.0f CR4/%u\n",
      modemPresetName(s_preset), initSf, initBw, initCr);
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

void setRxListenActive(bool on) {
  s_rxListenActive.store(on, std::memory_order_relaxed);
}

void standbyChipUnderMutex() {
  if (!lora) return;
  lora->standby();
}

uint32_t getTimeOnAir(size_t len) {
  if (!lora) return 0;
  return (uint32_t)lora->getTimeOnAir(len);
}

bool isChannelFree() {
  if (!lora) return true;
  // Не трогать SX1262, пока планировщик/loop в окне RX (иначе standby ломает приём).
  if (s_rxListenActive.load(std::memory_order_relaxed)) return false;
  if (!takeMutex(pdMS_TO_TICKS(50))) return false;  // не блокировать надолго
  lora->standby();
  int16_t cad = lora->scanChannel();
  releaseMutex();
  return (cad == RADIOLIB_CHANNEL_FREE);
}

void notifyCongestion() {
  uint8_t c = s_cadBusyCount.load(std::memory_order_relaxed);
  if (c < 255) s_cadBusyCount.store(c + 1, std::memory_order_relaxed);
  s_lastCongestionTime.store((uint32_t)millis(), std::memory_order_relaxed);
}

void setAsyncMode(bool on) { s_asyncMode = on; }

bool sendDirectInternal(const uint8_t* data, size_t len, char* reasonBuf, size_t reasonLen) {
  if (!lora || len > RADIOLIB_SX126X_MAX_PACKET_LENGTH) {
    radioSendReason(reasonBuf, reasonLen, !lora ? "no_lora" : "pkt_too_long");
    RIFTLINK_DIAG("RADIO", "event=TX_RESULT ok=0 cause=%s len=%u",
        !lora ? "no_lora" : "pkt_too_long", (unsigned)len);
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

  // CSMA/CA + BEB: CAD перед TX, exponential backoff при занятом канале
  lora->standby();
  for (int attempt = 0; attempt < CAD_MAX_RETRIES; attempt++) {
    int16_t cad = lora->scanChannel();
    if (cad == RADIOLIB_CHANNEL_FREE) break;
    RIFTLINK_DIAG("RADIO", "event=CAD_BUSY attempt=%d sf=%u len=%u",
        attempt + 1, (unsigned)getSpreadingFactor(), (unsigned)len);
    if (attempt < CAD_MAX_RETRIES - 1) {
      // BEB: CW = min(CW_MIN * 2^s_cadBusyCount, CW_MAX)
      uint8_t c = s_cadBusyCount.load(std::memory_order_relaxed);
      uint32_t cw = CAD_CW_MIN * (1u << (c < CAD_BEB_MAX ? c : CAD_BEB_MAX));
      if (cw > CAD_CW_MAX) cw = CAD_CW_MAX;
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

  RIFTLINK_DIAG("RADIO", "event=CAD_FREE_TX sf=%u len=%u toa_us=%lu",
      (unsigned)getSpreadingFactor(), (unsigned)len, (unsigned long)toa);
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
  if (!lora || sf < 7 || sf > 12) return;
  setCustomModem(sf, s_bw, s_cr);
}

void applyHardwareSpreadingFactor(uint8_t sf) {
  if (!lora || sf < 7 || sf > 12) return;
  if (sf == s_hwSf) return;
  lora->setSpreadingFactor(sf);
  s_hwSf = sf;
}

void applyHardwareModem(uint8_t sf, float bw, uint8_t cr) {
  if (!lora) return;
  if (sf < 7 || sf > 12) return;
  if (!isValidBw(bw) || cr < 5 || cr > 8) return;
  lora->setSpreadingFactor(sf);
  lora->setBandwidth(bw);
  lora->setCodingRate(cr);
  s_hwSf = sf;
}

bool requestSpreadingFactor(uint8_t sf) {
  if (sf < 7 || sf > 12) return false;
  return requestCustomModem(sf, s_bw, s_cr);
}

}  // namespace radio
