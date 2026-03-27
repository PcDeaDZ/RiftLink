/**
 * nRF52840 WDT: кормление в конце loop() — при реальном зависании (SPI/бесконечный цикл) будет reset.
 */
#include "nrf_wdt.h"

#if FAKETEC_NRF_WDT_ENABLED

#include <Arduino.h>

#if defined(NRF_WDT)

#ifndef WDT_RR_RR_Reload
#define WDT_RR_RR_Reload (0x6E524635UL)
#endif

static bool s_wdt_armed = false;

void faketec_wdt_begin(uint32_t timeout_ms) {
  if (timeout_ms < 2000u) timeout_ms = 2000u;
  if (timeout_ms > 120000u) timeout_ms = 120000u;
  /* timeout [s] ≈ (CRV + 1) / 32768 → CRV ≈ timeout_ms * 32768 / 1000 */
  uint64_t crv64 = ((uint64_t)timeout_ms * 32768ULL) / 1000ULL;
  if (crv64 > 0xFFFFFFULL) crv64 = 0xFFFFFFULL;
  if (crv64 < 15ULL) crv64 = 15ULL;
  uint32_t crv = (uint32_t)crv64;

  /* 0: WDT считается и в sleep (delay); при отладке с halt можно поставить HALT_Pause */
  NRF_WDT->CONFIG = 0;
  NRF_WDT->CRV = crv;
  NRF_WDT->RREN = 0x1u; /* RR[0] */
  NRF_WDT->TASKS_START = 1;
  s_wdt_armed = true;
  faketec_wdt_feed();
}

void faketec_wdt_feed(void) {
  if (!s_wdt_armed) return;
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;
}

#else

void faketec_wdt_begin(uint32_t) {}
void faketec_wdt_feed(void) {}

#endif /* NRF_WDT */

#endif /* FAKETEC_NRF_WDT_ENABLED */
