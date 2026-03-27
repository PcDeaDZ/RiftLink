/**
 * Аппаратный watchdog nRF52 (NRF_WDT): при зависании MCU — сброс.
 * Отключить: -DFAKETEC_NRF_WDT_ENABLED=0
 * Таймаут (мс): -DFAKETEC_WDT_TIMEOUT_MS=90000 (по умолчанию 90 с)
 */
#pragma once

#include <stdint.h>

#ifndef FAKETEC_NRF_WDT_ENABLED
#define FAKETEC_NRF_WDT_ENABLED 1
#endif

#ifndef FAKETEC_WDT_TIMEOUT_MS
#define FAKETEC_WDT_TIMEOUT_MS 90000u
#endif

#if FAKETEC_NRF_WDT_ENABLED
void faketec_wdt_begin(uint32_t timeout_ms);
void faketec_wdt_feed(void);
#else
static inline void faketec_wdt_begin(uint32_t) {}
static inline void faketec_wdt_feed(void) {}
#endif
