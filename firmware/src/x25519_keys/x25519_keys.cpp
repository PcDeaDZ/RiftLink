/**
 * X25519 — per-peer keys, E2E
 */

#include "x25519_keys.h"
#include "pkt_cache/pkt_cache.h"
#include "node/node.h"
#include "radio/radio.h"
#include "async_tasks.h"
#include "log.h"
#include <sodium.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <Arduino.h>
#include <string.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define MUTEX_TIMEOUT_MS 100
// Read-path ключей не должен часто фейлиться на кратком contention:
// при ACK/DELIVERY это даёт ложные "нет ключа"/undelivered, хотя ключ уже есть.
// Держим короткие попытки, но увеличиваем суммарное окно ожидания.
#define KEY_READ_RETRY_COUNT 8
#define KEY_READ_RETRY_TIMEOUT_MS 20

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_X25519_PUB "x25519_pub"
#define NVS_KEY_X25519_SEC "x25519_sec"

struct PeerKey {
  uint8_t peerId[protocol::NODE_ID_LEN];
  uint8_t peerPubKey[X25519_PUBKEY_LEN];
  uint8_t sharedKey[32];
  uint32_t timestamp;
  bool used;
};

struct PeerKeyCacheEntry {
  uint8_t peerId[protocol::NODE_ID_LEN];
  uint8_t sharedKey[32];
  bool used;
};

static uint8_t s_pubKey[X25519_PUBKEY_LEN];
static uint8_t s_secKey[crypto_box_SECRETKEYBYTES];
static PeerKey s_peers[X25519_MAX_PEERS];
static PeerKeyCacheEntry s_peerKeyCache[X25519_MAX_PEERS];
static std::atomic<uint32_t> s_peerKeyCacheSeq{0};  // even=stable, odd=writer in progress
static uint16_t s_pktIdCounter = 0;
static bool s_inited = false;
static SemaphoreHandle_t s_mutex = nullptr;

// Троттлинг: не спамить KEY_EXCHANGE — иначе забивают канал, MSG не проходят
#define KEY_EXCHANGE_THROTTLE_MS 8000    // первичный обмен без ключа: быстрее retry, чтобы не застрять в deadlock
#define KEY_RESPONSE_THROTTLE_MS 60000  // ответ когда ключ уже был — макс. раз в 60с (пир может повторить)
#define KEY_DEBOUNCE_MS 1500             // мин. пауза между отправками одному пиру (HELLO+KEY_EXCHANGE подряд → 1 пакет)
#define KEY_FORCE_MIN_GAP_MS 2500        // аварийный анти-шторм даже для forceSend(!hadKeyBefore)
#define KEY_HELLO_SLOT_BASE_MS 35
#define KEY_HELLO_SLOT_STEP_MS 110
#define KEY_HELLO_SLOT_JITTER_SPAN_MS 25
#define KEY_RESP_SLOT_BASE_MS 18
#define KEY_RESP_SLOT_STEP_MS 70
#define KEY_RESP_SLOT_JITTER_SPAN_MS 18
struct ThrottleEntry {
  std::atomic<uint32_t> idLo;
  std::atomic<uint32_t> idHi;
  std::atomic<uint32_t> lastSend;
};
static ThrottleEntry s_throttle[4];
static uint32_t s_lastKeyTxReadyMs = 0;
static std::atomic<uint32_t> s_throttleSeq{0};  // even=stable, odd=writer

/** Слот троттла по хэшу peerId — не round-robin: иначе 4 чужие TX затирают lastSend и дебаунс HELLO+KEY ломается */
static int throttleSlotForPeer(const uint8_t* peerId) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < protocol::NODE_ID_LEN; i++) {
    h ^= peerId[i];
    h *= 16777619u;
  }
  return (int)(h % 4);
}

static const char* safeReason(const char* reason) {
  return (reason && reason[0]) ? reason : "-";
}

static inline uint32_t idPartLo(const uint8_t* id) {
  uint32_t v = 0;
  memcpy(&v, id, sizeof(uint32_t));
  return v;
}

