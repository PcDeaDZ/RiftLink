/**

 * FakeTech Radio — LoRa SX1262 (RadioLib)

 * Параметры modem (SF/BW/CR/preamble) — паритет с firmware/src/radio/radio.cpp (Heltec V3).

 */



#include "radio.h"

#include "board.h"

#include "duty_cycle.h"

#include "region.h"

#include "storage.h"

#include "log.h"

#include "protocol/packet.h"

#include <RadioLib.h>

#include <SPI.h>

#include <Arduino.h>

/**
 * Сброс питания SX1262 (LORA_PWR_EN) на каждом 4-м hard RX recovery — опционально, по умолчанию выкл.
 * Включение: -DRIFTLINK_LORA_PWR_CYCLE_ON_HARD_RECOVERY=1 в build_flags.
 */
#ifndef RIFTLINK_LORA_PWR_CYCLE_ON_HARD_RECOVERY
#define RIFTLINK_LORA_PWR_CYCLE_ON_HARD_RECOVERY 0
#endif

#define TCXO_VOLTAGE 1.8f

#if defined(LORA_RXEN_PIN) && (LORA_RXEN_PIN >= 0)
static void ensureLoraRxenGpio() {
  pinMode((uint8_t)LORA_RXEN_PIN, OUTPUT);
  digitalWrite((uint8_t)LORA_RXEN_PIN, HIGH);
}
#define RADIO_ENSURE_RXEN_AFTER_TX() ensureLoraRxenGpio()
#else
#define RADIO_ENSURE_RXEN_AFTER_TX() ((void)0)
#endif

/** Как MODEM_PRESETS[] на ESP: Speed, Normal, Range, MaxRange */

struct ModemCfg {

  uint8_t sf;

  float bw;

  uint8_t cr;

};



static const ModemCfg kPresets[] = {

    {7, 250.0f, 5},

    {7, 125.0f, 5},

    {10, 125.0f, 5},

    {12, 125.0f, 8},

};



static Module* mod = nullptr;

static SX1262* lora = nullptr;

static uint8_t s_meshSf = 7;

static float s_bw = 125.0f;

static uint8_t s_cr = 5;

static uint8_t s_presetIdx = 1;

static bool s_customModem = false;

static int s_lastRssi = 0;

static int16_t s_lastTxErr = RADIOLIB_ERR_NONE;

static uint32_t s_rxTimeouts = 0;

/** Валидные кадры (RiftLink parse OK или ветка st>0); после перезагрузки MCU снова 0 — не путать с «снова глухой RX». */
static uint32_t s_rxPackets = 0;

static uint32_t s_rxRadioErrors = 0;

static uint32_t s_rxCrcMismatch = 0;

/** Throttle: лог нестандартного кода RadioLib (не таймаут, не CRC). */
static uint32_t s_lastRxOtherErrLogMs = 0;

static uint32_t s_lastRxParseFailLogMs = 0;

/** Лог RX_CONT_ARM не чаще чем раз в N мс (как на V3, но без спама каждый цикл). */
static uint32_t s_lastRxArmLogMs = 0;
static bool s_rxSilenceHintLogged = false;

/** RX recovery: тишина после последнего валидного RiftLink-кадра + рост rxTimeouts без новых пакетов. */
static uint32_t s_lastGoodRiftlinkRxMs = 0;
static uint32_t s_rxTimeoutsAtLastGoodPacket = 0;
static uint32_t s_radioInitOkMs = 0;
static uint32_t s_lastRxRecoveryMs = 0;
static uint32_t s_rxRecoverySeq = 0;
static uint8_t s_recoveriesSinceGoodRx = 0;

/** Между ожиданиями DIO1 IRQ (как RadioLib SX126x::receive) — BLE NUS / SoftDevice. */
static void (*s_rxBlePoll)(void) = nullptr;

/** Мин. интервал между recovery (мс). */
#define FAKETEC_RX_RECOVERY_MIN_GAP_MS 10000u
/**
 * Сколько таймаутов RX накопилось с момента последнего валидного RiftLink-кадра (~10/с при SF7/BW125).
 * Вместе с SILENCE_MS: за ~35–40 с тишины набегает ~350–400 таймаутов.
 */
