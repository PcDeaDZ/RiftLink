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

// AXP2101 register map — сверено с XPowersLib REG/AXP2101Constants.h
static constexpr uint8_t REG_PMU_STATUS1         = 0x00;
static constexpr uint8_t REG_PMU_STATUS2         = 0x01;
static constexpr uint8_t REG_CHIP_ID             = 0x03;
static constexpr uint8_t REG_ADC_CHANNEL_CTRL    = 0x30;  // bit0 = измерение Vbat
static constexpr uint8_t REG_ADC_DATA_BAT_H      = 0x34;
static constexpr uint8_t REG_ADC_DATA_BAT_L      = 0x35;
static constexpr uint8_t REG_BAT_DET_CTRL        = 0x68;  // bit0 = detect battery
static constexpr uint8_t REG_BAT_PERCENT_DATA    = 0xA4;
static constexpr uint8_t REG_ALDO2_VOLTAGE       = 0x93;  // LoRa power
static constexpr uint8_t REG_ALDO3_VOLTAGE       = 0x94;  // GPS power
static constexpr uint8_t REG_LDO_ONOFF_CTRL0     = 0x90;  // ALDO1-4 enable bits

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

  // Без этого readRegisterH5L8(Vbat) даёт мусор / 0 — как enableBattVoltageMeasure в XPowersLib
  {
    uint8_t adc = 0;
    if (pmuReadReg(REG_ADC_CHANNEL_CTRL, &adc)) {
      adc |= 1u << 0;
      (void)pmuWriteReg(REG_ADC_CHANNEL_CTRL, adc);
    }
  }
  {
    uint8_t det = 0;
    if (pmuReadReg(REG_BAT_DET_CTRL, &det)) {
      det |= 1u << 0;
      (void)pmuWriteReg(REG_BAT_DET_CTRL, det);
    }
  }

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
  uint8_t st1 = 0;
  if (!pmuReadReg(REG_PMU_STATUS1, &st1)) return 0;
  if ((st1 & (1u << 3)) == 0) return 0;  // батарея не подключена (XPowers isBatteryConnect)

  uint8_t h = 0, l = 0;
  if (!pmuReadReg(REG_ADC_DATA_BAT_H, &h)) return 0;
  if (!pmuReadReg(REG_ADC_DATA_BAT_L, &l)) return 0;
  // XPowers readRegisterH5L8 — напряжение батареи в мВ
  uint16_t mv = (uint16_t)(((uint16_t)(h & 0x1Fu) << 8) | l);
  return mv;
}

int lilygoTbeamReadBatteryPercent() {
  if (!s_pmuOk) return -1;
  uint8_t st1 = 0;
  if (!pmuReadReg(REG_PMU_STATUS1, &st1)) return -1;
  if ((st1 & (1u << 3)) == 0) return -1;

  uint8_t p = 0;
  if (!pmuReadReg(REG_BAT_PERCENT_DATA, &p)) return -1;
  if (p > 100u) return -1;
  return (int)p;
}

bool lilygoTbeamIsCharging() {
  if (!s_pmuOk) return false;
  uint8_t st2 = 0;
  if (!pmuReadReg(REG_PMU_STATUS2, &st2)) return false;
  // XPowers AXP2101::isCharging(): (STATUS2 >> 5) == 0x01
  return (st2 >> 5) == 0x01;
}

#else

void lilygoTbeamEarlyInit() {}
void lilygoTbeamSetLoraPower(bool) {}
void lilygoTbeamSetGpspower(bool) {}
void lilygoTbeamSetOledPower(bool) {}
uint16_t lilygoTbeamReadBatteryMv() { return 0; }
int lilygoTbeamReadBatteryPercent() { return -1; }
bool lilygoTbeamIsCharging() { return false; }

#endif
