/**
 * XL9555 — регистры совместимы с TCA9555 (16-битный I2C GPIO).
 * Адрес 0x20 — см. wiki / Meshtastic T-Pager.
 */

#include "lilygo_tpager.h"
#include <Wire.h>

#if defined(ARDUINO_LILYGO_T_LORA_PAGER)

static constexpr uint8_t kXl9555Addr = 0x20;
static constexpr int kPinSda = 3;
static constexpr int kPinScl = 2;

// Биты порта 0 на плате (wiki: GPIO3 = LoRa, GPIO4 = GNSS)
static constexpr uint8_t kBitLoraPwr = 3;
static constexpr uint8_t kBitGnssPwr = 4;

static constexpr uint8_t REG_INPUT0 = 0x00;
static constexpr uint8_t REG_INPUT1 = 0x01;
static constexpr uint8_t REG_OUTPUT0 = 0x02;
static constexpr uint8_t REG_OUTPUT1 = 0x03;
static constexpr uint8_t REG_CONFIG0 = 0x06;
static constexpr uint8_t REG_CONFIG1 = 0x07;

static bool s_wireOk = false;

static bool writeReg8(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kXl9555Addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool readReg8(uint8_t reg, uint8_t* out) {
  Wire.beginTransmission(kXl9555Addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)kXl9555Addr, 1) != 1) return false;
  *out = Wire.read();
  return true;
}

static void setPort0BitAsOutput(uint8_t bit) {
  uint8_t cfg = 0xFF;
  (void)readReg8(REG_CONFIG0, &cfg);
  cfg &= (uint8_t)~(1u << bit);
  (void)writeReg8(REG_CONFIG0, cfg);
}

static void setPort0BitLevel(uint8_t bit, bool high) {
  uint8_t out = 0;
  (void)readReg8(REG_OUTPUT0, &out);
  if (high)
    out |= (uint8_t)(1u << bit);
  else
    out &= (uint8_t)~(1u << bit);
  (void)writeReg8(REG_OUTPUT0, out);
}

void lilygoTpagerEarlyInit() {
  Wire.begin(kPinSda, kPinScl);
  Wire.setClock(400000);
  delay(20);

  Wire.beginTransmission(0x34);
  if (Wire.endTransmission() == 0) {
    Serial.println("[RiftLink] TCA8418: обнаружен (клавиатура — конфиг матрицы из LilyGoLib при необходимости)");
  }

  uint8_t probe = 0;
  if (!readReg8(REG_INPUT0, &probe)) {
    Serial.println("[RiftLink] XL9555: не отвечает на I2C — LoRa/GNSS могут не работать");
    s_wireOk = false;
    return;
  }
  s_wireOk = true;

  setPort0BitAsOutput(kBitLoraPwr);
  setPort0BitAsOutput(kBitGnssPwr);
  lilygoTpagerSetLoraPower(true);
  lilygoTpagerSetGnssPower(false);
  delay(50);
}

void lilygoTpagerSetLoraPower(bool on) {
  if (!s_wireOk) return;
  setPort0BitLevel(kBitLoraPwr, on);
}

void lilygoTpagerSetGnssPower(bool on) {
  if (!s_wireOk) return;
  setPort0BitLevel(kBitGnssPwr, on);
}

#else

void lilygoTpagerEarlyInit() {}
void lilygoTpagerSetLoraPower(bool) {}
void lilygoTpagerSetGnssPower(bool) {}

#endif