#define FAKETEC_RX_RECOVERY_TIMEOUT_DELTA 400u
/** Сколько мс без приёма валидного кадра при «ожидаемом» эфире — кандидат на recovery. */
#define FAKETEC_RX_RECOVERY_SILENCE_MS 35000u
/** Если с boot ещё ни одного валидного кадра — мягкий порог по таймаутам (мс от init). */
#define FAKETEC_RX_RECOVERY_BOOT_NO_RX_MS 90000u
/** За ~90 с при ~10 таймаутах/с — порядка 900; 800 ловит «глухой» приём без первого кадра. */
#define FAKETEC_RX_RECOVERY_BOOT_TIMEOUT_DELTA 800u

#define STORAGE_KEY_MODEM_PRESET "modem_preset"

/** Упрощённый CAD перед TX (паритет с идеей scanChannel на ESP). */
#define FAKETEC_CAD_MAX_RETRIES 5



static void applyCfgToChip(const ModemCfg& c) {

  if (!lora) return;

  s_meshSf = c.sf;

  s_bw = c.bw;

  s_cr = c.cr;

  lora->setSpreadingFactor(c.sf);

  lora->setBandwidth(c.bw);

  lora->setCodingRate(c.cr);

  const uint16_t preamble = (c.sf >= 10) ? 16 : 8;

  lora->setPreambleLength(preamble);

#if LORA_DIO2_RF_SWITCH
  /* После смены SF/BW чип может сбрасывать режим DIO2; без switch в положении RX — rx_pkt=0 при живом TX. */
  (void)lora->setDio2AsRfSwitch(true);
#endif

}



static bool validBw(float bw) {

  const float valid[] = {62.5f, 125.0f, 250.0f, 500.0f};

  for (float v : valid) {

    float d = bw - v;
    if (d < 0) d = -d;
    if (d < 0.1f) return true;

  }

  return false;

}



