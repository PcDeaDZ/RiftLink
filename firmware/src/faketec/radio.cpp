/**
 * FakeTech Radio — LoRa SX1262 (RadioLib)
 */

#include "radio.h"
#include "board.h"
#include "region.h"
#include <RadioLib.h>
#include <SPI.h>
#include <Arduino.h>

#define LORA_BW    125.0
#define LORA_SF    7
#define LORA_CR    5
#define TCXO_VOLTAGE 1.8f

static Module* mod = nullptr;
static SX1262* lora = nullptr;
static uint8_t s_currentSf = LORA_SF;
static int s_lastRssi = 0;

namespace radio {

bool init() {
  SPI.begin();

  mod = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
  lora = new SX1262(mod);

  float freq = region::getFreq();
  int power = region::getPower();

  int16_t st = lora->begin(freq, LORA_BW, LORA_SF, LORA_CR,
      RADIOLIB_SX126X_SYNC_WORD_PRIVATE, power, 16, TCXO_VOLTAGE, false);

  if (st != RADIOLIB_ERR_NONE) {
    Serial.print("[RiftLink] Radio init failed: ");
    Serial.println(st);
    return false;
  }

  lora->setCRC(2);
  lora->setSyncWord(0x12);
  return true;
}

bool send(const uint8_t* data, size_t len, uint8_t txSf, bool priority) {
  (void)priority;
  if (!lora) return false;
  uint8_t sf = (txSf >= 7 && txSf <= 12) ? txSf : s_currentSf;
  lora->setSpreadingFactor(sf);
  int16_t st = lora->transmit(const_cast<uint8_t*>(data), len);
  lora->setSpreadingFactor(s_currentSf);
  return (st == RADIOLIB_ERR_NONE);
}

int receive(uint8_t* buf, size_t maxLen) {
  if (!lora) return -1;
  int16_t st = lora->receive(buf, maxLen);
  if (st > 0) {
    s_lastRssi = lora->getRSSI();
    return st;
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
  if (sf >= 7 && sf <= 12) {
    s_currentSf = sf;
    if (lora) lora->setSpreadingFactor(sf);
  }
}

uint8_t getSpreadingFactor() {
  return s_currentSf;
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

}  // namespace radio
