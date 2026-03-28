/*
 * RiftLink nRF52840 — линейная нумерация GPIO как в Meshtastic DIY / Heltec T114:
 * 0..31 = P0.00..P0.31, 32..47 = P1.00..P1.15
 */
#ifndef _VARIANT_FAKETEC_NRF52840_
#define _VARIANT_FAKETEC_NRF52840_

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

#define PIN_LED1 (15)
#define LED_BUILTIN PIN_LED1
/** Bluefruit.cpp ожидает LED_BLUE для conn LED */
#define LED_BLUE PIN_LED1
#define LED_STATE_ON 1

#define WIRE_INTERFACES_COUNT 2
#define PIN_WIRE_SDA (36)
#define PIN_WIRE_SCL (11)

/* UART1 (Serial1) — как у Adafruit Feather nRF52840; нужно для Uart.cpp ядра */
#define PIN_SERIAL1_RX (8)
#define PIN_SERIAL1_TX (6)

#define SPI_INTERFACES_COUNT 2
#define PIN_SPI_MISO (2)
#define PIN_SPI_MOSI (47)
#define PIN_SPI_SCK (43)
/* SPI1: требуется для глобального SPI1 в SPI.cpp ядра Adafruit nRF52 */
#define PIN_SPI1_MISO PIN_SPI_MISO
#define PIN_SPI1_MOSI PIN_SPI_MOSI
#define PIN_SPI1_SCK PIN_SPI_SCK
/** SdFat (зависимость TinyUSB 3.x) ожидает макрос SS для дефолтного CS SPI */
#ifndef SS
#define SS 0
#endif

#ifdef __cplusplus
}
#endif

#endif
