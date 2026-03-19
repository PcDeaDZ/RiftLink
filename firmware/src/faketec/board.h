/**
 * FakeTech V5 Rev B — распиновка
 * NiceNano (nRF52840) + HT-RA62 / RA-01SH-P (SX1262)
 *
 * ВАЖНО: Проверьте схему вашей платы! Распиновка может отличаться.
 * Источник: Meshtastic nRF52 DIY, FakeTech HackMD
 */

#pragma once

// LoRa SX1262 (HT-RA62 / RA-01SH) — SPI
// Arduino pin numbers для Adafruit Feather nRF52840
// Для NiceNano на FakeTech PCB — возможно нужна корректировка
#define LORA_NSS   25
#define LORA_RST   12
#define LORA_DIO1  36
#define LORA_BUSY  39
#define LORA_SCK   15
#define LORA_MISO  16
#define LORA_MOSI  2

// I2C для OLED (опционально, автоопределение)
#define OLED_SDA   17
#define OLED_SCL   20
#define OLED_ADDR  0x3C

// Кнопка RST (если есть)
#define BUTTON_PIN 11