static inline uint32_t idPartHi(const uint8_t* id) {
  uint32_t v = 0;
  memcpy(&v, id + sizeof(uint32_t), sizeof(uint32_t));
  return v;
}

static bool throttleLastSendFor(const uint8_t* peerId, uint32_t* lastSendOut) {
  if (!peerId || !lastSendOut) return false;
  const uint32_t lo = idPartLo(peerId);
  const uint32_t hi = idPartHi(peerId);
  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t seqStart = s_throttleSeq.load(std::memory_order_acquire);
    if (seqStart & 1u) continue;
    bool found = false;
    uint32_t last = 0;
    for (int i = 0; i < 4; i++) {
      if (s_throttle[i].idLo.load(std::memory_order_relaxed) == lo &&
          s_throttle[i].idHi.load(std::memory_order_relaxed) == hi) {
        last = s_throttle[i].lastSend.load(std::memory_order_relaxed);
        found = true;
        break;
      }
    }
    uint32_t seqEnd = s_throttleSeq.load(std::memory_order_acquire);
    if (seqStart != seqEnd || (seqEnd & 1u)) continue;
    if (found) *lastSendOut = last;
    return found;
  }
  return false;
}

static void throttleUpdateFor(const uint8_t* peerId, uint32_t lastSend) {
  if (!peerId) return;
  int slot = throttleSlotForPeer(peerId);
  const uint32_t lo = idPartLo(peerId);
  const uint32_t hi = idPartHi(peerId);
  s_throttleSeq.fetch_add(1, std::memory_order_release);  // writer begin
  s_throttle[slot].idLo.store(lo, std::memory_order_relaxed);
  s_throttle[slot].idHi.store(hi, std::memory_order_relaxed);
  s_throttle[slot].lastSend.store(lastSend, std::memory_order_relaxed);
  s_throttleSeq.fetch_add(1, std::memory_order_release);  // writer end
}

static uint16_t idHash16(const uint8_t* id) {
  if (!id) return 0;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    h ^= id[i];
    h *= 16777619u;
  }
  return (uint16_t)((h >> 16) ^ (h & 0xFFFFu));
}

static uint32_t computeHelloSlotJitterMs(const uint8_t* peerId) {
  if (!peerId) return 0;
  const uint8_t* self = node::getId();
  uint16_t selfHash = idHash16(self);
  uint16_t peerHash = idHash16(peerId);
  uint32_t slot = (selfHash < peerHash) ? 0u : 1u;
  uint32_t salt = (uint32_t)(self[7] ^ peerId[7] ^ self[3] ^ peerId[3]);
  uint32_t jitter = salt % KEY_HELLO_SLOT_JITTER_SPAN_MS;
  return KEY_HELLO_SLOT_BASE_MS + (slot * KEY_HELLO_SLOT_STEP_MS) + jitter;
}

static uint32_t computeKeyResponseSlotJitterMs(const uint8_t* peerId) {
  if (!peerId) return 0;
  const uint8_t* self = node::getId();
  uint16_t selfHash = idHash16(self);
  uint16_t peerHash = idHash16(peerId);
  uint32_t slot = (selfHash < peerHash) ? 0u : 1u;
  uint32_t salt = (uint32_t)(self[6] ^ peerId[6] ^ self[1] ^ peerId[1]);
  uint32_t jitter = salt % KEY_RESP_SLOT_JITTER_SPAN_MS;
  return KEY_RESP_SLOT_BASE_MS + (slot * KEY_RESP_SLOT_STEP_MS) + jitter;
}

static bool takeKeyMutexWithRetry(const char* opTag, const uint8_t* peerId) {
  if (!s_mutex) return false;
  for (int i = 0; i < KEY_READ_RETRY_COUNT; i++) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(KEY_READ_RETRY_TIMEOUT_MS)) == pdTRUE) {
      return true;
    }
    if (i + 1 < KEY_READ_RETRY_COUNT) {
      RIFTLINK_DIAG("KEY", "event=KEY_BUSY_RETRY op=%s peer=%02X%02X attempt=%u",
          opTag ? opTag : "?", peerId ? peerId[0] : 0, peerId ? peerId[1] : 0, (unsigned)(i + 1));
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  RIFTLINK_DIAG("KEY", "event=KEY_BUSY_GIVEUP op=%s peer=%02X%02X",
      opTag ? opTag : "?", peerId ? peerId[0] : 0, peerId ? peerId[1] : 0);
  return false;
}

