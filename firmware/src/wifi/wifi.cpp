/**
 * RiftLink WiFi — STA, NVS
 */

#include "wifi.h"
#include "esp_now_slots/esp_now_slots.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
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

static bool isEnterpriseAuth(wifi_auth_mode_t mode) {
#ifdef WIFI_AUTH_WPA2_ENTERPRISE
  if (mode == WIFI_AUTH_WPA2_ENTERPRISE) return true;
#endif
  return false;
}

static bool scanSsidAuth(const char* ssid, wifi_auth_mode_t* outAuth, int8_t* outRssi) {
  if (!ssid || !ssid[0]) return false;
  wifi_scan_config_t cfg = {};
  cfg.show_hidden = true;
  cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  cfg.scan_time.active.min = 80;
  cfg.scan_time.active.max = 180;
  if (esp_wifi_scan_start(&cfg, true) != ESP_OK) return false;
  uint16_t count = 20;
  wifi_ap_record_t aps[20];
  if (esp_wifi_scan_get_ap_records(&count, aps) != ESP_OK) return false;
  for (uint16_t i = 0; i < count; i++) {
    if (strcmp((const char*)aps[i].ssid, ssid) == 0) {
      if (outAuth) *outAuth = aps[i].authmode;
      if (outRssi) *outRssi = aps[i].rssi;
      return true;
    }
  }
  return false;
}

static const char* authToStr(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2_PSK";
#ifdef WIFI_AUTH_WPA3_PSK
    case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK";
#endif
#ifdef WIFI_AUTH_WPA2_WPA3_PSK
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
#endif
    default: return "UNKNOWN";
  }
}

/** Меньше буферов и отключение AMPDU — ниже contiguous internal RAM для esp_wifi_init.
 *  static_rx_buf_num=1 в IDF недопустимо (лог: static rx buf number 1 is out of range).
 *  При «actual is 1» — не хватает DMA на второй буфер; уменьшать стек NimBLE / extmem (план).
 *  mgmt_sbuf_num: в IDF минимум 6. */
static void applyLowFootprintWifiConfig(wifi_init_config_t* cfg) {
  cfg->static_rx_buf_num = 2;
  cfg->dynamic_rx_buf_num = 2;
  cfg->mgmt_sbuf_num = 6;
  cfg->ampdu_rx_enable = 0;
  cfg->ampdu_tx_enable = 0;
  cfg->amsdu_tx_enable = 0;
  cfg->rx_ba_win = 0;
  cfg->nvs_enable = 0;         // credentials через свой NVS; wifi-internal NVS не нужен
  cfg->cache_tx_buf_num = 0;
  if (cfg->rx_mgmt_buf_num > 2) cfg->rx_mgmt_buf_num = 2;
  cfg->espnow_max_encrypt_num = 2;
}

