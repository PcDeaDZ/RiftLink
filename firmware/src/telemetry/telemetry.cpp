/**
 * RiftLink — телеметрия
 * Heltec V3/V4 (OLED): GPIO1 = ADC1_CH0 для батареи, GPIO37 = ADC_CTRL (HIGH = вкл. делитель)
 *   Делитель 390k+100k → коэффициент 4.9.  Meshtastic variant.h: BATTERY_PIN=1, ADC_CTRL=37
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

#define BAT_ADC_PIN      1      // GPIO1 (ADC1_CH0) — Heltec V3/V4 OLED
#define BAT_ADC_CTRL     37     // GPIO37: HIGH = включить делитель батареи
#define BAT_DIVIDER      4.9f   // резисторный делитель 390k / 100k

#if defined(USE_EINK)
#define PAPER_ADC_CTRL  19  // GPIO19: LOW = включить делитель батареи
#define PAPER_ADC_IN    20  // GPIO20: ADC2, напряжение после делителя ~50%
#endif

static bool s_inited = false;

namespace telemetry {

void init() {
#if defined(USE_EINK)
  pinMode(PAPER_ADC_CTRL, OUTPUT);
  digitalWrite(PAPER_ADC_CTRL, LOW);
#else
  pinMode(BAT_ADC_CTRL, OUTPUT);
  digitalWrite(BAT_ADC_CTRL, HIGH);   // включить делитель батареи (active HIGH)
  analogReadMilliVolts(BAT_ADC_PIN);  // инициализировать канал до установки аттенюации
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
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
  digitalWrite(BAT_ADC_CTRL, HIGH);
  delay(10);  // стабилизация делителя

  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogReadMilliVolts(BAT_ADC_PIN);
    delay(2);
  }
  uint32_t avgMv = sum / 8;
  if (avgMv < 50) return 0;  // нет батареи

  uint16_t batMv = (uint16_t)(avgMv * BAT_DIVIDER);
  Serial.printf("[bat] GPIO%d raw_avg=%lumV bat=%umV\n", BAT_ADC_PIN, avgMv, batMv);
  return batMv;
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
