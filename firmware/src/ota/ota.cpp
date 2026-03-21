/**
 * Legacy update stub.
 */

#include "ota.h"
#include <Arduino.h>

namespace ota {

bool start() {
  Serial.println("[OTA] Disabled in runtime; use BLE update");
  return false;
}

void stop() {
  // no-op
}

void update() {
  // no-op
}

bool isActive() {
  return false;
}

}  // namespace ota
