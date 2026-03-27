/**
 * Минимальный тест USB CDC — без mesh, FS, радио, BLE.
 * Сборка: pio run -e faketec_v5_smoke
 */

#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(300);
  yield();
  Serial.println();
  Serial.println("[SMOKE] FakeTech USB — если видите это, CDC и прошивка OK");
  Serial.flush();
}

void loop() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last >= 500) {
    last = now;
    Serial.print("[SMOKE] ");
    Serial.println(now);
    Serial.flush();
  }
  yield();
}
