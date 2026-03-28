/**
 * Пины LoRa / I2C для nRF (логические 0..47 = P0.n / P1.(n-32)), Meshtastic-совместимая схема.
 */
#pragma once

#if defined(RIFTLINK_BOARD_HELTEC_T114)
// Heltec Mesh Node T114 — SX1262 SPI0 (Meshtastic: variants/nrf52840/heltec_mesh_node_t114/variant.h)
// LoRa: CS24, DIO1 20, BUSY17, RST25; SPI MISO23 MOSI22 SCK19 — совпадает с upstream.
// I2C: ниже — шина 0 (P0.26/P0.27), как PIN_WIRE_SDA/SCL в Meshtastic. На разъёме расширения часто Wire1: SDA16 / SCL13.
// Встроенный TFT — ST7789 по SPI1 (NSS11, DC12, MOSI41, SCK40, RST2…), не эти пины. Общий variant faketec_nrf52840 задаёт PIN_WIRE 36/11 (ProMicro) — для T114 I2C-OLED вызывайте Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL).
#define LORA_NSS 24
#define LORA_RST 25
#define LORA_DIO1 20
#define LORA_BUSY 17
#define LORA_SCK 19
#define LORA_MISO 23
#define LORA_MOSI 22
#define LORA_RXEN 255
#define OLED_I2C_ADDR 0x3C
#define PIN_I2C_SDA 26
#define PIN_I2C_SCL 27
#else
// FakeTech V5 / ProMicro DIY + HT-RA62
#define LORA_NSS 45
#define LORA_RST 9
#define LORA_DIO1 10
#define LORA_BUSY 29
#define LORA_SCK 43
#define LORA_MISO 2
#define LORA_MOSI 47
#define LORA_RXEN 17
#define OLED_I2C_ADDR 0x3C
#define PIN_I2C_SDA 36
#define PIN_I2C_SCL 11
#endif
