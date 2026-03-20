/**
 * RiftLink WiFi — STA, NVS
 */

#include "wifi.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <cstring>
#include <nvs.h>
#include <nvs_flash.h>

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_SSID "wifi_ssid"
#define NVS_KEY_PASS "wifi_pass"

namespace wifi {

static bool s_inited = false;
static bool s_available = false;

/** Меньше буферов, чем WIFI_INIT_CONFIG_DEFAULT — ниже требование к contiguous internal RAM
 *  (после тяжёлого бутa esp_wifi_init иначе даёт ESP_ERR_NO_MEM). ESP-NOW/STA остаются рабочими.
 *  OLED: init вызывается сразу после BLE — если всё ещё NO_MEM, ещё сильнее ужать (см. IDF min). */
static void applyLowFootprintWifiConfig(wifi_init_config_t* cfg) {
  cfg->static_rx_buf_num = 5;    // default 10
  cfg->dynamic_rx_buf_num = 10;  // default 32
  cfg->mgmt_sbuf_num = 10;       // default 32
}

bool init() {
  if (s_inited) return s_available;
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  applyLowFootprintWifiConfig(&cfg);
  esp_err_t err = esp_wifi_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[RiftLink] WiFi init failed: %s (0x%x) — OTA/ESP-NOW отключены\n",
        esp_err_to_name(err), (unsigned)err);
    s_available = false;
    return false;
  }
  s_inited = true;
  esp_wifi_set_mode(WIFI_MODE_NULL);
  s_available = true;
  return true;
}

bool isAvailable() {
  return s_available;
}

bool setCredentials(const char* ssid, const char* pass) {
  if (!s_available || !ssid || strlen(ssid) == 0) return false;

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;

  nvs_set_str(h, NVS_KEY_SSID, ssid);
  nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
  nvs_commit(h);
  nvs_close(h);
  return true;
}

void connect() {
  if (!s_available) return;
  nvs_handle_t h;
  char ssid[64] = {0};
  char pass[64] = {0};
  size_t len;

  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
  len = sizeof(ssid);
  if (nvs_get_str(h, NVS_KEY_SSID, ssid, &len) != ESP_OK) {
    nvs_close(h);
    return;
  }
  len = sizeof(pass);
  nvs_get_str(h, NVS_KEY_PASS, pass, &len);
  nvs_close(h);

  if (ssid[0] == '\0') return;

  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
  }
  WiFi.begin(ssid, pass[0] ? pass : nullptr);
}

void disconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool isConnected() {
  return s_available && WiFi.status() == WL_CONNECTED;
}

void getStatus(char* ssidOut, size_t ssidLen, char* ipOut, size_t ipLen) {
  if (ssidOut && ssidLen > 0) ssidOut[0] = '\0';
  if (ipOut && ipLen > 0) ipOut[0] = '\0';
  if (!s_available) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (ssidOut && ssidLen > 0) {
      strncpy(ssidOut, WiFi.SSID().c_str(), ssidLen - 1);
      ssidOut[ssidLen - 1] = '\0';
    }
    if (ipOut && ipLen > 0) {
      strncpy(ipOut, WiFi.localIP().toString().c_str(), ipLen - 1);
      ipOut[ipLen - 1] = '\0';
    }
  }
}

bool hasCredentials() {
  if (!s_available) return false;
  nvs_handle_t h;
  char ssid[8] = {0};
  size_t len = sizeof(ssid);

  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  bool ok = (nvs_get_str(h, NVS_KEY_SSID, ssid, &len) == ESP_OK && ssid[0] != '\0');
  nvs_close(h);
  return ok;
}

}  // namespace wifi
