/**
 * BQ27220 — чтение Standard Commands по SMBus Read Word / byte.
 * TI SLUUDG0B Table 7-1: Voltage 0x04, AverageCurrent 0x0E, RelativeStateOfCharge 0x1C.
 */

#include "bq27220_tpager.h"
#include <Wire.h>

#if defined(ARDUINO_LILYGO_T_LORA_PAGER)

namespace bq27220_tpager {

static constexpr uint8_t kAddr = 0x55;

static constexpr uint8_t CMD_VOLTAGE = 0x04;
static constexpr uint8_t CMD_RELATIVE_SOC = 0x1C;
static constexpr uint8_t CMD_AVERAGE_CURRENT = 0x0E;

static bool s_probed = false;
static bool s_present = false;

static bool readWord(uint8_t cmd, uint16_t* out) {
  Wire.beginTransmission(kAddr);
  Wire.write(cmd);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)kAddr, 2) != 2) return false;
  uint8_t lo = Wire.read();
  uint8_t hi = Wire.read();
  *out = (uint16_t)lo | ((uint16_t)hi << 8);
  return true;
}

static bool readByteAfterCmd(uint8_t cmd, uint8_t* out) {
  Wire.beginTransmission(kAddr);
  Wire.write(cmd);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)kAddr, 1) != 1) return false;
  *out = Wire.read();
  return true;
}

bool probe() {
  if (s_probed) return s_present;
  s_probed = true;
  Wire.beginTransmission(kAddr);
  s_present = (Wire.endTransmission() == 0);
  return s_present;
}

uint16_t readVoltageMv() {
  if (!probe()) return 0;
  uint16_t w = 0;
  if (!readWord(CMD_VOLTAGE, &w)) return 0;
  if (w < 2000u || w > 5200u) return 0;
  return w;
}

int readRelativeSocPercent() {
  if (!probe()) return -1;
  uint8_t b = 0;
  if (!readByteAfterCmd(CMD_RELATIVE_SOC, &b)) return -1;
  if (b > 100u) return -1;
  return (int)b;
}

int16_t readAverageCurrentMa() {
  if (!probe()) return 0;
  uint16_t w = 0;
  if (!readWord(CMD_AVERAGE_CURRENT, &w)) return 0;
  return (int16_t)w;
}

bool isCharging() {
  int16_t i = readAverageCurrentMa();
  if (i > 25) return true;
  if (i < -10) return false;
  return false;
}

}  // namespace bq27220_tpager

#else

namespace bq27220_tpager {

bool probe() { return false; }
uint16_t readVoltageMv() { return 0; }
int readRelativeSocPercent() { return -1; }
int16_t readAverageCurrentMa() { return 0; }
bool isCharging() { return false; }

}  // namespace bq27220_tpager

#endif
