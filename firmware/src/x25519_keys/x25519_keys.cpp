/**
 * X25519 — per-peer keys, E2E
 */

#include "x25519_keys.h"
#include "pkt_cache/pkt_cache.h"
#include "node/node.h"
#include "radio/radio.h"
#include "log.h"
#include <sodium.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <Arduino.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define MUTEX_TIMEOUT_MS 100

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_X25519_PUB "x25519_pub"
#define NVS_KEY_X25519_SEC "x25519_sec"

struct PeerKey {
  uint8_t peerId[protocol::NODE_ID_LEN];
  uint8_t sharedKey[32];
  uint32_t timestamp;
  bool used;
};

static uint8_t s_pubKey[X25519_PUBKEY_LEN];
static uint8_t s_secKey[crypto_box_SECRETKEYBYTES];
static PeerKey s_peers[X25519_MAX_PEERS];
static uint16_t s_pktIdCounter = 0;
static bool s_inited = false;
static SemaphoreHandle_t s_mutex = nullptr;

// Троттлинг: не спамить KEY_EXCHANGE одному пиру — иначе забивают канал, MSG не проходят
#define KEY_EXCHANGE_THROTTLE_MS 18000   // мин. интервал до повторной отправки тому же пиру
#define KEY_RESPONSE_THROTTLE_MS 60000  // ответ когда ключ уже был — макс. раз в 60с (пир может повторить)
#define KEY_DEBOUNCE_MS 1500             // мин. пауза между отправками одному пиру (HELLO+KEY_EXCHANGE подряд → 1 пакет)
struct ThrottleEntry { uint8_t peerId[protocol::NODE_ID_LEN]; uint32_t lastSend; };
static ThrottleEntry s_throttle[4];
static uint8_t s_throttleIdx = 0;

namespace x25519_keys {

void init() {
  if (s_inited) return;
  s_mutex = xSemaphoreCreateMutex();

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    size_t len = X25519_PUBKEY_LEN;
    if (nvs_get_blob(h, NVS_KEY_X25519_PUB, s_pubKey, &len) == ESP_OK &&
        nvs_get_blob(h, NVS_KEY_X25519_SEC, s_secKey, &len) == ESP_OK) {
      nvs_close(h);
      s_inited = true;
      memset(s_peers, 0, sizeof(s_peers));
      memset(s_throttle, 0, sizeof(s_throttle));
      return;
    }
    nvs_close(h);
  }

  crypto_box_keypair(s_pubKey, s_secKey);

  nvs_handle_t hw;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &hw) == ESP_OK) {
    nvs_set_blob(hw, NVS_KEY_X25519_PUB, s_pubKey, X25519_PUBKEY_LEN);
    nvs_set_blob(hw, NVS_KEY_X25519_SEC, s_secKey, crypto_box_SECRETKEYBYTES);
    nvs_commit(hw);
    nvs_close(hw);
  }
  memset(s_peers, 0, sizeof(s_peers));
  memset(s_throttle, 0, sizeof(s_throttle));
  s_inited = true;
}

bool getOurPublicKey(uint8_t* out) {
  if (!s_inited || !out) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;
  memcpy(out, s_pubKey, X25519_PUBKEY_LEN);
  xSemaphoreGive(s_mutex);
  return true;
}

static int findPeer(const uint8_t* peerId) {
  for (int i = 0; i < X25519_MAX_PEERS; i++) {
    if (s_peers[i].used && memcmp(s_peers[i].peerId, peerId, protocol::NODE_ID_LEN) == 0)
      return i;
  }
  return -1;
}

static int findFreeSlot() {
  for (int i = 0; i < X25519_MAX_PEERS; i++) {
    if (!s_peers[i].used) return i;
  }
  int oldest = 0;
  uint32_t oldestTs = s_peers[0].timestamp;
  for (int i = 1; i < X25519_MAX_PEERS; i++) {
    if (s_peers[i].timestamp < oldestTs) {
      oldestTs = s_peers[i].timestamp;
      oldest = i;
    }
  }
  return oldest;
}

