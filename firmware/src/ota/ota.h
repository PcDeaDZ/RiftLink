/**
 * RiftLink OTA — Over-the-Air обновление прошивки
 * WiFi AP + ArduinoOTA. Запуск по BLE команде {"cmd":"ota"}
 */

#pragma once

namespace ota {

/** Запустить OTA режим: WiFi AP + ArduinoOTA */
void start();

/** Остановить OTA (если нужно) */
void stop();

/** Вызывать из loop() — обрабатывает OTA и yield */
void update();

/** OTA активен? */
bool isActive();

}  // namespace ota
