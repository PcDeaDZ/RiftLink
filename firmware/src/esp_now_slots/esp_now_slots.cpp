/**
 * ESP-NOW Slot Negotiation — 50–250 м
 * WiFi STA без подключения к AP + ESP-NOW broadcast RTS.
 * Формат RTS совместим с BLS-N: company 0x524C, "RTS", from(4), to(4), len(2), txAt(4).
 */

#include "esp_now_slots.h"

#if defined(RIFTLINK_DISABLE_ESP_NOW)
// Заглушки в esp_now_slots.h — без esp_now.h / esp_wifi в этом TU.
#else

#include "node/node.h"
#include <esp_wifi.h>
#include <esp_now.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <Arduino.h>
#include <string.h>

#define RTS_COMPANY_ID 0x524C  // "RL" — RiftLink
#define RTS_CACHE_MAX 4
#define RTS_TTL_MS 3000
#define RTS_DEFER_OVERLAP_MS 500
#define ESPNOW_CHANNEL_DEFAULT 6
#define ESPNOW_ADAPTIVE_CHANNELS 3   // 1, 6, 11 — неперекрывающиеся 2.4 GHz
#define ESPNOW_ADAPTIVE_INTERVAL_MS 300000  // re-scan раз в 5 мин
#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_ESPNOW_CH "espnow_ch"
#define NVS_KEY_ESPNOW_ADAPTIVE "espnow_adapt"

