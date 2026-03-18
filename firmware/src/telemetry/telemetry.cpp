/**
 * RiftLink — телеметрия
 * Heltec V3: GPIO7 = ADC1_CH6 для батареи (divider ~4.01)
 */

#include "telemetry.h"
#include "neighbors/neighbors.h"
#include "node/node.h"
#include "radio/radio.h"
#include "crypto/crypto.h"
#include "protocol/packet.h"
#include <Arduino.h>
#include <string.h>

#define BAT_ADC_PIN     7   // GPIO7 (Heltec V3)
#define BAT_DIVIDER     4.01f

static bool s_inited = false;

namespace telemetry {

void init() {
  s_inited = true;
}

uint16_t readBatteryMv() {
  if (!s_inited) return 0;

  uint32_t sum = 0;
  for (int i = 0; i < 4; i++) {
    sum += analogRead(BAT_ADC_PIN);
    delay(2);
  }
  int raw = sum / 4;
  if (raw < 10) return 0;  // Нет батареи или не подключено

  // 12-bit ADC, 3.3V ref, divider 4.01
  float v = (float)raw / 4096.0f * 3.3f * BAT_DIVIDER;
  return (uint16_t)(v * 1000.0f);
}

void send() {
  uint16_t batMv = readBatteryMv();
  uint16_t heapKb = (uint16_t)(ESP.getFreeHeap() / 1024);

  uint8_t plain[TELEM_PAYLOAD_LEN];
  memcpy(plain, &batMv, 2);
  memcpy(plain + 2, &heapKb, 2);

  uint8_t encBuf[32];
  size_t encLen = sizeof(encBuf);
  if (!crypto::encrypt(plain, TELEM_PAYLOAD_LEN, encBuf, &encLen)) return;

  uint8_t pkt[protocol::HEADER_LEN + 32];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_TELEMETRY,
      encBuf, encLen, true, false, false);
  if (len > 0) radio::send(pkt, len, neighbors::rssiToSf(neighbors::getMinRssi()));
}

}  // namespace telemetry