bool init() {
  if (s_inited) return s_available;
  Serial.printf("[BLE_CHAIN] stage=fw_wifi action=init_start free=%u largest=%u largest_dma=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  // Arduino-esp32 3.x: не писать Wi‑Fi креды в NVS при каждом begin — меньше сюрпризов с порядком init
  WiFi.persistent(false);
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  applyLowFootprintWifiConfig(&cfg);
  const size_t largestDma =
      heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  Serial.printf("[RiftLink] WiFi esp_wifi_init: largest_dma(internal)=%u (RX буферы Wi‑Fi — DMA)\n",
      (unsigned)largestDma);
  // 2 static RX буфера требуют contiguous DMA; при <8K часто «Expected to init 2 rx buffer, actual is 0/1»
  if (largestDma < 8192) {
    Serial.printf("[RiftLink] WiFi: предупреждение — largest_dma=%u < 8192, init может вернуть ESP_ERR_NO_MEM\n",
        (unsigned)largestDma);
  }
  esp_err_t err = esp_wifi_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[BLE_CHAIN] stage=fw_wifi action=init_fail err=0x%x\n", (unsigned)err);
    Serial.printf("[RiftLink] WiFi init failed: %s (0x%x) — OTA/ESP-NOW отключены "
        "(internal free=%u largest_int=%u largest_dma=%u)\n",
        esp_err_to_name(err), (unsigned)err,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    s_available = false;
    return false;
  }
  s_inited = true;
  esp_wifi_set_mode(WIFI_MODE_NULL);
  s_available = true;
  Serial.printf("[BLE_CHAIN] stage=fw_wifi action=init_ok free=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return true;
}

void deinit() {
  if (!s_inited) return;
  Serial.println("[WiFi] Deinit...");
  // Stop STA session and force radio off before low-level deinit.
  WiFi.disconnect(true, true);
  delay(40);
  WiFi.mode(WIFI_OFF);
  delay(40);
  esp_wifi_stop();
  esp_wifi_deinit();
  s_inited = false;
  s_available = false;
  Serial.printf("[WiFi] Deinit done, heap free=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

bool ensureInit() {
  if (s_available) return true;
  Serial.printf("[RiftLink] wifi::ensureInit — lazy init (heap before: free=%u largest=%u)\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  if (!init()) return false;
  esp_now_slots::init();
  Serial.printf("[RiftLink] wifi::ensureInit OK (heap after: free=%u)\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return true;
}

bool isAvailable() {
  return s_available;
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

bool getSavedSsid(char* out, size_t outLen) {
  if (!out || outLen == 0) return false;
  out[0] = '\0';
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  size_t len = outLen;
  const esp_err_t err = nvs_get_str(h, NVS_KEY_SSID, out, &len);
  nvs_close(h);
  return (err == ESP_OK && out[0] != '\0');
}

bool connect() {
  if (!s_available) return false;
  nvs_handle_t h;
  char ssid[64] = {0};
  char pass[64] = {0};
  size_t len;

  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  len = sizeof(ssid);
  if (nvs_get_str(h, NVS_KEY_SSID, ssid, &len) != ESP_OK) {
    nvs_close(h);
    return false;
  }
  len = sizeof(pass);
  nvs_get_str(h, NVS_KEY_PASS, pass, &len);
  nvs_close(h);

  if (ssid[0] == '\0') return false;
  Serial.printf("[BLE_CHAIN] stage=fw_wifi action=connect_start ssid_len=%u pass_len=%u\n",
      (unsigned)strlen(ssid), (unsigned)strlen(pass));

  if (WiFi.getMode() != WIFI_STA) {
    if (!WiFi.mode(WIFI_STA)) {
      Serial.println("[WiFi] WiFi.mode(WIFI_STA) failed");
      return false;
    }
  }
  WiFi.setSleep(false);
  delay(40);
  // Очистка предыдущей сессии уже после перевода в STA (иначе "STA not started").
  WiFi.disconnect(true, true);
  delay(80);
  esp_err_t stErr = esp_wifi_start();
  if (stErr != ESP_OK) {
    Serial.printf("[WiFi] esp_wifi_start before scan: %s (0x%x)\n", esp_err_to_name(stErr), (unsigned)stErr);
  }
  wifi_auth_mode_t auth = WIFI_AUTH_MAX;
  int8_t rssi = 0;
  const bool found = scanSsidAuth(ssid, &auth, &rssi);
  Serial.printf("[WiFi] STA target='%s', found=%s, auth=%s, rssi=%d, pass_len=%u\n",
      ssid,
      found ? "yes" : "no",
      found ? authToStr(auth) : "N/A",
      (int)rssi,
      (unsigned)strlen(pass));
  if (found && isEnterpriseAuth(auth)) {
    Serial.printf("[WiFi] SSID '%s' uses enterprise auth (unsupported), rssi=%d\n", ssid, (int)rssi);
    return false;
  }
#ifdef WIFI_AUTH_WPA2_PSK
  // Некоторые роутеры в mixed WPA2/WPA3 режиме на ESP32 стабильнее с минимумом WPA2.
  if (found && auth != WIFI_AUTH_OPEN) {
    WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
  }
#endif
  const bool openNet = found && auth == WIFI_AUTH_OPEN;
  wl_status_t st = WiFi.begin(ssid, openNet ? nullptr : (pass[0] ? pass : nullptr));
  if (st == WL_CONNECT_FAILED) {
    Serial.println("[BLE_CHAIN] stage=fw_wifi action=connect_fail reason=begin_failed");
    Serial.printf("[WiFi] WiFi.begin failed to start STA (ssid_len=%u)\n",
        (unsigned)strlen(ssid));
    return false;
  }
  Serial.println("[BLE_CHAIN] stage=fw_wifi action=connect_pending");
  return true;
}

void disconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool isConnected() {
  return s_available && WiFi.status() == WL_CONNECTED;
}

bool isStaConnecting() {
  if (!s_available) return false;
  if (WiFi.status() == WL_CONNECTED) return false;
  if (!hasCredentials()) return false;
  const wifi_mode_t mode = WiFi.getMode();
  if (mode != WIFI_STA && mode != WIFI_AP_STA) return false;
  const wl_status_t st = WiFi.status();
  if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) return false;
  return true;
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
  nvs_handle_t h;
  char ssid[8] = {0};
  size_t len = sizeof(ssid);

  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  bool ok = (nvs_get_str(h, NVS_KEY_SSID, ssid, &len) == ESP_OK && ssid[0] != '\0');
  nvs_close(h);
  return ok;
}

}  // namespace wifi
