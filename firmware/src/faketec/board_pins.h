/**
 * Пины LoRa / I2C для nRF (логические 0..47 = P0.n / P1.(n-32)), Meshtastic-совместимая схема.
 * TCXO и DIO2 RF switch задаются в `platformio.ini` (`LORA_MODULE_HAS_TCXO`, `LORA_DIO2_RF_SWITCH`), не здесь.
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
/** Встроенный ST7789 135×240 (SPI1, не общий с LoRa SPI). Сверено с Meshtastic variants/nrf52840/heltec_mesh_node_t114/variant.h (ST7789_*). */
/** Питание панели TFT (VTFT_CTRL): без LOW на этом пине дисплей не получает питание / моргает. См. TFTDisplay.cpp DISPLAYON. */
#define TFT_VTFT_CTRL 3
#define TFT_VTFT_PWR_ON LOW
#define TFT_VTFT_PWR_OFF HIGH
#define TFT_SPI_CS 11
#define TFT_SPI_DC 12
#define TFT_SPI_RST 2
#define TFT_SPI_SCK 40
#define TFT_SPI_MOSI 41
/** MISO для SPI1 не используется чтением TFT — задан незадействованный GPIO (как «заглушка» для setPins). */
#define TFT_SPI_MISO 39
/** Подсветка: VTFT_LEDA в upstream Meshtastic, не путать с закомментированным ST7789_BL (32+6). */
#define TFT_BL 15
/** Включение подсветки (Meshtastic: TFT_BACKLIGHT_ON LOW). */
#define TFT_BL_ON LOW
/** Батарея: AIN2 на P0.04, делитель включается ADC_CTRL P0.06 HIGH — см. telemetry_nrf.cpp и Meshtastic BATTERY_PIN / ADC_CTRL. */
#define T114_BATT_ADC_PIN 4
#define T114_ADC_CTRL_PIN 6
#define T114_ADC_CTRL_ON HIGH
/** Meshtastic heltec_mesh_node_t114: PIN_LED1 = P1.3 (35), кнопка P1.10 (42), активный уровень LED — см. LED_STATE_ON в variant. */
#define T114_LED_PIN 35
#define T114_BUTTON_PIN 42
#define T114_LED_ON LOW
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