namespace radio {



int16_t getLastTxError() {

  return s_lastTxErr;

}

void setRxBlePoll(void (*fn)(void)) { s_rxBlePoll = fn; }

/** Паритет с RadioLib 6.6 SX126x::receive (LoRa): ожидание DIO1 + yield + опционально BLE. */
static int16_t sx1262ReceiveLoRaWithPoll(uint8_t* data, size_t len) {
  if (!lora) return RADIOLIB_ERR_UNKNOWN;
  int16_t state = lora->standby();
  if (state != RADIOLIB_ERR_NONE) return state;
  float symbolLength = (float)(uint32_t(1) << s_meshSf) / (float)s_bw;
  uint32_t timeout = (uint32_t)(symbolLength * 100.0f);
  if (timeout < 1) timeout = 1;
  uint32_t timeoutValue = (uint32_t)(((float)timeout * 1000.0f) / 15.625f);
  if (timeoutValue > 0xFFFFFFu) timeoutValue = 0xFFFFFFu;
  state = lora->startReceive(timeoutValue);
  if (state != RADIOLIB_ERR_NONE) return state;
  uint32_t start = millis();
  bool softTimeout = false;
  while (!digitalRead(LORA_DIO1)) {
    if (s_rxBlePoll) s_rxBlePoll();
    yield();
    if ((uint32_t)(millis() - start) > timeout) {
      softTimeout = true;
      break;
    }
  }
  state = lora->standby();
  if (state != RADIOLIB_ERR_NONE && state != RADIOLIB_ERR_SPI_CMD_TIMEOUT) return state;
  if ((lora->getIrqStatus() & RADIOLIB_SX126X_IRQ_TIMEOUT) || softTimeout) {
    (void)lora->standby();
    return RADIOLIB_ERR_RX_TIMEOUT;
  }
  return lora->readData(data, len);
}

#if RIFTLINK_LORA_PWR_CYCLE_ON_HARD_RECOVERY
/** Счётчик только hard_begin (power-cycle SX1262). Сброс при новом валидном RX. */
static uint32_t s_hardRxRecoveryCount = 0;
#endif

static void recordGoodRiftlinkPacket() {
  s_lastGoodRiftlinkRxMs = millis();
  s_rxTimeoutsAtLastGoodPacket = s_rxTimeouts;
  s_recoveriesSinceGoodRx = 0;
#if RIFTLINK_LORA_PWR_CYCLE_ON_HARD_RECOVERY
  s_hardRxRecoveryCount = 0;
#endif
}

static void bumpRecoverySeq(const char* action, const char* reason) {
  s_rxRecoverySeq++;
  s_lastRxRecoveryMs = millis();
  s_recoveriesSinceGoodRx++;
  uint32_t sinceGood = 0;
  if (s_lastGoodRiftlinkRxMs != 0) sinceGood = (uint32_t)(millis() - s_lastGoodRiftlinkRxMs);
  RIFTLINK_DIAG("RADIO",
      "event=RX_RECOVERY action=%s reason=%s seq=%lu tmo=%lu pkt=%lu err=%lu since_good_ms=%lu n=%u", action, reason,
      (unsigned long)s_rxRecoverySeq, (unsigned long)s_rxTimeouts, (unsigned long)s_rxPackets,
      (unsigned long)s_rxRadioErrors, (unsigned long)sinceGood, (unsigned)s_recoveriesSinceGoodRx);
}

static void performRxRecoverySoft(const char* reason) {
  if (!lora) return;
  bumpRecoverySeq("soft", reason);
  (void)lora->standby();
  /* clearIrqStatus() в RadioLib SX126x — protected; сброс IRQ произойдёт в следующем receive() (startReceive/readData). */
  float freq = region::getFreq();
  int power = region::getPower();
  lora->setFrequency(freq);
  lora->setOutputPower(power);
  applyCfgToChip({s_meshSf, s_bw, s_cr});
  lora->setCRC(2);
  lora->setSyncWord(0x12);
#if LORA_DIO2_RF_SWITCH
  (void)lora->setDio2AsRfSwitch(true);
#endif
#if defined(LORA_RXEN_PIN) && (LORA_RXEN_PIN >= 0)
  ensureLoraRxenGpio();
#endif
  delay(2);
}

static void performRxRecoveryHard(const char* reason) {
  if (!lora) return;
  bumpRecoverySeq("hard_begin", reason);
#if RIFTLINK_LORA_PWR_CYCLE_ON_HARD_RECOVERY
  s_hardRxRecoveryCount++;
#if LORA_PWR_EN_PIN >= 0
  /* Эвристика: после серии begin() — полный сброс питания модуля (включается только через -D). */
  if (s_hardRxRecoveryCount >= 4u && (s_hardRxRecoveryCount % 4u) == 0u) {
    RIFTLINK_DIAG("RADIO", "event=RX_RECOVERY action=hard_lora_pwr_cycle n_hard=%lu", (unsigned long)s_hardRxRecoveryCount);
    pinMode(LORA_PWR_EN_PIN, OUTPUT);
    digitalWrite(LORA_PWR_EN_PIN, LOW);
    delay(40);
    digitalWrite(LORA_PWR_EN_PIN, HIGH);
    delay(80);
#if defined(LORA_RXEN_PIN) && (LORA_RXEN_PIN >= 0)
    ensureLoraRxenGpio();
#endif
  }
#endif
#endif
  const float tcxoV = LORA_MODULE_HAS_TCXO ? TCXO_VOLTAGE : 0.0f;
  float freq = region::getFreq();
  int power = region::getPower();
  const uint16_t preamble = (s_meshSf >= 10) ? 16 : 8;
  int16_t bst = lora->begin(freq, s_bw, s_meshSf, s_cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, power, preamble, tcxoV, false);
  if (bst != RADIOLIB_ERR_NONE) {
    RIFTLINK_DIAG("RADIO", "event=RX_RECOVERY action=hard_begin_fail err=%d", (int)bst);
    return;
  }
  lora->setCRC(2);
  lora->setSyncWord(0x12);
#if LORA_DIO2_RF_SWITCH
  (void)lora->setDio2AsRfSwitch(true);
#endif
#if defined(LORA_RXEN_PIN) && (LORA_RXEN_PIN >= 0)
  ensureLoraRxenGpio();
#endif
  delay(2);
}

static void maybeRxRecovery(const char* reasonHint) {
  const uint32_t now = millis();
  if (s_lastRxRecoveryMs != 0 && (uint32_t)(now - s_lastRxRecoveryMs) < FAKETEC_RX_RECOVERY_MIN_GAP_MS) return;

  const uint32_t deltaTmo = s_rxTimeouts - s_rxTimeoutsAtLastGoodPacket;
  const bool bootNoRx = (s_lastGoodRiftlinkRxMs == 0) && (s_radioInitOkMs != 0) &&
                          ((uint32_t)(now - s_radioInitOkMs) >= FAKETEC_RX_RECOVERY_BOOT_NO_RX_MS) &&
                          (deltaTmo >= FAKETEC_RX_RECOVERY_BOOT_TIMEOUT_DELTA);
  const bool silence =
      (s_lastGoodRiftlinkRxMs != 0) && ((uint32_t)(now - s_lastGoodRiftlinkRxMs) >= FAKETEC_RX_RECOVERY_SILENCE_MS);
  const bool tmoStorm = (deltaTmo >= FAKETEC_RX_RECOVERY_TIMEOUT_DELTA);
  if (!bootNoRx && !(silence && tmoStorm)) return;

  /* Первый recovery — soft; если ключа/кадра всё ещё нет — следующий hard (begin). */
  if (s_recoveriesSinceGoodRx >= 1) {
    performRxRecoveryHard(reasonHint);
  } else {
    performRxRecoverySoft(reasonHint);
  }
}

void applyModemPreset(uint8_t presetIndex) {

  if (presetIndex > 3) return;

  s_presetIdx = presetIndex;

  s_customModem = false;

  applyCfgToChip(kPresets[presetIndex]);

  (void)storage::setI8(STORAGE_KEY_MODEM_PRESET, (int8_t)presetIndex);

  RIFTLINK_DIAG("RADIO", "event=MODEM_PRESET preset=%u sf=%u bw=%.0f cr=%u", (unsigned)presetIndex,

      (unsigned)s_meshSf, (double)s_bw, (unsigned)s_cr);

}



void applyModemConfig(uint8_t sf, float bw, uint8_t cr) {

  if (!lora) return;

  if (sf < 7 || sf > 12) return;

  if (cr < 5 || cr > 8) return;

  if (!validBw(bw)) bw = 125.0f;

  ModemCfg c;

  c.sf = sf;

  c.bw = bw;

  c.cr = cr;

  s_customModem = true;

  applyCfgToChip(c);

  RIFTLINK_DIAG("RADIO", "event=MODEM_CUSTOM sf=%u bw=%.0f cr=%u", (unsigned)sf, (double)bw, (unsigned)cr);

}



bool init() {

#if LORA_PWR_EN_PIN >= 0

  pinMode(LORA_PWR_EN_PIN, OUTPUT);

  digitalWrite(LORA_PWR_EN_PIN, HIGH);

  delay(50);

#endif

#if defined(LORA_RXEN_PIN) && (LORA_RXEN_PIN >= 0)
  ensureLoraRxenGpio();
  RIFTLINK_DIAG("RADIO", "event=RXEN_INIT pin=%u level=HIGH", (unsigned)LORA_RXEN_PIN);
#endif

  SPI.setPins(LORA_MISO, LORA_SCK, LORA_MOSI);

  SPI.begin();



  static const SPISettings kSpiSettings(LORA_SPI_HZ, MSBFIRST, SPI_MODE0);

  mod = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, SPI, kSpiSettings);

