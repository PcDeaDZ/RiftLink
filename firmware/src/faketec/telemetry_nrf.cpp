/**
 * Лёгкая телеметрия для FakeTech: batMv + heapKb в OP_TELEMETRY (как Heltec V3).
 */

#include "telemetry_nrf.h"
#include "async_tx.h"
#include "crypto.h"
#include "heap_metrics.h"
#include "neighbors.h"
#include "node.h"
#include "protocol/packet.h"
#include "log.h"
#include <Arduino.h>
#include <string.h>

namespace telemetry_nrf {

static constexpr size_t kPayloadLen = 4;
static constexpr uint32_t kIntervalMs = 120000;
static uint32_t s_lastMs = 0;

void init() { s_lastMs = millis(); }

void tick() {
  const uint32_t now = millis();
  if ((int32_t)(now - s_lastMs) < (int32_t)kIntervalMs) return;
  s_lastMs = now;

  uint16_t batMv = 0;
  uint16_t heapKb = (uint16_t)(heap_metrics_free_bytes() / 1024u);
  uint8_t plain[kPayloadLen];
  memcpy(plain, &batMv, 2);
  memcpy(plain + 2, &heapKb, 2);

  uint8_t encBuf[32];
  size_t encLen = sizeof(encBuf);
  if (!crypto::encrypt(plain, kPayloadLen, encBuf, &encLen)) return;

  uint8_t pkt[protocol::PAYLOAD_OFFSET + 32];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_TELEMETRY,
      encBuf, encLen, true, false, false);
  if (len == 0) return;

  uint8_t txSf = neighbors::rssiToSf(neighbors::getMinRssi());
  char reasonBuf[40];
  if (!queueTxPacket(pkt, len, txSf, false, TxRequestClass::data, reasonBuf, sizeof(reasonBuf))) {
    RIFTLINK_DIAG("TELEM", "event=TELEM_TX_SKIP cause=%s", reasonBuf[0] ? reasonBuf : "?");
  }
}

}  // namespace telemetry_nrf