namespace esp_now_slots {

struct RtsEntry {
  uint8_t from[4];
  uint8_t to[4];
  uint16_t len;
  uint32_t txAt;
  uint32_t receivedAt;
};

static const uint8_t s_adaptiveChannels[ESPNOW_ADAPTIVE_CHANNELS] = {1, 6, 11};

static RtsEntry s_rtsCache[RTS_CACHE_MAX];
static uint8_t s_rtsCount = 0;
static bool s_inited = false;
static bool s_ok = false;
static bool s_adaptive = false;
static uint8_t s_channel = ESPNOW_CHANNEL_DEFAULT;
static uint32_t s_lastAdaptiveScan = 0;

static const uint8_t s_broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static bool loadAdaptive() {
  nvs_handle h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  int8_t v;
  esp_err_t err = nvs_get_i8(h, NVS_KEY_ESPNOW_ADAPTIVE, &v);
  nvs_close(h);
  return (err == ESP_OK && v != 0);
}

static bool saveAdaptive(bool on) {
  nvs_handle h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_i8(h, NVS_KEY_ESPNOW_ADAPTIVE, on ? 1 : 0);
  nvs_commit(h);
  nvs_close(h);
  return (err == ESP_OK);
}

static uint8_t loadChannel() {
  nvs_handle h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return ESPNOW_CHANNEL_DEFAULT;
  int8_t ch;
  esp_err_t err = nvs_get_i8(h, NVS_KEY_ESPNOW_CH, &ch);
  nvs_close(h);
  if (err == ESP_OK && ch >= 1 && ch <= 13) return (uint8_t)ch;
  return ESPNOW_CHANNEL_DEFAULT;
}

static bool saveChannel(uint8_t ch) {
  nvs_handle h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_i8(h, NVS_KEY_ESPNOW_CH, (int8_t)ch);
  nvs_commit(h);
  nvs_close(h);
  return (err == ESP_OK);
}

/** Сканировать каналы 1, 6, 11. Вернуть канал с наименьшей загрузкой (min max RSSI). */
static uint8_t scanAndPickBestChannel() {
  wifi_scan_config_t cfg = {};
  cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  cfg.scan_time = {.active = {.min = 100, .max = 300}};
  if (esp_wifi_scan_start(&cfg, true) != ESP_OK) return ESPNOW_CHANNEL_DEFAULT;

  uint16_t count = 20;
  wifi_ap_record_t ap[20];
  esp_err_t err = esp_wifi_scan_get_ap_records(&count, ap);
  esp_wifi_scan_stop();
  if (err != ESP_OK || count == 0) return ESPNOW_CHANNEL_DEFAULT;

  int8_t maxRssi[ESPNOW_ADAPTIVE_CHANNELS];
  for (int i = 0; i < ESPNOW_ADAPTIVE_CHANNELS; i++) maxRssi[i] = -127;

  for (uint16_t i = 0; i < count; i++) {
    uint8_t ch = ap[i].primary;
    for (int j = 0; j < ESPNOW_ADAPTIVE_CHANNELS; j++) {
      if (ch == s_adaptiveChannels[j]) {
        if (ap[i].rssi > maxRssi[j]) maxRssi[j] = ap[i].rssi;
        break;
      }
    }
  }

  int bestIdx = 0;
  int8_t bestRssi = maxRssi[0];
  for (int i = 1; i < ESPNOW_ADAPTIVE_CHANNELS; i++) {
    if (maxRssi[i] < bestRssi) {
      bestRssi = maxRssi[i];
      bestIdx = i;
    }
  }
  return s_adaptiveChannels[bestIdx];
}

static void pruneRtsCache() {
  uint32_t now = millis();
  uint8_t w = 0;
  for (uint8_t r = 0; r < s_rtsCount; r++) {
    if (now - s_rtsCache[r].receivedAt < RTS_TTL_MS) {
      if (w != r) s_rtsCache[w] = s_rtsCache[r];
      w++;
    }
  }
  s_rtsCount = w;
}

static void recvCb(const esp_now_recv_info_t* recv_info, const uint8_t* data, int len) {
  (void)recv_info;
  if (!data || len < 19) return;
  if (data[0] != (RTS_COMPANY_ID & 0xFF) || data[1] != ((RTS_COMPANY_ID >> 8) & 0xFF)) return;
  if (data[2] != 0x52 || data[3] != 0x54 || data[4] != 0x53) return;  // "RTS"

  uint8_t from[4], to[4];
  uint16_t plen;
  uint32_t txAt;
  memcpy(from, data + 5, 4);
  memcpy(to, data + 9, 4);
  plen = (uint16_t)data[13] | ((uint16_t)data[14] << 8);
  txAt = (uint32_t)data[15] | ((uint32_t)data[16] << 8) | ((uint32_t)data[17] << 16) | ((uint32_t)data[18] << 24);

  addReceivedRts(from, to, plen, txAt);
}

uint8_t getChannel() {
  return s_channel;
}

bool isAdaptive() {
  return s_adaptive;
}

bool setAdaptive(bool on) {
  if (!saveAdaptive(on)) return false;
  s_adaptive = on;
  return true;
}

void tickAdaptive() {
  if (!s_adaptive || !s_inited) return;
  uint32_t now = millis();
  if (now - s_lastAdaptiveScan < ESPNOW_ADAPTIVE_INTERVAL_MS) return;
  s_lastAdaptiveScan = now;

  uint8_t best = scanAndPickBestChannel();
  if (best != s_channel && best >= 1 && best <= 13) {
    setChannel(best);
    Serial.printf("[RiftLink] ESP-NOW adaptive: channel %u\n", (unsigned)best);
  }
}

bool setChannel(uint8_t ch) {
  if (ch < 1 || ch > 13) return false;
  if (!saveChannel(ch)) return false;
  s_channel = ch;
  if (s_inited && s_ok) {
    esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
    esp_now_del_peer(s_broadcastMac);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, s_broadcastMac, 6);
    peer.channel = s_channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) s_ok = false;
  }
  return true;
}