  lora = new SX1262(mod);



#if !LORA_MODULE_HAS_TCXO

  lora->XTAL = true;

#endif

  const float tcxoV = LORA_MODULE_HAS_TCXO ? TCXO_VOLTAGE : 0.0f;



  float freq = region::getFreq();

  int power = region::getPower();



  int8_t stored = -1;

  uint8_t preset = 1;

  if (storage::getI8(STORAGE_KEY_MODEM_PRESET, &stored) && stored >= 0 && stored <= 3) {

    preset = (uint8_t)stored;

  }

  const ModemCfg& cfg = kPresets[preset];

  s_presetIdx = preset;

  s_customModem = false;

  s_meshSf = cfg.sf;

  s_bw = cfg.bw;

  s_cr = cfg.cr;

  const uint16_t preamble = (cfg.sf >= 10) ? 16 : 8;



  int16_t st = lora->begin(freq, cfg.bw, cfg.sf, cfg.cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, power, preamble, tcxoV, false);



  if (st != RADIOLIB_ERR_NONE) {

    RIFTLINK_DIAG("RADIO", "event=INIT ok=0 err=%d", (int)st);

    if (st == RADIOLIB_ERR_CHIP_NOT_FOUND) {

      RIFTLINK_DIAG("RADIO", "event=INIT hint=CHIP_NOT_FOUND check=SPI_NSS_RST_PWR board_h=LORA_PWR_EN_PIN");

    }

    return false;

  }



