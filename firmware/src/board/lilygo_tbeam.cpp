/**
 * LilyGO T-Beam V1.1/V1.2 — AXP2101 PMU driver (минимальный).
 * Регистры: https://github.com/lewisxhe/XPowersLib (XPowersAXP2101.h)
 * Адрес I2C: 0x34
 *
 * Питание LoRa: ALDO2 (3.3V)
 * Питание GPS:  ALDO3 (3.3V)
 * OLED:         общее 3.3V от DCDC1 (всегда включено)
 *
 * Батарея: ADC внутри AXP2101, регистры TS/VBAT.
 */

#include "lilygo_tbeam.h"
#include <Wire.h>

#if defined(ARDUINO_LILYGO_T_BEAM)

static constexpr uint8_t kPmuAddr = 0x34;
static constexpr int kPinSda = 21;
static constexpr int kPinScl = 22;
static bool s_pmuOk = false;

static bool pmuWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kPmuAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool pmuReadReg(uint8_t reg, uint8_t* out) {
  if (!out) return false;
  Wire.beginTransmission(kPmuAddr);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  delayMicroseconds(20);
  if (Wire.requestFrom((int)kPmuAddr, 1) != 1) return false;
  if (!Wire.available()) return false;
  *out = (uint8_t)Wire.read();
  return true;
}

static bool pmuReadReg16(uint8_t regH, uint8_t regL, uint16_t* out) {
  uint8_t h = 0, l = 0;
  if (!pmuReadReg(regH, &h)) return false;
  if (!pmuReadReg(regL, &l)) return false;
  *out = ((uint16_t)h << 8) | l;
  return true;
}

// AXP2101 register map (partial)
static constexpr uint8_t REG_PMU_STATUS1       = 0x00;
static constexpr uint8_t REG_PMU_STATUS2       = 0x01;
static constexpr uint8_t REG_CHIP_ID           = 0x03;
static constexpr uint8_t REG_VBUS_IPSOUT       = 0x30;
static constexpr uint8_t REG_BAT_CHARGING_CTRL = 0x62;
static constexpr uint8_t REG_ALDO2_VOLTAGE     = 0x93;  // LoRa power
static constexpr uint8_t REG_ALDO3_VOLTAGE     = 0x94;  // GPS power
static constexpr uint8_t REG_LDO_ONOFF_CTRL0   = 0x90;  // ALDO1-4 enable bits
static constexpr uint8_t REG_ADC_ENABLE        = 0x30;
static constexpr uint8_t REG_VBAT_H            = 0x78;  // Battery voltage high byte
static constexpr uint8_t REG_VBAT_L            = 0x79;  // Battery voltage low byte
static constexpr uint8_t REG_BAT_PERCENT       = 0xA4;  // Battery SOC (state of charge)

static void setAldo(uint8_t aldoReg, uint8_t voltageSetting, uint8_t enableBit, bool on) {
  if (!s_pmuOk) return;
  pmuWriteReg(aldoReg, voltageSetting);  // 3.3V = 0x1C (28 * 100mV + 500mV = 3300mV)
  uint8_t ctrl = 0;
  pmuReadReg(REG_LDO_ONOFF_CTRL0, &ctrl);
  if (on)
    ctrl |= (1u << enableBit);
  else
    ctrl &= ~(1u << enableBit);
  pmuWriteReg(REG_LDO_ONOFF_CTRL0, ctrl);
}

void lilygoTbeamEarlyInit() {
  Wire.begin(kPinSda, kPinScl);
  Wire.setClock(400000);
  delay(20);

  uint8_t chipId = 0;
  if (!pmuReadReg(REG_CHIP_ID, &chipId)) {
    Serial.println("[RiftLink] AXP2101: не отвечает — питание LoRa/GPS через PMU недоступно");
    s_pmuOk = false;
    return;
  }
  Serial.printf("[RiftLink] AXP2101: chip ID 0x%02X\n", chipId);
  s_pmuOk = true;

  lilygoTbeamSetLoraPower(true);
  lilygoTbeamSetGpspower(false);
  lilygoTbeamSetOledPower(true);
  delay(50);
}

void lilygoTbeamSetLoraPower(bool on) {
  // ALDO2: 3.3V (0x1C = 28, voltage = 500 + 28*100 = 3300mV), bit 1 in LDO_ONOFF_CTRL0
  setAldo(REG_ALDO2_VOLTAGE, 0x1C, 1, on);
}

void lilygoTbeamSetGpspower(bool on) {
  // ALDO3: 3.3V, bit 2 in LDO_ONOFF_CTRL0
  setAldo(REG_ALDO3_VOLTAGE, 0x1C, 2, on);
}

void lilygoTbeamSetOledPower(bool on) {
  (void)on;
  // OLED питается от DCDC1 3.3V (always-on). Нет отдельной LDO.
}

uint16_t lilygoTbeamReadBatteryMv() {
  if (!s_pmuOk) return 0;
  uint8_t h = 0, l = 0;
  if (!pmuReadReg(REG_VBAT_H, &h)) return 0;
  if (!pmuReadReg(REG_VBAT_L, &l)) return 0;
  // AXP2101: VBAT[13:0], шаг 1 mV
  uint16_t raw = ((uint16_t)(h & 0x3F) << 8) | l;
  return raw;
}

bool lilygoTbeamIsCharging() {
  if (!s_pmuOk) return false;
  uint8_t st = 0;
  if (!pmuReadReg(REG_PMU_STATUS1, &st)) return false;
  // Bit 5 in STATUS1: charging active
  return (st & 0x20) != 0;
}

#else

void lilygoTbeamEarlyInit() {}
void lilygoTbeamSetLoraPower(bool) {}
void lilygoTbeamSetGpspower(bool) {}
void lilygoTbeamSetOledPower(bool) {}
uint16_t lilygoTbeamReadBatteryMv() { return 0; }
bool lilygoTbeamIsCharging() { return false; }

#endif
