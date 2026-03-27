/**
 * FakeTech (fakeTec PCB) — распиновка SX1262 + OLED
 * NiceNano (nRF52840) + HT-RA62 / RA-01SH
 *
 * Номера: логический Arduino pin N = nRF GPIO N (0..31 = P0, 32..47 = P1).
 * Совпадает с Meshtastic variant **nrf52_promicro_diy_tcxo** — fakeTec основан на этом DIY
 * (см. https://github.com/gargomoma/fakeTec_pcb ).
 *
 * Ранее в board.h были другие числа (25/12/36/39/…) — на реальной fakeTec это не те линии → CHIP_NOT_FOUND (-2).
 */

#pragma once

// LoRa SX1262 — как в Meshtastic nrf52_promicro_diy_tcxo (SX126X_* / SPI)
#define LORA_NSS   45   // P1.13 CS
#define LORA_RST   9    // P0.09 NRST
#define LORA_DIO1  10   // P0.10 IRQ (не путать с P1.04 — там I2C SDA для OLED)
#define LORA_BUSY  29   // P0.29 BUSY
#define LORA_SCK   43   // P1.11 SCK
#define LORA_MISO  2    // P0.02 MISO
#define LORA_MOSI  47   // P1.15 MOSI

// I2C OLED (сторона платы fakeTec — как в promicro DIY)
#define OLED_SDA   36   // P1.04
#define OLED_SCL   11   // P0.11
#define OLED_ADDR  0x3C

// Кнопка (P1.00) — не совпадает с OLED_SCL
#define BUTTON_PIN 32

/** Питание 3.3 В периферии (P0.13). -1 = не дёргать. */
#define LORA_PWR_EN_PIN 13
/**
 * RXEN (часто выход FEM / линия на модуле SX1262). На fakeTec / Meshtastic nrf52_promicro_diy_tcxo — P0.17 (лог. пин 17).
 * Сборка без линии на MCU: `-DLORA_RXEN_DISABLED` или `-DLORA_RXEN_PIN=-1`.
 */
#if !defined(LORA_RXEN_DISABLED)
#ifndef LORA_RXEN_PIN
#define LORA_RXEN_PIN 17
#endif
#endif
/** Частота SPI к SX1262 */
#define LORA_SPI_HZ 1000000
/** 1 = TCXO (HT-RA62 / E22 с TCXO), 0 = кварц (RA-01SH и др.) */
#ifndef LORA_MODULE_HAS_TCXO
#define LORA_MODULE_HAS_TCXO 1
#endif
/**
 * У многих модулей на SX1262 (HT-RA62, E22) линия DIO2 заведена на RF switch (RX/TX).
 * Без setDio2AsRfSwitch(true) возможна картина «в эфир передаётся, с V3 слышно, обратно нет».
 * RA-01SH без внешнего switch — поставьте 0 в build_flags: -DLORA_DIO2_RF_SWITCH=0
 */
#ifndef LORA_DIO2_RF_SWITCH
#define LORA_DIO2_RF_SWITCH 1
#endif
