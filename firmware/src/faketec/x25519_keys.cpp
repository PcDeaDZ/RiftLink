/**
 * X25519 / KEY_EXCHANGE — упрощённый паритет с ESP (без pkt_cache, без очереди TX).
 */

#include "x25519_keys.h"
#include "node.h"
#include "protocol/packet.h"
#include "radio.h"
#include "storage.h"
#include "log.h"

extern "C" {
#include "sodium/crypto_scalarmult_curve25519.h"
#include "sodium/crypto_core_hsalsa20.h"
#include "sodium/randombytes.h"
}

#include <Arduino.h>
#include <string.h>

#define STORAGE_X25519_PUB "x25519_pub"
#define STORAGE_X25519_SEC "x25519_sec"

#define KEY_DEBOUNCE_MS 1500
#define KEY_EXCHANGE_THROTTLE_MS 8000
#define KEY_RESPONSE_THROTTLE_MS 60000
#define KEY_FORCE_MIN_GAP_MS 2500

static bool isHandshakeKeyOfferReason(const char* reason) {
  if (!reason) return false;
  return strcmp(reason, "hello") == 0 || strcmp(reason, "hello_fwd") == 0 || strcmp(reason, "retry") == 0;
}

struct PeerKey {
  uint8_t peerId[protocol::NODE_ID_LEN];
  uint8_t peerPubKey[X25519_PUBKEY_LEN];
  uint8_t sharedKey[32];
  uint32_t timestamp;
  bool used;
};

static uint8_t s_pubKey[X25519_PUBKEY_LEN];
static uint8_t s_secKey[32];
static PeerKey s_peers[X25519_MAX_PEERS];
static uint16_t s_pktIdCounter = 0;
static bool s_inited = false;
static uint32_t s_lastKeyTxReadyMs = 0;

struct ThrottleSlot {
  uint32_t idLo;
  uint32_t idHi;
  uint32_t lastSend;
  bool inUse;
};
static ThrottleSlot s_throttle[4];

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

static int throttleSlotForPeer(const uint8_t* peerId) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < protocol::NODE_ID_LEN; i++) {
    h ^= peerId[i];
    h *= 16777619u;
  }
  return (int)(h % 4);
}

static bool throttleLastSendFor(const uint8_t* peerId, uint32_t* lastSendOut) {
  if (!peerId || !lastSendOut) return false;
  const uint32_t lo = idPartLo(peerId);
  const uint32_t hi = idPartHi(peerId);
  for (int i = 0; i < 4; i++) {
    if (s_throttle[i].inUse && s_throttle[i].idLo == lo && s_throttle[i].idHi == hi) {
      *lastSendOut = s_throttle[i].lastSend;
      return true;
    }
  }
  return false;
}

static void throttleUpdateFor(const uint8_t* peerId, uint32_t lastSend) {
  if (!peerId) return;
  int slot = throttleSlotForPeer(peerId);
  s_throttle[slot].idLo = idPartLo(peerId);
  s_throttle[slot].idHi = idPartHi(peerId);
  s_throttle[slot].lastSend = lastSend;
  s_throttle[slot].inUse = true;
}

static int rl_box_beforenm(uint8_t* k, const uint8_t* pk, const uint8_t* sk) {
  static const unsigned char zero[16] = {0};
  unsigned char s[32];
  if (crypto_scalarmult_curve25519(s, sk, pk) != 0) return -1;
  return crypto_core_hsalsa20(k, zero, s, NULL);
}

