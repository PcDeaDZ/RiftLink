/*
 * FakeTech V5 / NiceNano: линейная карта Arduino pin N -> nRF GPIO N (0..47),
 * как в Meshtastic DIY (nrf52_promicro_diy_tcxo). Иначе пины 36/39 (P1.04/P1.07)
 * недопустимы для adafruit_feather_nrf52840 (только 0..34) — HardFault до Serial.
 */

#include "variant.h"
#include "nrf.h"
#include "wiring_constants.h"
#include "wiring_digital.h"

const uint32_t g_ADigitalPinMap[] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47};

void initVariant() {
  /* Пусто: питание LoRa / LED зависят от ревизии FakeTech; не трогаем P0.13 и т.д. */
}