void onKeyExchange(const uint8_t* peerId, const uint8_t* theirPubKey) {
  if (!s_inited || !peerId || !theirPubKey || node::isForMe(peerId) || node::isInvalidNodeId(peerId)) return;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return;

  int idx = findPeer(peerId);
  if (idx < 0) idx = findFreeSlot();

  if (crypto_box_beforenm(s_peers[idx].sharedKey, theirPubKey, s_secKey) != 0) return;

  memcpy(s_peers[idx].peerId, peerId, protocol::NODE_ID_LEN);
  s_peers[idx].timestamp = millis();
  s_peers[idx].used = true;
  xSemaphoreGive(s_mutex);
  RIFTLINK_LOG_EVENT("[RiftLink] X25519 key with %02X%02X\n", peerId[0], peerId[1]);
}

bool hasKeyFor(const uint8_t* peerId) {
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;
  bool ok = findPeer(peerId) >= 0;
  xSemaphoreGive(s_mutex);
  return ok;
}

bool getKeyFor(const uint8_t* peerId, uint8_t* keyOut) {
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;
  int idx = findPeer(peerId);
  bool ok = (idx >= 0 && keyOut);
  if (ok) {
    memcpy(keyOut, s_peers[idx].sharedKey, 32);
    s_peers[idx].timestamp = millis();  // активное использование — не вытеснять при eviction
  }
  xSemaphoreGive(s_mutex);
  return ok;
}

void sendKeyExchange(const uint8_t* peerId, bool useSf12, bool forceSend, bool hadKeyBefore) {
  if (!s_inited || !peerId || node::isForMe(peerId) || node::isBroadcast(peerId) || node::isInvalidNodeId(peerId)) return;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return;

  // Ключ уже есть и это не ответ на их KEY_EXCHANGE — не захламлять радио
  if (findPeer(peerId) >= 0 && !forceSend) {
    xSemaphoreGive(s_mutex);
    return;
  }

  // Debounce: HELLO и KEY_EXCHANGE приходят подряд — не слать два пакета, один уже в эфире
  uint32_t now = millis();
  for (int i = 0; i < 4; i++) {
    if (memcmp(s_throttle[i].peerId, peerId, protocol::NODE_ID_LEN) == 0 &&
        now - s_throttle[i].lastSend < KEY_DEBOUNCE_MS) {
      xSemaphoreGive(s_mutex);
      return;
    }
  }

  if (forceSend && !hadKeyBefore) {
    // Первый ответ — без длинного троттла, пир ждёт наш ключ
  } else {
    uint32_t throttleMs = hadKeyBefore ? KEY_RESPONSE_THROTTLE_MS : KEY_EXCHANGE_THROTTLE_MS;
    for (int i = 0; i < 4; i++) {
      if (memcmp(s_throttle[i].peerId, peerId, protocol::NODE_ID_LEN) == 0) {
        if (now - s_throttle[i].lastSend < throttleMs) {
          xSemaphoreGive(s_mutex);
          return;  // троттл — ключ уже был, не дрочить
        }
        break;
      }
    }
  }

  RIFTLINK_LOG_EVENT("[RiftLink] Sending KEY_EXCHANGE to %02X%02X\n", peerId[0], peerId[1]);

  // Записать в throttle
  memcpy(s_throttle[s_throttleIdx].peerId, peerId, protocol::NODE_ID_LEN);
  s_throttle[s_throttleIdx].lastSend = millis();
  s_throttleIdx = (s_throttleIdx + 1) % 4;

  uint16_t pktId = ++s_pktIdCounter;
  uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID + X25519_PUBKEY_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), peerId, 31, protocol::OP_KEY_EXCHANGE,
      s_pubKey, X25519_PUBKEY_LEN, false, false, false, protocol::CHANNEL_DEFAULT, pktId);
  uint8_t txSf = useSf12 ? 12 : 0;
  if (len > 0) {
    pkt_cache::add(peerId, pktId, pkt, len);
    radio::send(pkt, len, txSf, useSf12);
  }
  xSemaphoreGive(s_mutex);
}

}  // namespace x25519_keys
