/**
 * Телеметрия nRF52840: OP_TELEMETRY broadcast.
 * - Батарея: FakeTech — без делителя readBatteryMv() = 0. Heltec T114 — AIN2 + ADC_CTRL (board_pins.h, Meshtastic variant).
 * - Heap: при отсутствии сильного xPortGetFreeHeapSize — nrf_sdh_get_free_heap_size() (куча под SoftDevice).
 * Документация: docs/API.md (nRF52840).
 */

#include "telemetry/telemetry.h"
#include "async_tasks.h"
#include "crypto/crypto.h"
#include "log.h"
#include "neighbors/neighbors.h"
#include "node/node.h"
#include "protocol/packet.h"

#if defined(RIFTLINK_BOARD_HELTEC_T114)
#include "board_pins.h"
#endif

#include <Arduino.h>
#include <stddef.h>
#include <string.h>

#if __has_include(<nrf_sdh.h>)
#include <nrf_sdh.h>
#define RIFTLINK_HAS_NRF_SDH 1
#else
#define RIFTLINK_HAS_NRF_SDH 0
#endif

static size_t nrfHeapFreeBytes() {
#if RIFTLINK_HAS_NRF_SDH
  return nrf_sdh_get_free_heap_size();
#else
  return 0U;
#endif
}

extern "C" __attribute__((weak)) size_t xPortGetFreeHeapSize(void) {
  return nrfHeapFreeBytes();
}

namespace telemetry {

#if defined(RIFTLINK_BOARD_HELTEC_T114)
static constexpr float kT114BattMult = 4.916f;
static bool s_t114AdcReady = false;

static void t114AdcEnsure() {
  if (s_t114AdcReady) return;
  pinMode(T114_ADC_CTRL_PIN, OUTPUT);
  digitalWrite(T114_ADC_CTRL_PIN, T114_ADC_CTRL_ON);
  analogReadResolution(12);
  s_t114AdcReady = true;
}
#endif

void init() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  t114AdcEnsure();
#endif
}

uint16_t readBatteryMv() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  t114AdcEnsure();
  int raw = analogRead(T114_BATT_ADC_PIN);
  if (raw <= 0) return 0;
  const float vAin = (static_cast<float>(raw) * 3.0f) / 4096.0f;
  const float vBat = vAin * kT114BattMult;
  return static_cast<uint16_t>(vBat * 1000.0f + 0.5f);
#else
  return 0;
#endif
}

bool isCharging() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  uint16_t mv = readBatteryMv();
  return mv > 4200;
#else
  return false;
#endif
}

/** Как `telemetry.cpp` (Heltec ESP): общая формула `batteryPercentFromMv`. */
int batteryPercent() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  return batteryPercentFromMv(readBatteryMv());
#else
  return -1;
#endif
}

void send() {
  uint16_t batMv = readBatteryMv();
  size_t heapFree = xPortGetFreeHeapSize();
  if (heapFree == 0) heapFree = nrfHeapFreeBytes();
  uint16_t heapKb = static_cast<uint16_t>(heapFree / 1024U);

  uint8_t plain[TELEM_PAYLOAD_LEN];
  memcpy(plain, &batMv, 2);
  memcpy(plain + 2, &heapKb, 2);

  uint8_t encBuf[32];
  size_t encLen = sizeof(encBuf);
  if (!crypto::encrypt(plain, TELEM_PAYLOAD_LEN, encBuf, &encLen)) return;

  uint8_t pkt[protocol::PAYLOAD_OFFSET + 32];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), protocol::BROADCAST_ID, 31,
      protocol::OP_TELEMETRY, encBuf, encLen, true, false, false);
  if (len == 0) return;
  uint8_t txSf = neighbors::rssiToSf(neighbors::getMinRssi());
  char reasonBuf[40];
  if (!queueTxPacket(pkt, len, txSf, false, TxRequestClass::data, reasonBuf, sizeof(reasonBuf))) {
    queueDeferredSend(pkt, len, txSf, 140, true);
    RIFTLINK_DIAG("TELEM", "event=TELEM_TX_DEFER cause=%s", reasonBuf[0] ? reasonBuf : "?");
  }
}

}  // namespace telemetry
