/*
 * Heltec Mesh Node T114 — тот же линейный GPIO, что faketec_nrf52840, но LED не на P0.15:
 * на плате T114 пин 15 — подсветка TFT (VTFT_LEDA / BL), индикатор — P1.3 (35), активный LOW
 * (Meshtastic: PIN_LED1 = 32+3, LED_STATE_ON 0).
 */
#ifndef _VARIANT_HELTEC_MESH_T114_
#define _VARIANT_HELTEC_MESH_T114_

#define VARIANT_MCK (64000000ul)
#define USE_LFXO

#include "WVariant.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PINS_COUNT (48)
#define NUM_DIGITAL_PINS (48)
#define NUM_ANALOG_INPUTS (8)
#define NUM_ANALOG_OUTPUTS (0)

/** Не P0.15 — там BL дисплея; см. board_pins.h TFT_BL / display_nrf. */
#define PIN_LED1 (35)
#define LED_BUILTIN PIN_LED1
/** Bluefruit.cpp: conn LED через LED_BLUE */
#define LED_BLUE PIN_LED1
/** Светодиод на T114 включается уровнем LOW */
#define LED_STATE_ON 0

#define WIRE_INTERFACES_COUNT 1
/** Как на T114 разъёме I2C (Meshtastic PIN_WIRE): P0.26 / P0.27 — OLED внешний; TFT по SPI1. */
#define PIN_WIRE_SDA (26)
#define PIN_WIRE_SCL (27)

/** L76K UART: MCU RX=P1.5 (37), MCU TX=P1.7 (39) — как Meshtastic heltec_mesh_node_t114 (GPS_RX/GPS_TX). */
#define PIN_SERIAL1_RX (37)
#define PIN_SERIAL1_TX (39)

#define SPI_INTERFACES_COUNT 2
#define PIN_SPI_MISO (2)
#define PIN_SPI_MOSI (47)
#define PIN_SPI_SCK (43)
#define PIN_SPI1_MISO PIN_SPI_MISO
#define PIN_SPI1_MOSI PIN_SPI_MOSI
#define PIN_SPI1_SCK PIN_SPI_SCK
#ifndef SS
#define SS 0
#endif

#ifdef __cplusplus
}
#endif

#endif
