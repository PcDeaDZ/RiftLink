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
#include <nvs.h>
#include <atomic>
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

#define LORA_BW    125.0
#define LORA_SF    7
#define LORA_CR    5
#define TCXO_VOLTAGE 1.8f   // SX1262 TCXO 1.8V (V3/V4)

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
/** Логический mesh SF (NVS, HELLO, txSf=0). Отдельно от физического SF чипа при TX с rssiToSf. */
static uint8_t s_meshSf = LORA_SF;
/** Текущий SF на SX1262 — чтобы не дёргать SPI без нужды. */
static uint8_t s_hwSf = LORA_SF;
static const char* NVS_NAMESPACE = "riftlink";
static const char* NVS_KEY_SF = "lora_sf";

static uint8_t loadSfFromNvs() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return LORA_SF;
  uint8_t sf = LORA_SF;
  esp_err_t err = nvs_get_u8(h, NVS_KEY_SF, &sf);
  nvs_close(h);
  if (err != ESP_OK || sf < 7 || sf > 12) return LORA_SF;
  return sf;
}

static void saveSfToNvs(uint8_t sf) {
  if (sf < 7 || sf > 12) return;
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, NVS_KEY_SF, sf);
  nvs_commit(h);
  nvs_close(h);
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

  // Preamble 16 (как Meshtastic) — больше времени на синхронизацию RX, меньше потерь при «просыпании»
  uint8_t initSf = loadSfFromNvs();
  int16_t st = lora->begin(freq, LORA_BW, initSf, LORA_CR, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, power, 16, TCXO_VOLTAGE, false);
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
    return false;
  }

  // CSMA/CA + BEB: CAD перед TX, exponential backoff при занятом канале
  lora->standby();
  for (int attempt = 0; attempt < CAD_MAX_RETRIES; attempt++) {
    int16_t cad = lora->scanChannel();
    if (cad == RADIOLIB_CHANNEL_FREE) break;
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
        return false;
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
      if (reasonBuf && reasonLen > 0) {
        snprintf(reasonBuf, reasonLen, "tx_err_%d", (int)st);
      }
      return false;
    }
  }
  duty_cycle::recordSend(toa);
  s_cadBusyCount.store(0, std::memory_order_relaxed);  // успешная TX — сброс BEB
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

void setSpreadingFactor(uint8_t sf) {
  if (!lora || sf < 7 || sf > 12) return;
  if (sf == s_meshSf && sf == s_hwSf) return;
  lora->setSpreadingFactor(sf);
  s_meshSf = sf;
  s_hwSf = sf;
  saveSfToNvs(sf);
}

void applyHardwareSpreadingFactor(uint8_t sf) {
  if (!lora || sf < 7 || sf > 12) return;
  if (sf == s_hwSf) return;
  lora->setSpreadingFactor(sf);
  s_hwSf = sf;
}

void requestSpreadingFactor(uint8_t sf) {
  if (sf < 7 || sf > 12 || !radioCmdQueue) return;
  RadioCmd cmd;
  cmd.type = RadioCmdType::ApplySf;
  cmd.priority = false;
  cmd.u.spread.sf = sf;
  (void)xQueueSend(radioCmdQueue, &cmd, 0);
}

uint8_t getSpreadingFactor() {
  return s_meshSf;
}

}  // namespace radio