  lora->setCRC(2);

  lora->setSyncWord(0x12);

#if LORA_DIO2_RF_SWITCH
  {
    int16_t d2 = lora->setDio2AsRfSwitch(true);
    if (d2 != RADIOLIB_ERR_NONE) {
      RIFTLINK_DIAG("RADIO", "event=DIO2_RF_SWITCH err=%d note=check_board_h_LORA_DIO2_RF_SWITCH", (int)d2);
    }
  }
#endif

  RIFTLINK_DIAG("RADIO",
      "event=INIT ok=1 freq_mhz=%.2f preset=%u sf=%u bw=%.0f cr=%u preamble=%u sync=0x12 crc=2 tx_power=%d tcxo=%d "
      "dio2_rf_sw=%d",
      (double)freq, (unsigned)preset, (unsigned)cfg.sf, (double)cfg.bw, (unsigned)cfg.cr, (unsigned)preamble, power,
      LORA_MODULE_HAS_TCXO ? 1 : 0, LORA_DIO2_RF_SWITCH ? 1 : 0);

  s_radioInitOkMs = millis();
  s_lastGoodRiftlinkRxMs = 0;
  s_rxTimeoutsAtLastGoodPacket = 0;

  return true;

}



bool send(const uint8_t* data, size_t len, uint8_t txSf, bool priority) {

  (void)priority;

  if (!lora) return false;

  const uint8_t sf = (txSf >= 7 && txSf <= 12) ? txSf : s_meshSf;

  lora->setSpreadingFactor(sf);

  lora->setPreambleLength((sf >= 10) ? 16 : 8);

  const uint32_t toaUs = getTimeOnAir(len);
  if (!duty_cycle::canSend(toaUs)) {
    RIFTLINK_DIAG("RADIO", "event=TX_RESULT ok=0 cause=duty_cycle len=%u toa_us=%lu sf=%u",
        (unsigned)len, (unsigned long)toaUs, (unsigned)sf);
    Serial.println("[RiftLink] Duty cycle limit (EU 1%) — TX skipped");
    applyCfgToChip({s_meshSf, s_bw, s_cr});
    RADIO_ENSURE_RXEN_AFTER_TX();
    s_lastTxErr = RADIOLIB_ERR_NONE;
    return false;
  }

  (void)lora->standby();
  for (int attempt = 0; attempt < FAKETEC_CAD_MAX_RETRIES; attempt++) {
    int16_t cad = lora->scanChannel();
    if (cad == RADIOLIB_CHANNEL_FREE) break;
    if (attempt < FAKETEC_CAD_MAX_RETRIES - 1) delay(1 + (int)(random() % 24));
  }

  int16_t st = lora->transmit(const_cast<uint8_t*>(data), len);

  s_lastTxErr = st;

  if (st == RADIOLIB_ERR_NONE) {
    duty_cycle::recordSend(toaUs);
    RIFTLINK_DIAG("RADIO", "event=TX_RESULT ok=1 len=%u toa_us=%lu sf=%u", (unsigned)len, (unsigned long)toaUs,
        (unsigned)sf);
    /* SX126x: короткая пауза после TX перед возвратом в RX — меньше шанс пропустить ответный KEY по эфиру. */
    delay(3);
  }

  applyCfgToChip({s_meshSf, s_bw, s_cr});
  /* После TX SX1262 держит DIO2 в RF switch; внешний FEM по RXEN на fakeTec не трогает — явно в приём. */
  RADIO_ENSURE_RXEN_AFTER_TX();

  return (st == RADIOLIB_ERR_NONE);

}



