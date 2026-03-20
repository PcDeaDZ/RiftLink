/**
 * Radio Mode Manager — переключение BLE ↔ WiFi.
 * Brief coexistence (~2-3 сек) при handoff: WiFi стартует до выключения BLE,
 * чтобы передать IP-адрес через BLE notify перед разрывом.
 */

#include "radio_mode.h"
#include "ble/ble.h"
#include "wifi/wifi.h"
#include "esp_now_slots/esp_now_slots.h"
#include "ota/ota.h"
#include "ws_server/ws_server.h"
#include "cmd_handler/cmd_handler.h"
#include <esp_heap_caps.h>
#include <Arduino.h>
#include <cstring>

namespace radio_mode {

static Mode s_mode = BLE;
static bool s_switching = false;

// Pending switch request (выполняется из update(), не из BLE callback)
static volatile bool s_pendingSwitch = false;
static Mode s_pendingTarget = BLE;
static WifiVariant s_pendingVariant = AP;
static char s_pendingSsid[64] = {0};
static char s_pendingPass[64] = {0};

Mode current() { return s_mode; }
bool isSwitching() { return s_switching; }

static void doSwitchToWifi(WifiVariant variant, const char* ssid, const char* pass) {
  s_switching = true;
  Serial.printf("[RadioMode] BLE → WiFi (%s), heap before=%u\n",
      variant == AP ? "AP" : "STA",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  // 1. Start WiFi while BLE still alive (brief coexistence for handoff)
  if (!wifi::init()) {
    Serial.println("[RadioMode] WiFi init FAILED, staying in BLE");
    s_switching = false;
    return;
  }

  // 2. ESP-NOW
  esp_now_slots::init();

  // 3. Start AP or connect STA
  char ip[24] = "192.168.4.1";
  char usedSsid[64] = {0};
  if (variant == STA && ssid && ssid[0]) {
    wifi::setCredentials(ssid, pass);
    wifi::connect();
    // Wait for connection (up to 10s)
    for (int i = 0; i < 100 && !wifi::isConnected(); i++) {
      delay(100);
    }
    if (wifi::isConnected()) {
      wifi::getStatus(usedSsid, sizeof(usedSsid), ip, sizeof(ip));
    } else {
      Serial.println("[RadioMode] STA connect failed, falling back to AP");
      variant = AP;
    }
  }
  if (variant == AP) {
    ota::start();
    strncpy(usedSsid, wifi::getApSsid(), sizeof(usedSsid) - 1);
    strncpy(ip, "192.168.4.1", sizeof(ip) - 1);
  }

  // 4. Start WebSocket server + wire command handler
  ws_server::start();
  cmd_handler::init();

  // 5. Send WiFi info to phone via BLE (still alive during coexistence)
  ble::notifyWifi(true, usedSsid, ip);
  delay(200);  // let BLE notify propagate

  // 6. Shutdown BLE
  ble::deinit();

  s_mode = WIFI;
  s_switching = false;
  Serial.printf("[RadioMode] Now in WiFi mode (%s), IP=%s, heap=%u\n",
      variant == AP ? "AP" : "STA", ip,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static void doSwitchToBle() {
  s_switching = true;
  Serial.printf("[RadioMode] WiFi → BLE, heap before=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  // 1. Stop WebSocket server
  ws_server::stop();

  // 2. Stop OTA if active
  if (ota::isActive()) ota::stop();

  // 3. Deinit ESP-NOW + WiFi
  esp_now_slots::deinit();
  wifi::deinit();

  // 4. Restart BLE
  if (!ble::init()) {
    Serial.println("[RadioMode] BLE re-init FAILED");
  }

  s_mode = BLE;
  s_switching = false;
  Serial.printf("[RadioMode] Now in BLE mode, heap=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

bool switchTo(Mode target, WifiVariant variant, const char* ssid, const char* pass) {
  if (s_switching) return false;
  if (target == s_mode) return true;

  // Defer to update() — don't do heavy work from BLE callback context
  s_pendingTarget = target;
  s_pendingVariant = variant;
  if (ssid) { strncpy(s_pendingSsid, ssid, sizeof(s_pendingSsid) - 1); s_pendingSsid[63] = '\0'; }
  else s_pendingSsid[0] = '\0';
  if (pass) { strncpy(s_pendingPass, pass, sizeof(s_pendingPass) - 1); s_pendingPass[63] = '\0'; }
  else s_pendingPass[0] = '\0';
  __sync_synchronize();
  s_pendingSwitch = true;
  return true;
}

void update() {
  if (!s_pendingSwitch) return;
  s_pendingSwitch = false;

  if (s_pendingTarget == WIFI && s_mode == BLE) {
    doSwitchToWifi(s_pendingVariant, s_pendingSsid, s_pendingPass);
  } else if (s_pendingTarget == BLE && s_mode == WIFI) {
    doSwitchToBle();
  }
}

}  // namespace radio_mode
