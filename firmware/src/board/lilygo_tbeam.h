/**
 * LilyGO T-Beam V1.1/V1.2 — ранняя инициализация (AXP2101 PMU, I2C, питание).
 * PMU AXP2101 на I2C: SDA=21, SCL=22, addr 0x34
 * OLED SSD1306: SDA=21, SCL=22 (общая шина I2C)
 * GPS NEO-6M: UART RX=34 (ESP ← GPS), TX=12 (ESP → GPS); питание через AXP2101
 * LoRa SX1262: SPI SCK=5, MISO=19, MOSI=27, NSS=18, RST=23, DIO1=33, BUSY=32
 *   (совпадает с Arduino variant `tbeam` pins_arduino.h и Meshtastic variants/esp32/tbeam)
 */

#pragma once

#include <Arduino.h>

/** I2C + AXP2101 init. Вызывать до SPI/дисплея/радио. */
void lilygoTbeamEarlyInit();

/** Питание LoRa через AXP2101 LDO */
void lilygoTbeamSetLoraPower(bool on);

/** Питание GPS через AXP2101 LDO */
void lilygoTbeamSetGpspower(bool on);

/** Питание OLED через AXP2101 (если отдельная линия) */
void lilygoTbeamSetOledPower(bool on);

/** Напряжение батареи (mV) через AXP2101 ADC. 0 если PMU не инициализирован / батарея не обнаружена. */
uint16_t lilygoTbeamReadBatteryMv();

/** SOC из PMU (0xA4), если валиден; иначе -1 — тогда использовать кривую по напряжению. */
int lilygoTbeamReadBatteryPercent();

/** Заряжается ли батарея (AXP2101 STATUS2, как в XPowersLib). */
bool lilygoTbeamIsCharging();
