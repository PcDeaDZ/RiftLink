/**
 * RiftLink WiFi — STA, NVS
 */

#include "wifi.h"
#include "esp_now_slots/esp_now_slots.h"
#include "node/node.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <cstring>
#include <nvs.h>
#include <nvs_flash.h>

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_SSID "wifi_ssid"
#define NVS_KEY_PASS "wifi_pass"
#define NVS_KEY_AP_PASS "ap_pass"

namespace wifi {

static bool s_inited = false;
static bool s_available = false;
static bool s_apCredsInited = false;
static char s_apSsid[16] = {0};
static char s_apPass[12] = {0};

static void generateRandomApPassword() {
  static const char CHARSET[] = "abcdefghjkmnpqrstuvwxyz23456789";
  for (int i = 0; i < 8; i++) {
    s_apPass[i] = CHARSET[esp_random() % (sizeof(CHARSET) - 1)];
  }
  s_apPass[8] = '\0';
}

static void ensureApCreds() {
  if (s_apCredsInited) return;
  s_apCredsInited = true;
  const uint8_t* id = node::getId();
  snprintf(s_apSsid, sizeof(s_apSsid), "RL-%02X%02X%02X%02X", id[0], id[1], id[2], id[3]);

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    char buf[12] = {0};
    size_t len = sizeof(buf);
    if (nvs_get_str(h, NVS_KEY_AP_PASS, buf, &len) == ESP_OK && buf[0]) {
      strncpy(s_apPass, buf, sizeof(s_apPass) - 1);
    } else {
      generateRandomApPassword();
      nvs_set_str(h, NVS_KEY_AP_PASS, s_apPass);
      nvs_commit(h);
    }
    nvs_close(h);
  } else {
    generateRandomApPassword();
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
  return true;
}

void deinit() {
  if (!s_inited) return;
  Serial.println("[WiFi] Deinit...");
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

const char* getApSsid() {
  ensureApCreds();
  return s_apSsid;
}

const char* getApPassword() {
  ensureApCreds();
  return s_apPass;
}

void regenerateApPassword() {
  ensureApCreds();
  generateRandomApPassword();
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_str(h, NVS_KEY_AP_PASS, s_apPass);
    nvs_commit(h);
    nvs_close(h);
  }
  Serial.printf("[WiFi] New AP password: %s\n", s_apPass);
}

}  // namespace wifi
