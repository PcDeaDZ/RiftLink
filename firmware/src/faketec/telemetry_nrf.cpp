/**
 * Телеметрия nRF52840: OP_TELEMETRY broadcast (батарея 0 без ADC; heap — FreeRTOS при наличии).
 */

#include "telemetry/telemetry.h"
#include "async_tasks.h"
#include "crypto/crypto.h"
#include "log.h"
#include "neighbors/neighbors.h"
#include "node/node.h"
#include "protocol/packet.h"

#include <Arduino.h>
#include <stddef.h>
#include <string.h>

// heap_3 на Adafruit nRF52 часто не экспортирует xPortGetFreeHeapSize; слабый stub даёт 0 до появления сильного символа.
extern "C" __attribute__((weak)) size_t xPortGetFreeHeapSize(void) {
  return 0U;
}

namespace telemetry {

void init() {}

uint16_t readBatteryMv() {
  return 0;
}

bool isCharging() {
  return false;
}

int batteryPercent() {
  return -1;
}

void send() {
  uint16_t batMv = readBatteryMv();
  uint16_t heapKb = (uint16_t)(xPortGetFreeHeapSize() / 1024U);

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