void deinit() {
  if (!s_inited) return;
  Serial.println("[ESP-NOW] Deinit...");
  esp_now_unregister_recv_cb();
  esp_now_del_peer(s_broadcastMac);
  esp_now_deinit();
  s_ok = false;
  s_inited = false;
  memset(s_rtsCache, 0, sizeof(s_rtsCache));
  s_rtsCount = 0;
  Serial.printf("[ESP-NOW] Deinit done, heap free=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void init() {
  if (s_inited) return;
  s_inited = true;
  memset(s_rtsCache, 0, sizeof(s_rtsCache));
  s_rtsCount = 0;
  s_ok = false;
  s_adaptive = loadAdaptive();

  // esp_wifi API вместо WiFi.mode() — единообразие с wifi::init(), меньше обвязки Arduino
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) return;
  err = esp_wifi_start();
  if (err != ESP_OK) return;
  delay(200);  // дать WiFi время на запуск

  if (s_adaptive) {
    s_channel = scanAndPickBestChannel();
    saveChannel(s_channel);
    s_lastAdaptiveScan = millis();
    Serial.printf("[RiftLink] ESP-NOW adaptive init: channel %u\n", (unsigned)s_channel);
  } else {
    s_channel = loadChannel();
  }
  esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);

  err = esp_now_init();
  if (err != ESP_OK) return;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, s_broadcastMac, 6);
  peer.channel = s_channel;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  err = esp_now_add_peer(&peer);
  if (err != ESP_OK) {
    esp_now_deinit();
    return;
  }

  err = esp_now_register_recv_cb(recvCb);
  if (err != ESP_OK) {
    esp_now_del_peer(s_broadcastMac);
    esp_now_deinit();
    return;
  }

  s_ok = true;
}

void addReceivedRts(const uint8_t* from, const uint8_t* to, uint16_t len, uint32_t txAt) {
  if (s_rtsCount >= RTS_CACHE_MAX) {
    memmove(&s_rtsCache[0], &s_rtsCache[1], (RTS_CACHE_MAX - 1) * sizeof(RtsEntry));
    s_rtsCount = RTS_CACHE_MAX - 1;
  }
  RtsEntry* e = &s_rtsCache[s_rtsCount];
  memcpy(e->from, from, 4);
  memcpy(e->to, to, 4);
  e->len = len;
  e->txAt = txAt;
  e->receivedAt = millis();
  s_rtsCount++;
}

bool sendRtsBeforeLora(const uint8_t* to, size_t payloadLen) {
  if (!s_ok || !to || node::isBroadcast(to)) return false;

  const uint8_t* from = node::getId();
  uint32_t txAt = millis() + 60;

  uint8_t buf[19];
  buf[0] = RTS_COMPANY_ID & 0xFF;
  buf[1] = (RTS_COMPANY_ID >> 8) & 0xFF;
  buf[2] = 0x52;
  buf[3] = 0x54;
  buf[4] = 0x53;
  memcpy(buf + 5, from, 4);
  memcpy(buf + 9, to, 4);
  buf[13] = (uint8_t)(payloadLen & 0xFF);
  buf[14] = (uint8_t)((payloadLen >> 8) & 0xFF);
  buf[15] = (uint8_t)(txAt & 0xFF);
  buf[16] = (uint8_t)((txAt >> 8) & 0xFF);
  buf[17] = (uint8_t)((txAt >> 16) & 0xFF);
  buf[18] = (uint8_t)((txAt >> 24) & 0xFF);

  esp_err_t err = esp_now_send(s_broadcastMac, buf, sizeof(buf));
  return (err == ESP_OK);
}

bool shouldDeferTx(const uint8_t* to) {
  if (!s_ok) return false;
  pruneRtsCache();
  uint32_t now = millis();
  for (uint8_t i = 0; i < s_rtsCount; i++) {
    const RtsEntry* e = &s_rtsCache[i];
    if (now - e->receivedAt > RTS_TTL_MS) continue;
    if (now >= (uint32_t)(e->txAt - 50) && now <= e->txAt + RTS_DEFER_OVERLAP_MS)
      return true;
  }
  return false;
}

}  // namespace esp_now_slots

#endif /* !RIFTLINK_DISABLE_ESP_NOW */
