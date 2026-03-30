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
#include <nrf.h>
#if __has_include(<tusb.h>)
#include <tusb.h>
#define RIFTLINK_T114_TUSB 1
#else
#define RIFTLINK_T114_TUSB 0
#endif
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
/** EMA по mV: без сглаживания шум ADC даёт скачки % на границе шага. */
static uint16_t s_t114BattMvEma = 0;
/** EMA по линейному % (0…100 float), затем округление — плавнее, чем дискретные 31↔32 от целых шагов. */
static float s_t114PctSmooth = -1.f;
/** Доля нового замера в EMA по % (остальное — старое сглаженное). */
static constexpr float kT114PctSmoothAlpha = 0.08f;

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
  if (raw <= 0) {
    s_t114BattMvEma = 0;
    return 0;
  }
  const float vAin = (static_cast<float>(raw) * 3.0f) / 4096.0f;
  const float vBat = vAin * kT114BattMult;
  const uint16_t inst = static_cast<uint16_t>(vBat * 1000.0f + 0.5f);
  if (s_t114BattMvEma == 0) {
    s_t114BattMvEma = inst;
  } else {
    /* α=1/16 по mV — ещё плавнее у границы соседних процентов. */
    const uint32_t a = (uint32_t)s_t114BattMvEma;
    s_t114BattMvEma = static_cast<uint16_t>((15U * a + (uint32_t)inst + 8U) / 16U);
  }
  return s_t114BattMvEma;
#else
  return 0;
#endif
}

static float t114_linear_pct_from_mv(uint16_t mv) {
  if (mv < 2500) return -1.f;
  if (mv >= telemetry::kBatteryPctMvMax) return 100.f;
  if (mv <= telemetry::kBatteryPctMvMin) return 0.f;
  const float numer =
      (static_cast<float>(static_cast<int>(mv) - static_cast<int>(telemetry::kBatteryPctMvMin)) * 100.f) +
      (static_cast<float>(telemetry::kBatteryPctMvSpan) * 0.5f);
  float p = numer / static_cast<float>(telemetry::kBatteryPctMvSpan);
  if (p > 100.f) p = 100.f;
  if (p < 0.f) p = 0.f;
  return p;
}

bool isCharging() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  /* Без активной USB-сессии нельзя полагаться на VBUS: VBUSDETECT залипает в 1 после снятия кабеля.
   * Эвристика «mV между 3.2 и 4.12 В» при отсутствии сессии давала ложную молнию почти всегда
   * (нормальная АКБ в этом диапазоне). Иконка зарядки = только tud_connected/tud_mounted. */
  if ((NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) == 0U) return false;

#if RIFTLINK_T114_TUSB && defined(CFG_TUD_ENABLED) && CFG_TUD_ENABLED
  if (tud_inited()) {
    return tud_connected() || tud_mounted();
  }
#endif
  /* Нет device stack в этой сборке — по одному VBUS отличить зарядку от залипания нельзя. */
  return false;
#else
  return false;
#endif
}

/** Как `telemetry.cpp` (Heltec ESP): общая формула `batteryPercentFromMv`. */
int batteryPercent() {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  const uint16_t mv = readBatteryMv();
  if (mv == 0) {
    s_t114PctSmooth = -1.f;
    return -1;
  }
  const float pLin = t114_linear_pct_from_mv(mv);
  if (pLin < 0.f) {
    s_t114PctSmooth = -1.f;
    return -1;
  }
  if (s_t114PctSmooth < 0.f) {
    s_t114PctSmooth = pLin;
  } else {
    s_t114PctSmooth = (1.f - kT114PctSmoothAlpha) * s_t114PctSmooth + kT114PctSmoothAlpha * pLin;
  }
  int out = static_cast<int>(s_t114PctSmooth + 0.5f);
  if (out > 100) out = 100;
  if (out < 0) out = 0;
  return out;
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