static void publishPeerKeyCacheLocked() {
  // Seqlock-style publication: readers accept cache only on stable even sequence.
  s_peerKeyCacheSeq.fetch_add(1, std::memory_order_release);  // writer begin (odd)
  for (int i = 0; i < X25519_MAX_PEERS; i++) {
    s_peerKeyCache[i].used = s_peers[i].used;
    if (!s_peers[i].used) continue;
    memcpy(s_peerKeyCache[i].peerId, s_peers[i].peerId, protocol::NODE_ID_LEN);
    memcpy(s_peerKeyCache[i].sharedKey, s_peers[i].sharedKey, 32);
  }
  s_peerKeyCacheSeq.fetch_add(1, std::memory_order_release);  // writer end (even)
}

static bool getKeyFromCache(const uint8_t* peerId, uint8_t* keyOut, bool* hasKeyOnly) {
  if (!peerId) return false;
  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t seqStart = s_peerKeyCacheSeq.load(std::memory_order_acquire);
    if (seqStart & 1u) continue;  // writer in progress
    bool found = false;
    int foundIdx = -1;
    for (int i = 0; i < X25519_MAX_PEERS; i++) {
      if (!s_peerKeyCache[i].used) continue;
      if (memcmp(s_peerKeyCache[i].peerId, peerId, protocol::NODE_ID_LEN) == 0) {
        found = true;
        foundIdx = i;
        break;
      }
    }
    uint32_t seqEnd = s_peerKeyCacheSeq.load(std::memory_order_acquire);
    if (seqStart != seqEnd || (seqEnd & 1u)) continue;  // unstable read, retry
    if (!found) return false;
    if (keyOut && foundIdx >= 0) {
      memcpy(keyOut, s_peerKeyCache[foundIdx].sharedKey, 32);
    }
    if (hasKeyOnly) *hasKeyOnly = true;
    return true;
  }
  return false;
}

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
      memset(s_peerKeyCache, 0, sizeof(s_peerKeyCache));
      publishPeerKeyCacheLocked();
      for (int i = 0; i < 4; i++) {
        s_throttle[i].idLo.store(0, std::memory_order_relaxed);
        s_throttle[i].idHi.store(0, std::memory_order_relaxed);
        s_throttle[i].lastSend.store(0, std::memory_order_relaxed);
      }
      s_throttleSeq.store(0, std::memory_order_relaxed);
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
  memset(s_peerKeyCache, 0, sizeof(s_peerKeyCache));
  publishPeerKeyCacheLocked();
  for (int i = 0; i < 4; i++) {
    s_throttle[i].idLo.store(0, std::memory_order_relaxed);
    s_throttle[i].idHi.store(0, std::memory_order_relaxed);
    s_throttle[i].lastSend.store(0, std::memory_order_relaxed);
  }
  s_throttleSeq.store(0, std::memory_order_relaxed);
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
  if (!s_inited) {
    RIFTLINK_DIAG("KEY", "event=KEY_STORE_FAIL cause=not_inited");
    RIFTLINK_LOG_ERR("[RiftLink] X25519 onKeyExchange skip: keys not inited\n");
    return;
  }
  if (!peerId || !theirPubKey) {
    RIFTLINK_DIAG("KEY", "event=KEY_STORE_FAIL cause=null_input");
    RIFTLINK_LOG_ERR("[RiftLink] X25519 onKeyExchange skip: null peer/payload\n");
    return;
  }
  if (node::isForMe(peerId)) {
    RIFTLINK_DIAG("KEY", "event=KEY_STORE_SKIP cause=self_echo");
    return;  // эхо своего KEY — без шума
  }
  if (node::isInvalidNodeId(peerId)) {
    RIFTLINK_DIAG("KEY", "event=KEY_STORE_FAIL cause=invalid_node peer=%02X%02X", peerId[0], peerId[1]);
    RIFTLINK_LOG_ERR("[RiftLink] X25519 onKeyExchange skip: invalid node id\n");
    return;
  }
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
    RIFTLINK_DIAG("KEY", "event=KEY_STORE_FAIL cause=mutex_timeout peer=%02X%02X", peerId[0], peerId[1]);
    RIFTLINK_LOG_ERR("[RiftLink] X25519 onKeyExchange: mutex timeout peer=%02X%02X\n",
        peerId[0], peerId[1]);
    return;
  }

  int idx = findPeer(peerId);
  if (idx < 0) idx = findFreeSlot();

  if (crypto_box_beforenm(s_peers[idx].sharedKey, theirPubKey, s_secKey) != 0) {
    xSemaphoreGive(s_mutex);
    RIFTLINK_DIAG("KEY", "event=KEY_STORE_FAIL cause=beforenm_failed peer=%02X%02X pub0=%02X pub1=%02X",
        peerId[0], peerId[1], theirPubKey[0], theirPubKey[1]);
    // Частая причина «нет строки X25519 key»: мусор в 32 B после обрезанного KEY в эфире (см. NACK/retransmit)
    RIFTLINK_LOG_ERR("[RiftLink] X25519 crypto_box_beforenm FAILED peer=%02X%02X pub32=%02X%02X...\n",
        peerId[0], peerId[1], theirPubKey[0], theirPubKey[1]);
    return;
  }

  memcpy(s_peers[idx].peerId, peerId, protocol::NODE_ID_LEN);
  memcpy(s_peers[idx].peerPubKey, theirPubKey, X25519_PUBKEY_LEN);
  s_peers[idx].timestamp = millis();
  s_peers[idx].used = true;
  publishPeerKeyCacheLocked();
  xSemaphoreGive(s_mutex);
  RIFTLINK_DIAG("KEY", "event=KEY_STORE_OK peer=%02X%02X slot=%d peers_max=%d",
      peerId[0], peerId[1], idx, X25519_MAX_PEERS);
  RIFTLINK_LOG_EVENT("[RiftLink] X25519 key with %02X%02X\n", peerId[0], peerId[1]);
}