int receive(uint8_t* buf, size_t maxLen) {

  if (!lora || !buf || maxLen == 0) return -1;

  /*
   * RadioLib 6.x (SX126x::receive): при timeout=0 длительность ожидания IRQ ~ 5*getTimeOnAir(len) (мс).
   * В len передаётся максимум чтения; SX1262 LoRa — не больше RADIOLIB_SX126X_MAX_PACKET_LENGTH (255).
   * Буфер у main больше 255 — если передать sizeof(buf), TOA и блокировка loop раздуваются без пользы,
   * страдают BLE/USB и шанс отдать время SoftDevice.
   */
  const size_t lenAsk = (maxLen > RADIOLIB_SX126X_MAX_PACKET_LENGTH) ? RADIOLIB_SX126X_MAX_PACKET_LENGTH : maxLen;

  /*
   * RadioLib 6.x (SX126x::receive): для LoRa таймаут = ~100 символов → ~100 мс при SF7/BW125,
   * не 30 с как у ESP в отдельной задаче (RX_CONT_ARM timeout_ms=30000). Один вызов receive() —
   * одно короткое «окно»; цикл loop() повторяет приём → суммарно эфир слушается почти непрерывно.
   */
  const uint32_t nowMs = millis();
  if (nowMs - s_lastRxArmLogMs >= 30000) {
    s_lastRxArmLogMs = nowMs;
    const float symMs = (float)(1u << s_meshSf) / s_bw;
    const unsigned sliceApprox = (unsigned)(symMs * 100.0f + 0.5f);
    RIFTLINK_DIAG("RADIO",
        "event=RX_CONT_ARM sf=%u bw=%.0f slice_ms~%u rx_tmo=%lu rx_pkt=%lu rx_err=%lu rx_crc=%lu",
        (unsigned)s_meshSf, (double)s_bw, sliceApprox, (unsigned long)s_rxTimeouts, (unsigned long)s_rxPackets,
        (unsigned long)s_rxRadioErrors, (unsigned long)s_rxCrcMismatch);
  }

  int16_t st = sx1262ReceiveLoRaWithPoll(buf, lenAsk);

  if (st == RADIOLIB_ERR_RX_TIMEOUT) {

    s_rxTimeouts++;
    if (!s_rxSilenceHintLogged && s_rxTimeouts > 600u && s_rxPackets == 0u) {
      s_rxSilenceHintLogged = true;
      RIFTLINK_DIAG("RADIO",
          "event=RX_SILENCE hint=if_RA01SH_use_pio_-e_faketec_v5_ble_ra01sh "
          "(TCXO=0_DIO2=0)_else_check_antenna_region_match_V4");
    }

    maybeRxRecovery("rx_timeout");

    return 0;

  }

  /*
   * RadioLib 6.x SX126x::receive() в конце возвращает readData(), а тот — код последней операции
   * (clearIrqStatus), т.е. RADIOLIB_ERR_NONE (0) при успешном приёме, а не длину кадра.
   * Длину берём из разбора RiftLink-заголовка (expectedLen).
   */
  if (st == RADIOLIB_ERR_NONE) {

    protocol::PacketHeader hdr;

    const uint8_t* payload = nullptr;

    size_t payloadLen = 0;

    protocol::ParseResult pr;

    /*
     * parsePacketEx() в packet.cpp: len > HEADER_LEN_PKTID + MAX_PAYLOAD → сразу false,
     * без заполнения ParseResult; при lenAsk=255 (SX1262 max) остаётся ложный parse=no_sync.
     * Передаём верхнюю границу по протоколу (макс. кадр RiftLink в эфире ≤ 224 байт).
     */
    const size_t kProtocolMaxAir = protocol::HEADER_LEN_PKTID + protocol::MAX_PAYLOAD;

    const size_t parseLen = (lenAsk > kProtocolMaxAir) ? kProtocolMaxAir : lenAsk;

    const bool parsed = protocol::parsePacketEx(buf, parseLen, &hdr, &payload, &payloadLen, &pr);

    if (parsed && pr.expectedLen > 0 && pr.expectedLen <= lenAsk) {

      s_rxPackets++;
      recordGoodRiftlinkPacket();

      s_lastRssi = lora->getRSSI();

      return (int)pr.expectedLen;

    }

    {

      const uint32_t t = millis();

      if (t - s_lastRxParseFailLogMs >= 30000) {

        s_lastRxParseFailLogMs = t;

        const char* ps = protocol::parseStatusToString(pr.status);

        RIFTLINK_DIAG("RADIO",

            "event=RX_LORA_OK_PARSE_FAIL parse=%s exp_len=%u parseLen=%u lenAsk=%u b0..3=%02X%02X%02X%02X note=chip_CRC_ok_not_RiftLink_or_trunc",

            ps ? ps : "?", (unsigned)pr.expectedLen, (unsigned)parseLen, (unsigned)lenAsk, lenAsk > 0 ? buf[0] : 0,

            lenAsk > 1 ? buf[1] : 0, lenAsk > 2 ? buf[2] : 0, lenAsk > 3 ? buf[3] : 0);

      }

    }

    return -1;

  }

  if (st > 0) {

    s_rxPackets++;
    recordGoodRiftlinkPacket();

    s_lastRssi = lora->getRSSI();

    return st;

  }

  if (st == RADIOLIB_ERR_CRC_MISMATCH) {

    s_rxCrcMismatch++;

    return -1;

  }

  s_rxRadioErrors++;

  {

    const uint32_t t = millis();

    if (t - s_lastRxOtherErrLogMs >= 20000) {

      s_lastRxOtherErrLogMs = t;

      RIFTLINK_DIAG("RADIO", "event=RX_FAIL st=%d note=not_crc_not_timeout (SPI/IRQ/header?)", (int)st);

    }

  }

  return -1;

}



