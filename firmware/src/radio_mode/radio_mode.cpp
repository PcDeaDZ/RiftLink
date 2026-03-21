/**
 * Radio Mode Manager — переключение BLE ↔ WiFi.
 * Режимы взаимоисключающие: при переходе в WiFi BLE выключается сразу
 * (чтобы снизить пиковое потребление RAM и исключить OOM на handoff).
 */

#include "radio_mode.h"
#include "ble/ble.h"
#include "wifi/wifi.h"
#include "esp_now_slots/esp_now_slots.h"
#include "ws_server/ws_server.h"
#include "cmd_handler/cmd_handler.h"
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <Arduino.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace radio_mode {

static Mode s_mode = BLE;
static bool s_switching = false;
static WifiVariant s_wifiVariant = STA;

// Pending switch request (выполняется из update(), не из BLE callback)
static volatile bool s_pendingSwitch = false;
static Mode s_pendingTarget = BLE;
static WifiVariant s_pendingVariant = STA;
static char s_pendingSsid[64] = {0};
static char s_pendingPass[64] = {0};
static bool s_staConnectPending = false;
static uint32_t s_staConnectDeadlineMs = 0;
static volatile bool s_bleInitTaskDone = false;
static volatile bool s_bleInitTaskOk = false;
static bool s_bleInitInProgress = false;
static uint32_t s_bleInitStartedMs = 0;
static uint8_t s_bleInitRetryCount = 0;
static constexpr uint32_t BLE_INIT_TIMEOUT_MS = 12000;
static constexpr uint32_t BLE_INIT_TASK_STACK = 6144;

static void bleInitTask(void* arg) {
  (void)arg;
  bool ok = ble::init();
  s_bleInitTaskOk = ok;
  __sync_synchronize();
  s_bleInitTaskDone = true;
  vTaskDelete(nullptr);
}

static bool startBleInitTaskAsync(const char* reason) {
  s_bleInitTaskDone = false;
  s_bleInitTaskOk = false;
  BaseType_t tOk = xTaskCreate(bleInitTask, "bleInit", BLE_INIT_TASK_STACK, nullptr, 3, nullptr);
  if (tOk != pdPASS) {
    Serial.printf("[RadioMode] BLE init task create FAILED (%s), heap=%u\n",
        reason,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return false;
  }
  s_bleInitInProgress = true;
  s_bleInitStartedMs = millis();
  return true;
}

Mode current() { return s_mode; }
WifiVariant currentWifiVariant() { return s_wifiVariant; }
bool isSwitching() { return s_switching; }

static void doSwitchToWifi(WifiVariant variant, const char* ssid, const char* pass) {
  s_switching = true;
  Serial.printf("[BLE_CHAIN] stage=fw_mode_switch action=to_wifi_start mode=ble->wifi heap=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  Serial.printf("[RadioMode] BLE → WiFi (STA), heap before=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  // STA допускаем либо с SSID, либо с уже сохранёнными credentials.
  if (variant == STA && ((!ssid || !ssid[0]) && !wifi::hasCredentials())) {
    Serial.println("[RadioMode] STA requested without SSID and no saved credentials");
    s_switching = false;
    return;
  }

  // 1) Сразу гасим BLE, чтобы освободить RAM перед WiFi init.
  ble::deinit();

  // 2) Поднимаем WiFi.
  if (!wifi::init()) {
    Serial.println("[BLE_CHAIN] stage=fw_mode_switch action=to_wifi_fail reason=wifi_init");
    Serial.println("[RadioMode] WiFi init FAILED, restoring BLE");
    ble::init();
    s_switching = false;
    return;
  }

  // 3) STA flow
  if (ssid && ssid[0]) {
    wifi::setCredentials(ssid, pass);
  }
  if (!wifi::connect()) {
    Serial.println("[BLE_CHAIN] stage=fw_mode_switch action=to_wifi_fail reason=sta_connect_start");
    Serial.println("[RadioMode] STA connect start refused, restoring BLE");
    wifi::deinit();
    s_mode = WIFI;
    s_wifiVariant = STA;
    s_pendingTarget = BLE;
    s_pendingVariant = STA;
    s_pendingSsid[0] = '\0';
    s_pendingPass[0] = '\0';
    __sync_synchronize();
    s_pendingSwitch = true;
    s_switching = false;
    return;
  }
  // Неблокирующее ожидание подключения: loop/UI/кнопки продолжают работать.
  s_mode = WIFI;
  s_wifiVariant = STA;
  s_staConnectPending = true;
  s_staConnectDeadlineMs = millis() + 10000;
  Serial.printf("[BLE_CHAIN] stage=fw_mode_switch action=to_wifi_pending deadlineMs=%u\n",
      (unsigned)s_staConnectDeadlineMs);
  Serial.println("[RadioMode] STA connect started (non-blocking)");
}

static void doSwitchToBle() {
  if (s_bleInitInProgress) return;
  s_switching = true;
  s_staConnectPending = false;
  s_staConnectDeadlineMs = 0;
  s_bleInitRetryCount = 0;
  Serial.printf("[BLE_CHAIN] stage=fw_mode_switch action=to_ble_start mode=wifi->ble heap=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  Serial.printf("[RadioMode] WiFi → BLE, heap before=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  // 1. Stop WebSocket server
  ws_server::stop();

  // 2. Deinit ESP-NOW + WiFi
  esp_now_slots::deinit();
  wifi::deinit();
  // 3. Restart BLE через отдельную задачу с таймаутом (неблокирующе).
  // Основной loop не ждёт здесь, чтобы не "замораживать" кнопки/экран.
  if (!startBleInitTaskAsync("initial")) {
    if (s_bleInitRetryCount < 2) {
      s_bleInitRetryCount++;
      Serial.printf("[RadioMode] BLE init task create FAILED, retry %u/2\n", (unsigned)s_bleInitRetryCount);
      delay(140);
      if (startBleInitTaskAsync("retry")) {
        Serial.println("[RadioMode] BLE init started (retry)");
        return;
      }
    }
    Serial.println("[RadioMode] BLE init task create FAILED, try direct init");
    bool ok = ble::init();
    if (ok) {
      s_mode = BLE;
      s_wifiVariant = STA;
      s_switching = false;
      Serial.printf("[RadioMode] BLE init direct OK, heap=%u\n",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      return;
    }
    Serial.println("[RadioMode] BLE direct init FAILED, reboot");
    delay(120);
    esp_restart();
    return;
  }
  Serial.println("[RadioMode] BLE init started (non-blocking)");
}

bool switchTo(Mode target, WifiVariant variant, const char* ssid, const char* pass) {
  if (s_switching) return false;
  if (target == s_mode) return true;

  // Defer to update() — don't do heavy work from BLE callback context
  s_pendingTarget = target;
  s_pendingVariant = STA;
  if (ssid) { strncpy(s_pendingSsid, ssid, sizeof(s_pendingSsid) - 1); s_pendingSsid[63] = '\0'; }
  else s_pendingSsid[0] = '\0';
  if (pass) { strncpy(s_pendingPass, pass, sizeof(s_pendingPass) - 1); s_pendingPass[63] = '\0'; }
  else s_pendingPass[0] = '\0';
  __sync_synchronize();
  s_pendingSwitch = true;
  return true;
}

void update() {
  if (s_staConnectPending) {
    if (wifi::isConnected()) {
      char ip[24] = {0};
      char usedSsid[64] = {0};
      wifi::getStatus(usedSsid, sizeof(usedSsid), ip, sizeof(ip));
      esp_now_slots::init();
      ws_server::start();
      cmd_handler::init();
      s_mode = WIFI;
      s_wifiVariant = STA;
      s_staConnectPending = false;
      s_staConnectDeadlineMs = 0;
      s_switching = false;
      Serial.printf("[BLE_CHAIN] stage=fw_mode_switch action=to_wifi_ready ip=%s heap=%u\n",
          ip[0] ? ip : "0.0.0.0",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      Serial.printf("[RadioMode] Now in WiFi mode (STA), IP=%s, heap=%u\n",
          ip[0] ? ip : "0.0.0.0",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      return;
    }
    if ((int32_t)(millis() - s_staConnectDeadlineMs) >= 0) {
      Serial.println("[BLE_CHAIN] stage=fw_mode_switch action=to_wifi_timeout fallback=to_ble");
      Serial.println("[RadioMode] STA connect failed, switching back to BLE");
      s_staConnectPending = false;
      s_staConnectDeadlineMs = 0;
      esp_now_slots::deinit();
      wifi::deinit();
      s_mode = WIFI;
      s_wifiVariant = STA;
      s_pendingTarget = BLE;
      s_pendingVariant = STA;
      s_pendingSsid[0] = '\0';
      s_pendingPass[0] = '\0';
      __sync_synchronize();
      s_pendingSwitch = true;
      s_switching = false;
      return;
    }
    return;
  }

  if (s_bleInitInProgress) {
    if (s_bleInitTaskDone) {
      s_bleInitInProgress = false;
      if (!s_bleInitTaskOk) {
        if (s_bleInitRetryCount < 2) {
          s_bleInitRetryCount++;
          Serial.printf("[RadioMode] BLE re-init FAILED, retry %u/2\n", (unsigned)s_bleInitRetryCount);
          delay(160);
          if (startBleInitTaskAsync("reinit_failed_retry")) {
            return;
          }
        }
        Serial.println("[RadioMode] BLE re-init FAILED, try direct init");
        bool ok = ble::init();
        if (ok) {
          s_mode = BLE;
          s_wifiVariant = STA;
          s_switching = false;
          Serial.printf("[RadioMode] BLE init direct OK, heap=%u\n",
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
          return;
        }
        Serial.println("[RadioMode] BLE direct init FAILED, reboot");
        delay(120);
        esp_restart();
        return;
      }
      s_mode = BLE;
      s_wifiVariant = STA;
      s_switching = false;
      Serial.printf("[BLE_CHAIN] stage=fw_mode_switch action=to_ble_ready heap=%u\n",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      Serial.printf("[RadioMode] Now in BLE mode, heap=%u\n",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      return;
    }
    if ((int32_t)(millis() - s_bleInitStartedMs) >= BLE_INIT_TIMEOUT_MS) {
      s_bleInitInProgress = false;
      if (s_bleInitRetryCount < 2) {
        s_bleInitRetryCount++;
        Serial.printf("[RadioMode] BLE init timeout, retry %u/2\n", (unsigned)s_bleInitRetryCount);
        delay(180);
        if (startBleInitTaskAsync("timeout_retry")) {
          return;
        }
      }
      Serial.println("[RadioMode] BLE init timeout, try direct init");
      bool ok = ble::init();
      if (ok) {
        s_mode = BLE;
        s_wifiVariant = STA;
        s_switching = false;
        Serial.printf("[RadioMode] BLE init direct OK, heap=%u\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return;
      }
      Serial.println("[RadioMode] BLE init timeout, reboot");
      delay(120);
      esp_restart();
      return;
    }
    // Пока BLE поднимается, не блокируем loop.
    return;
  }

  if (!s_pendingSwitch) return;
  s_pendingSwitch = false;

  if (s_pendingTarget == WIFI && s_mode == BLE) {
    doSwitchToWifi(s_pendingVariant, s_pendingSsid, s_pendingPass);
  } else if (s_pendingTarget == BLE && s_mode == WIFI) {
    doSwitchToBle();
  }
}

}  // namespace radio_mode