bool isPeerPubKeyMismatch(const uint8_t* peerId, const uint8_t* theirPubKey) {
  if (!peerId || !theirPubKey || !s_mutex) return false;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;
  int idx = findPeer(peerId);
  bool mismatch = false;
  if (idx >= 0 && s_peers[idx].used) {
    mismatch = memcmp(s_peers[idx].peerPubKey, theirPubKey, X25519_PUBKEY_LEN) != 0;
  }
  xSemaphoreGive(s_mutex);
  return mismatch;
}

bool hasKeyFor(const uint8_t* peerId) {
  if (!peerId) return false;
  bool hasKey = false;
  if (getKeyFromCache(peerId, nullptr, &hasKey)) return hasKey;
  // Fallback path only when cache snapshot was unstable.
  if (!takeKeyMutexWithRetry("hasKeyFor", peerId)) return false;
  bool ok = findPeer(peerId) >= 0;
  xSemaphoreGive(s_mutex);
  return ok;
}

bool getKeyFor(const uint8_t* peerId, uint8_t* keyOut) {
  if (!peerId || !keyOut) return false;
  if (getKeyFromCache(peerId, keyOut, nullptr)) return true;
  // Fallback path only when cache snapshot was unstable.
  if (!takeKeyMutexWithRetry("getKeyFor", peerId)) return false;
  int idx = findPeer(peerId);
  bool ok = (idx >= 0);
  if (ok) memcpy(keyOut, s_peers[idx].sharedKey, 32);
  xSemaphoreGive(s_mutex);
  return ok;
}

