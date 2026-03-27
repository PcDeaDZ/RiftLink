/**
 * FakeTech Radio — LoRa SX1262 (HT-RA62 / RA-01SH)
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace radio {

bool init();
/** Последний код RadioLib после transmit (для диагностики). */
int16_t getLastTxError();
bool send(const uint8_t* data, size_t len, uint8_t txSf = 0, bool priority = false);
int receive(uint8_t* buf, size_t maxLen);
/** Опрос BLE/SoftDevice между ожиданием IRQ LoRa (P0: NUS не голодать при RX). nullptr = только yield(). */
void setRxBlePoll(void (*fn)(void));
int getLastRssi();
void applyRegion(float freq, int power);

/** Пресеты как у Heltec `radio::MODEM_*` (firmware/src/radio/radio.cpp). */
void applyModemPreset(uint8_t presetIndex);
void applyModemConfig(uint8_t sf, float bw, uint8_t cr);

void setSpreadingFactor(uint8_t sf);
uint8_t getSpreadingFactor();
/** SF для TX из очереди (async_tx): паритет с `async_tasks::queueTxRequestInternal` — всегда текущий mesh SF из настроек. */
uint8_t getMeshTxSfForQueue();
float getBandwidth();
uint8_t getCodingRate();
/** 0..3 = пресет; 4 = последний applyModemConfig без пресета. */
uint8_t getModemPresetIndex();

uint32_t getTimeOnAir(size_t len);
bool isChannelFree();

/** NACK/коллизии: уведомление для BEB (как radio.cpp на ESP). */
void notifyCongestion();
/** 0..3 — уровень «загрузки» эфира (упрощённо для msg_queue/ack_coalesce). */
uint8_t getCongestionLevel();

/** Счётчики для диагностики «слышит ли радио эфир» (таймаут RX / принятый кадр / ошибка чипа). */
struct RxDiagStats {
  uint32_t rxTimeouts;
  uint32_t rxPackets;
  /** SPI/IRQ и др. (не CRC). */
  uint32_t rxRadioErrors;
  /** RADIOLIB_ERR_CRC_MISMATCH: чужой LoRa / шум на частоте — не обязательно «нет V3». */
  uint32_t rxCrcMismatch;
};
RxDiagStats getRxDiagStats();

/** Монотонно растущий seq при каждом RX_RECOVERY (для корреляции с main / KEY). */
uint32_t getRxRecoverySeq();
/** Время millis() последнего recovery, 0 если ещё не было. */
uint32_t getLastRxRecoveryMs();

}  // namespace radio
