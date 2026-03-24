/**
 * LilyGO T-Lora Pager — ранняя инициализация (I2C, XL9555, питание LoRa/GNSS).
 * Пины и XL9555: https://wiki.lilygo.cc/get_started/en/LoRa_GPS/T-LoraPager/T-LoraPager.html
 */

#pragma once

#include <Arduino.h>

/** SDA=3, SCL=2 — до SPI радио/дисплея */
void lilygoTpagerEarlyInit();

/** Питание SX1262 через GPIO расширителя (XL9555), без этого SPI LoRa молчит */
void lilygoTpagerSetLoraPower(bool on);

/** Питание MIA-M10Q (GNSS) через XL9555 */
void lilygoTpagerSetGnssPower(bool on);