void sendKeyExchange(const uint8_t* peerId, bool forceSend, bool hadKeyBefore, const char* reason) {
  if (!s_inited) {
    RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=not_inited");
    return;
  }
  if (!peerId) {
    RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=null_peer reason=%s", safeReason(reason));
    return;
  }
  if (node::isForMe(peerId)) {
    RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=self_peer reason=%s", safeReason(reason));
    return;
  }
  if (node::isBroadcast(peerId)) {
    RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=broadcast_peer reason=%s", safeReason(reason));
    return;
  }
  if (node::isInvalidNodeId(peerId)) {
    RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=invalid_peer peer=%02X%02X reason=%s",
        peerId[0], peerId[1], safeReason(reason));
    return;
  }
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
    RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=mutex_timeout peer=%02X%02X reason=%s",
        peerId[0], peerId[1], safeReason(reason));
    return;
  }
  RIFTLINK_DIAG("KEY", "event=KEY_TX_ATTEMPT peer=%02X%02X reason=%s force=%u had=%u",
      peerId[0], peerId[1], safeReason(reason), (unsigned)forceSend, (unsigned)hadKeyBefore);

  // Ключ уже есть и это не ответ на их KEY_EXCHANGE — не захламлять радио
  if (findPeer(peerId) >= 0 && !forceSend) {
    xSemaphoreGive(s_mutex);
    RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=already_has_key peer=%02X%02X reason=%s",
        peerId[0], peerId[1], safeReason(reason));
    return;
  }

  const bool bypassDebounce = forceSend && !hadKeyBefore;
  // Debounce: HELLO и KEY_EXCHANGE приходят подряд — не слать два пакета, один уже в эфире.
  // Для force-ответа на свежий KEY_EXCHANGE debounce отключаем, иначе второй узел может не получить наш ключ.
  uint32_t now = millis();
  if (forceSend && !hadKeyBefore) {
    uint32_t lastSend = 0;
    if (throttleLastSendFor(peerId, &lastSend) && (now - lastSend < KEY_FORCE_MIN_GAP_MS)) {
      xSemaphoreGive(s_mutex);
      RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=force_min_gap peer=%02X%02X delta_ms=%lu min_gap_ms=%lu reason=%s",
          peerId[0], peerId[1], (unsigned long)(now - lastSend),
          (unsigned long)KEY_FORCE_MIN_GAP_MS, safeReason(reason));
      return;
    }
  }
  if (!bypassDebounce) {
    uint32_t lastSend = 0;
    if (throttleLastSendFor(peerId, &lastSend) && (now - lastSend < KEY_DEBOUNCE_MS)) {
      xSemaphoreGive(s_mutex);
      RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=debounce peer=%02X%02X delta_ms=%lu reason=%s",
          peerId[0], peerId[1], (unsigned long)(now - lastSend), safeReason(reason));
      return;
    }
  } else {
    RIFTLINK_DIAG("KEY", "event=KEY_TX_FORCE_RESPONSE peer=%02X%02X reason=%s",
        peerId[0], peerId[1], safeReason(reason));
  }

  if (forceSend && !hadKeyBefore) {
    // Первый ответ — без длинного троттла, пир ждёт наш ключ
  } else {
    // key_rx_dup: peer can lose pairwise state while keeping same pubkey.
    // Use medium throttle to heal asymmetry faster without enabling KEY storm.
    bool isDupRecovery = forceSend && hadKeyBefore && reason && strcmp(reason, "key_rx_dup") == 0;
    uint32_t throttleMs = isDupRecovery ? KEY_EXCHANGE_THROTTLE_MS
                                        : (hadKeyBefore ? KEY_RESPONSE_THROTTLE_MS : KEY_EXCHANGE_THROTTLE_MS);
    uint32_t lastSend = 0;
    if (throttleLastSendFor(peerId, &lastSend) && (now - lastSend < throttleMs)) {
      xSemaphoreGive(s_mutex);
      RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=throttle peer=%02X%02X delta_ms=%lu throttle_ms=%lu reason=%s",
          peerId[0], peerId[1], (unsigned long)(now - lastSend),
          (unsigned long)throttleMs, safeReason(reason));
      return;  // троттл — ключ уже был, не дрочить
    }
  }

  // Записать в throttle и снять копию ключа под мьютексом; radio::send() — сеть/очереди/другие мьютексы — только после Give.
  throttleUpdateFor(peerId, millis());
  uint16_t pktId = ++s_pktIdCounter;
  uint8_t pubCopy[X25519_PUBKEY_LEN];
  memcpy(pubCopy, s_pubKey, X25519_PUBKEY_LEN);
  xSemaphoreGive(s_mutex);

  if (reason && reason[0]) {
    RIFTLINK_LOG_EVENT("[RiftLink] Sending KEY_EXCHANGE (%s) to %02X%02X\n", reason, peerId[0], peerId[1]);
  } else {
    RIFTLINK_LOG_EVENT("[RiftLink] Sending KEY_EXCHANGE to %02X%02X\n", peerId[0], peerId[1]);
  }
  uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID + X25519_PUBKEY_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), peerId, 31, protocol::OP_KEY_EXCHANGE,
      pubCopy, X25519_PUBKEY_LEN, false, false, false, protocol::CHANNEL_DEFAULT, pktId);
  // Всегда текущий mesh SF (txSf=0) — без отдельного SF12 для KEY при фиксированном SF в настройках.
  if (len > 0) {
    pkt_cache::add(peerId, pktId, pkt, len);
    // Для первичного handshake (до готового ключа) всегда используем асимметрию по shortId,
    // чтобы hello/retry не уходили одновременно на двух узлах.
    bool primaryPath = (!forceSend && !hadKeyBefore);
    bool keyRxResponse = (forceSend && reason && strcmp(reason, "key_rx") == 0);
    if (primaryPath) {
      uint32_t jitterMs = computeHelloSlotJitterMs(peerId);
      queueDeferredSend(pkt, len, 0, jitterMs);
      s_lastKeyTxReadyMs = millis();
      RIFTLINK_DIAG("KEY", "event=KEY_TX_DEFER peer=%02X%02X pktId=%u len=%u delay_ms=%lu reason=%s slot_mode=%s",
          peerId[0], peerId[1], (unsigned)pktId, (unsigned)len, (unsigned long)jitterMs,
          safeReason(reason), "shortid_primary");
    } else if (keyRxResponse) {
      uint32_t jitterMs = computeKeyResponseSlotJitterMs(peerId);
      queueDeferredSend(pkt, len, 0, jitterMs);
      s_lastKeyTxReadyMs = millis();
      RIFTLINK_DIAG("KEY", "event=KEY_TX_DEFER peer=%02X%02X pktId=%u len=%u delay_ms=%lu reason=%s slot_mode=%s",
          peerId[0], peerId[1], (unsigned)pktId, (unsigned)len, (unsigned long)jitterMs,
          safeReason(reason), "shortid_keyrx");
    } else {
      char reasonBuf[40];
      RIFTLINK_DIAG("KEY", "event=KEY_TX_READY peer=%02X%02X pktId=%u len=%u txSf=mesh priority=1",
          peerId[0], peerId[1], (unsigned)pktId, (unsigned)len);
      // priority: не теряться за ACK/очередью — иначе «Sending KEY_EXCHANGE» без фактического TX и залипания.
      if (!queueTxPacket(pkt, len, 0, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
        RIFTLINK_DIAG("KEY", "event=KEY_TX_NOT_QUEUED peer=%02X%02X pktId=%u cause=%s",
            peerId[0], peerId[1], (unsigned)pktId, reasonBuf[0] ? reasonBuf : "?");
        RIFTLINK_LOG_ERR("[RiftLink] KEY_EXCHANGE tx NOT queued (%s) to %02X%02X pktId=%u\n",
            reasonBuf[0] ? reasonBuf : "?", peerId[0], peerId[1], (unsigned)pktId);
      } else {
        s_lastKeyTxReadyMs = millis();
      }
    }
  } else {
    RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=build_packet_failed peer=%02X%02X reason=%s",
        peerId[0], peerId[1], safeReason(reason));
  }
}

uint32_t getLastKeyTxReadyMs() {
  return s_lastKeyTxReadyMs;
}

}  // namespace x25519_keys
