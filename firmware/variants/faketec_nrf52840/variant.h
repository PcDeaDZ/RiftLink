#ifndef _VARIANT_FAKETEC_NRF52840_
#define _VARIANT_FAKETEC_NRF52840_

/** Master clock frequency */
#define VARIANT_MCK (64000000ul)

#define USE_LFXO

#include "WVariant.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 48 логических пина: индекс == номер GPIO Nordic (P0.0..P0.31, P1.0..P1.15) */
#define PINS_COUNT (48)
#define NUM_DIGITAL_PINS (48)
#define NUM_ANALOG_INPUTS (8)
#define NUM_ANALOG_OUTPUTS (0)

#define WIRE_INTERFACES_COUNT 1
/* Совпадает с board.h FakeTech (I2C OLED) */
#define PIN_WIRE_SDA (17)
#define PIN_WIRE_SCL (20)

#define SPI_INTERFACES_COUNT 1
/* Дефолт SPI — совпадает с board.h после SPI.setPins в radio.cpp */
#define PIN_SPI_MISO (16)
#define PIN_SPI_MOSI (2)
#define PIN_SPI_SCK (15)

static const uint8_t SS = (25);
static const uint8_t MOSI = PIN_SPI_MOSI;
static const uint8_t MISO = PIN_SPI_MISO;
static const uint8_t SCK = PIN_SPI_SCK;

#define PIN_SERIAL1_RX (24)
#define PIN_SERIAL1_TX (25)

/* Светодиоды NiceNano/Feather-подобные — не совпадают с линиями LoRa в board.h */
#define PIN_LED1 (42)
#define PIN_LED2 (43)
#define LED_BUILTIN PIN_LED1
#define LED_BLUE PIN_LED1
#define LED_RED PIN_LED2
#define LED_STATE_ON 1

#define PIN_A0 (4)
#define PIN_A1 (5)
#define PIN_A2 (30)
#define PIN_A3 (28)
#define PIN_A4 (2)
#define PIN_A5 (3)
#define PIN_A6 (29)
#define PIN_A7 (31)

static const uint8_t A0 = PIN_A0;
static const uint8_t A1 = PIN_A1;
static const uint8_t A2 = PIN_A2;
static const uint8_t A3 = PIN_A3;
static const uint8_t A4 = PIN_A4;
static const uint8_t A5 = PIN_A5;
static const uint8_t A6 = PIN_A6;
static const uint8_t A7 = PIN_A7;
#define ADC_RESOLUTION 14

#ifdef __cplusplus
}
#endif

#endif
