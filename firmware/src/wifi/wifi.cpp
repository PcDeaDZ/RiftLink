/**
 * RiftLink WiFi — STA, NVS
 */

#include "wifi.h"
#include <WiFi.h>
#include <cstring>
#include <nvs.h>
#include <nvs_flash.h>

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_SSID "wifi_ssid"
#define NVS_KEY_PASS "wifi_pass"

namespace wifi {

static bool s_inited = false;

void init() {
  if (s_inited) return;
  WiFi.mode(WIFI_OFF);
  s_inited = true;
}

bool setCredentials(const char* ssid, const char* pass) {
  if (!ssid || strlen(ssid) == 0) return false;

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;

  nvs_set_str(h, NVS_KEY_SSID, ssid);
  nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
  nvs_commit(h);
  nvs_close(h);
  return true;
}

void connect() {
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
  return WiFi.status() == WL_CONNECTED;
}

void getStatus(char* ssidOut, size_t ssidLen, char* ipOut, size_t ipLen) {
  if (ssidOut && ssidLen > 0) ssidOut[0] = '\0';
  if (ipOut && ipLen > 0) ipOut[0] = '\0';

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
  nvs_handle_t h;
  char ssid[8] = {0};
  size_t len = sizeof(ssid);

  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  bool ok = (nvs_get_str(h, NVS_KEY_SSID, ssid, &len) == ESP_OK && ssid[0] != '\0');
  nvs_close(h);
  return ok;
}

}  // namespace wifi