static int findPeer(const uint8_t* peerId) {
  for (int i = 0; i < X25519_MAX_PEERS; i++) {
    if (s_peers[i].used && memcmp(s_peers[i].peerId, peerId, protocol::NODE_ID_LEN) == 0) return i;
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

namespace x25519_keys {

void init() {
  if (s_inited) return;

  size_t plen = X25519_PUBKEY_LEN;
  size_t slen = sizeof(s_secKey);
  if (storage::getBlob(STORAGE_X25519_PUB, s_pubKey, &plen) && plen == X25519_PUBKEY_LEN &&
      storage::getBlob(STORAGE_X25519_SEC, s_secKey, &slen) && slen == sizeof(s_secKey)) {
    memset(s_peers, 0, sizeof(s_peers));
    memset(s_throttle, 0, sizeof(s_throttle));
    s_inited = true;
    return;
  }

  randombytes_buf(s_secKey, sizeof(s_secKey));
  if (crypto_scalarmult_curve25519_base(s_pubKey, s_secKey) != 0) {
    RIFTLINK_DIAG("KEY", "event=KEY_INIT_FAIL cause=scalarmult_base");
    return;
  }
  storage::setBlob(STORAGE_X25519_PUB, s_pubKey, X25519_PUBKEY_LEN);
  storage::setBlob(STORAGE_X25519_SEC, s_secKey, sizeof(s_secKey));
  memset(s_peers, 0, sizeof(s_peers));
  memset(s_throttle, 0, sizeof(s_throttle));
  s_inited = true;
}

bool getOurPublicKey(uint8_t* out) {
  if (!s_inited || !out) return false;
  memcpy(out, s_pubKey, X25519_PUBKEY_LEN);
  return true;
}

void onKeyExchange(const uint8_t* peerId, const uint8_t* theirPubKey) {
  if (!s_inited || !peerId || !theirPubKey) return;
  if (node::isForMe(peerId)) return;
  if (node::isInvalidNodeId(peerId)) return;

  int idx = findPeer(peerId);
  if (idx < 0) idx = findFreeSlot();

  if (rl_box_beforenm(s_peers[idx].sharedKey, theirPubKey, s_secKey) != 0) {
    RIFTLINK_DIAG("KEY", "event=KEY_STORE_FAIL cause=beforenm_failed peer=%02X%02X", peerId[0], peerId[1]);
    sendKeyExchange(peerId, true, false, "key_store_beforenm_fail");
    return;
  }

  memcpy(s_peers[idx].peerId, peerId, protocol::NODE_ID_LEN);
  memcpy(s_peers[idx].peerPubKey, theirPubKey, X25519_PUBKEY_LEN);
  s_peers[idx].timestamp = millis();
  s_peers[idx].used = true;
  RIFTLINK_DIAG("KEY", "event=KEY_STORE_OK peer=%02X%02X slot=%d", peerId[0], peerId[1], idx);
}

bool isPeerPubKeyMismatch(const uint8_t* peerId, const uint8_t* theirPubKey) {
  if (!peerId || !theirPubKey) return false;
  int idx = findPeer(peerId);
  if (idx < 0 || !s_peers[idx].used) return false;
  return memcmp(s_peers[idx].peerPubKey, theirPubKey, X25519_PUBKEY_LEN) != 0;
}

bool hasKeyFor(const uint8_t* peerId) {
  if (!peerId) return false;
  return findPeer(peerId) >= 0;
}

bool getKeyFor(const uint8_t* peerId, uint8_t* keyOut) {
  if (!peerId || !keyOut) return false;
  int idx = findPeer(peerId);
  if (idx < 0) return false;
  memcpy(keyOut, s_peers[idx].sharedKey, 32);
  return true;
}

void sendKeyExchange(const uint8_t* peerId, bool forceSend, bool hadKeyBefore, const char* reason) {
  if (!s_inited || !peerId) return;
  if (node::isForMe(peerId) || node::isBroadcast(peerId) || node::isInvalidNodeId(peerId)) return;

  if (findPeer(peerId) >= 0 && !forceSend) {
    RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=already_has_key peer=%02X%02X", peerId[0], peerId[1]);
    return;
  }

  uint32_t now = millis();
  const bool bypassDebounce = forceSend && !hadKeyBefore;

  /* hello_fwd сразу после HELLO ставит lastSend; key_rx на том же пире через мс — критичный ответ,
   * его нельзя глушить 2500 ms (иначе Heltec получает наш KEY из hello_fwd, а второй KEY не уходит). */
  if (forceSend && !hadKeyBefore) {
    const bool isKeyRxResponse = reason && strcmp(reason, "key_rx") == 0;
    if (!isKeyRxResponse) {
      uint32_t lastSend = 0;
      if (throttleLastSendFor(peerId, &lastSend) && (now - lastSend < KEY_FORCE_MIN_GAP_MS)) {
        RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=force_min_gap peer=%02X%02X", peerId[0], peerId[1]);
        return;
      }
    }
  } else if (!bypassDebounce) {
    if (!isHandshakeKeyOfferReason(reason)) {
      uint32_t lastSend = 0;
      if (throttleLastSendFor(peerId, &lastSend) && (now - lastSend < KEY_DEBOUNCE_MS)) {
        RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=debounce peer=%02X%02X", peerId[0], peerId[1]);
        return;
      }
    }
  }

  if (!(forceSend && !hadKeyBefore)) {
    if (!isHandshakeKeyOfferReason(reason)) {
      uint32_t throttleMs = hadKeyBefore ? KEY_RESPONSE_THROTTLE_MS : KEY_EXCHANGE_THROTTLE_MS;
      uint32_t lastSend = 0;
      if (throttleLastSendFor(peerId, &lastSend) && (now - lastSend < throttleMs)) {
        RIFTLINK_DIAG("KEY", "event=KEY_TX_SKIP cause=throttle peer=%02X%02X", peerId[0], peerId[1]);
        return;
      }
    }
  }

  throttleUpdateFor(peerId, millis());
  uint16_t pktId = ++s_pktIdCounter;
  uint8_t pubCopy[X25519_PUBKEY_LEN];
  memcpy(pubCopy, s_pubKey, X25519_PUBKEY_LEN);

  uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID + X25519_PUBKEY_LEN];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), peerId, 31, protocol::OP_KEY_EXCHANGE,
                                     pubCopy, X25519_PUBKEY_LEN, false, false, false, protocol::CHANNEL_DEFAULT,
                                     pktId);
  if (len > 0 && radio::send(pkt, len)) {
    s_lastKeyTxReadyMs = millis();
    RIFTLINK_DIAG("KEY", "event=KEY_TX ok=1 peer=%02X%02X pktId=%u len=%u", peerId[0], peerId[1],
                  (unsigned)pktId, (unsigned)len);
  } else {
    RIFTLINK_DIAG("KEY", "event=KEY_TX ok=0 peer=%02X%02X", peerId[0], peerId[1]);
  }
}

uint32_t getLastKeyTxReadyMs() {
  return s_lastKeyTxReadyMs;
}

}  // namespace x25519_keys
