/**
 * ESP-NOW Slot Negotiation — 50–250 м
 * WiFi STA без подключения к AP + ESP-NOW broadcast RTS.
 * Формат RTS совместим с BLS-N: company 0x524C, "RTS", from(4), to(4), len(2), txAt(4).
 */

#include "esp_now_slots.h"
#include "node/node.h"
#include <esp_wifi.h>
#include <esp_now.h>
#include <WiFi.h>
#include <Arduino.h>
#include <string.h>

#define RTS_COMPANY_ID 0x524C  // "RL" — RiftLink
#define RTS_CACHE_MAX 4
#define RTS_TTL_MS 3000
#define RTS_DEFER_OVERLAP_MS 500
#define ESPNOW_CHANNEL 6

namespace esp_now_slots {

struct RtsEntry {
  uint8_t from[4];
  uint8_t to[4];
  uint16_t len;
  uint32_t txAt;
  uint32_t receivedAt;
};

static RtsEntry s_rtsCache[RTS_CACHE_MAX];
static uint8_t s_rtsCount = 0;
static bool s_inited = false;
static bool s_ok = false;

static const uint8_t s_broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

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

static void recvCb(const uint8_t* mac, const uint8_t* data, int len) {
  (void)mac;
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

void init() {
  if (s_inited) return;
  s_inited = true;
  memset(s_rtsCache, 0, sizeof(s_rtsCache));
  s_rtsCount = 0;
  s_ok = false;

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  delay(200);  // дать WiFi время на запуск

  esp_err_t err = esp_now_init();
  if (err != ESP_OK) return;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, s_broadcastMac, 6);
  peer.channel = ESPNOW_CHANNEL;
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
