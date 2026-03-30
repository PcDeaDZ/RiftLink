/**
 * nRF52832/nRF52840: один канал WDT, таймаут ~30 с @ 32768 Hz (CRV).
 * Если WDT уже запущен (загрузчик/ядро) — только кормим RR[0].
 */

#include "nrf_wdt_feed.h"

#include <Arduino.h>

#if defined(NRF52840_XXAA) || defined(NRF52832_XXAA)
#include <nrf.h>
#endif

#ifndef RIFTLINK_WDT_TIMEOUT_SEC
#define RIFTLINK_WDT_TIMEOUT_SEC 30u
#endif

static constexpr uint32_t kWdtReloadKey = 0x6E524635UL;

void riftlink_wdt_feed(void) {
#if defined(NRF52840_XXAA) || defined(NRF52832_XXAA)
  if (NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) {
    NRF_WDT->RR[0] = kWdtReloadKey;
  }
#endif
}

void riftlink_wdt_init(void) {
#if defined(NRF52840_XXAA) || defined(NRF52832_XXAA)
  if (NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) {
    riftlink_wdt_feed();
    return;
  }
  /* HALT/SLEEP: не останавливать WDT в sleep — иначе «сон» без feed съест таймаут; Pause = стоп счётчика в sleep. */
  NRF_WDT->CONFIG = (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos) | (WDT_CONFIG_SLEEP_Pause << WDT_CONFIG_SLEEP_Pos);
  const uint32_t crv = 32768u * RIFTLINK_WDT_TIMEOUT_SEC;
  if (crv >= 0xFFFFFFu) {
    NRF_WDT->CRV = 0xFFFFFFu;
  } else {
    NRF_WDT->CRV = crv;
  }
  NRF_WDT->RREN = 0x1u;
  NRF_WDT->TASKS_START = 1;
  riftlink_wdt_feed();
#endif
}