int getLastRssi() {

  return s_lastRssi;

}



void applyRegion(float freq, int power) {

  if (lora) {

    lora->setFrequency(freq);

    lora->setOutputPower(power);

  }

}



void setSpreadingFactor(uint8_t sf) {

  if (sf < 7 || sf > 12) return;

  s_meshSf = sf;

  s_customModem = true;

  if (lora) {

    lora->setSpreadingFactor(sf);

    lora->setPreambleLength((sf >= 10) ? 16 : 8);

  }

}



uint8_t getSpreadingFactor() {

  return s_meshSf;

}



uint8_t getMeshTxSfForQueue() {

  uint8_t sf = s_meshSf;

  if (sf < 7 || sf > 12) sf = 7;

  return sf;

}



float getBandwidth() {

  return s_bw;

}



uint8_t getCodingRate() {

  return s_cr;

}



uint8_t getModemPresetIndex() {

  if (s_customModem) return 4;

  return s_presetIdx;

}



uint32_t getTimeOnAir(size_t len) {

  if (!lora) return 0;

  return (uint32_t)lora->getTimeOnAir(len);

}



bool isChannelFree() {

  if (!lora) return true;

  lora->standby();

  int16_t cad = lora->scanChannel();

  return (cad == RADIOLIB_CHANNEL_FREE);

}

RxDiagStats getRxDiagStats() {

  RxDiagStats s;

  s.rxTimeouts = s_rxTimeouts;

  s.rxPackets = s_rxPackets;

  s.rxRadioErrors = s_rxRadioErrors;

  s.rxCrcMismatch = s_rxCrcMismatch;

  return s;

}

static uint8_t s_congestionLevel = 0;

void notifyCongestion() {
  if (s_congestionLevel < 3) s_congestionLevel++;
}

uint8_t getCongestionLevel() { return s_congestionLevel; }

uint32_t getRxRecoverySeq() { return s_rxRecoverySeq; }

uint32_t getLastRxRecoveryMs() { return s_lastRxRecoveryMs; }

}  // namespace radio


