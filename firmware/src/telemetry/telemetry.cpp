/**
 * RiftLink — телеметрия
 * Heltec V3 (OLED): GPIO7 = ADC1_CH6 для батареи (divider ~4.01)
 * Heltec V3 Paper: GPIO7 = E-Ink BUSY. Батарея на GPIO19/20 (ADC2, divider ~50%).
 */

#include "telemetry.h"
#include "neighbors/neighbors.h"
#include "node/node.h"
#include "radio/radio.h"
#include "crypto/crypto.h"
#include "protocol/packet.h"
#include <Arduino.h>
#include <string.h>

#define BAT_ADC_PIN     7   // GPIO7 (Heltec V3 OLED)
#define BAT_DIVIDER     4.01f

#if defined(USE_EINK)
#define PAPER_ADC_CTRL  19  // GPIO19: LOW = включить делитель батареи
#define PAPER_ADC_IN    20  // GPIO20: ADC2, напряжение после делителя ~50%
#endif

static bool s_inited = false;

namespace telemetry {

void init() {
#if defined(USE_EINK)
  pinMode(PAPER_ADC_CTRL, OUTPUT);
  digitalWrite(PAPER_ADC_CTRL, LOW);  // включить цепь измерения батареи
#endif
  s_inited = true;
}

uint16_t readBatteryMv() {
  if (!s_inited) return 0;

#if defined(USE_EINK)
  // Paper: GPIO19=LOW (включить), GPIO20=ADC2. Делитель ~50% → умножить на 2.
  uint32_t sum = 0;
  int n = 0;
  for (int i = 0; i < 8; i++) {
    int mv = analogReadMilliVolts(PAPER_ADC_IN);
    if (mv > 0 && mv < 2500) {  // 0–2.5V после делителя (0–5V батарея)
      sum += mv;
      n++;
    }
    delay(2);
  }
  if (n < 4) return 0;
  uint32_t avg = sum / n;
  return (uint16_t)(avg * 2);  // делитель ~50%
#else
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
#endif
}

bool isCharging() {
  uint16_t mv = readBatteryMv();
  return mv > 4200;
}

int batteryPercent() {
  uint16_t mv = readBatteryMv();
  if (mv < 2500) return -1;
  if (mv >= 4200) return 100;
  if (mv <= 3000) return 0;
  return (int)((mv - 3000) / 12);
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

  uint8_t pkt[protocol::PAYLOAD_OFFSET + 32];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_TELEMETRY,
      encBuf, encLen, true, false, false);
  if (len > 0) radio::send(pkt, len, neighbors::rssiToSf(neighbors::getMinRssi()));
}

}  // namespace telemetry
