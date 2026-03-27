/**
 * Network Coding XOR — кэш пакетов, XOR при relay, декодирование у получателя
 */

#include "network_coding/network_coding.h"
#include "node.h"
#include <string.h>

#define XOR_CACHE_SIZE 4
#define PENDING_XOR_SIZE 2

// Meta в payload OP_XOR_RELAY: pktIdA(2) + pktIdB(2) + fromA(8) + toA(8) + fromB(8) + toB(8) = 36
#define XOR_META_LEN 36

static uint32_t payloadHash(const uint8_t* p, size_t n) {
  uint32_t h = 5381;
  for (size_t i = 0; i < n && i < 128; i++) h = ((h << 5) + h) + p[i];
  return h;
}

static bool isUnicast(const uint8_t* to) {
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    if (to[i] != 0xFF) return true;
  }
  return false;
}

/** Пара подходит: to совпадает ИЛИ (from/to разные но оба unicast) */
static bool isPair(const uint8_t* fromA, const uint8_t* toA,
                   const uint8_t* fromB, const uint8_t* toB) {
  if (!isUnicast(toA) || !isUnicast(toB)) return false;
  if (memcmp(toA, toB, protocol::NODE_ID_LEN) == 0) return true;
  return true;  // разные to, оба unicast
}

struct XorCacheEntry {
  uint8_t from[protocol::NODE_ID_LEN];
  uint8_t to[protocol::NODE_ID_LEN];
  uint16_t pktId;
  uint8_t pkt[256];
  uint16_t len;
  uint32_t payloadHash;
  bool inUse;
};

struct PendingXorEntry {
  uint8_t xorPayload[protocol::MAX_PAYLOAD];
  uint16_t xorLen;
  uint16_t pktIdA, pktIdB;
  uint8_t fromA[protocol::NODE_ID_LEN], toA[protocol::NODE_ID_LEN];
  uint8_t fromB[protocol::NODE_ID_LEN], toB[protocol::NODE_ID_LEN];
  bool inUse;
};

static XorCacheEntry s_cache[XOR_CACHE_SIZE];
static PendingXorEntry s_pending[PENDING_XOR_SIZE];
static int s_pairIdxA = -1;
static int s_pairIdxB = -1;
static bool s_inited = false;

namespace network_coding {

void init() {
  if (s_inited) return;
  for (int i = 0; i < XOR_CACHE_SIZE; i++) s_cache[i].inUse = false;
  for (int i = 0; i < PENDING_XOR_SIZE; i++) s_pending[i].inUse = false;
  s_inited = true;
}

bool addForXor(const uint8_t* pkt, size_t len, const uint8_t* from, const uint8_t* to) {
  if (!s_inited || !pkt || !from || !to || len > 256) return false;

  protocol::PacketHeader hdr;
  const uint8_t* payload;
  size_t payloadLen;
  if (!protocol::parsePacket(pkt, len, &hdr, &payload, &payloadLen)) return false;

  if (hdr.opcode != protocol::OP_MSG || hdr.pktId == 0) return false;
  if (!isUnicast(to)) return false;

  uint32_t ph = payloadHash(payload, payloadLen);

  int newIdx = -1;
  for (int i = 0; i < XOR_CACHE_SIZE; i++) {
    if (!s_cache[i].inUse) {
      if (newIdx < 0) newIdx = i;
      continue;
    }
    if (isPair(from, to, s_cache[i].from, s_cache[i].to)) {
      s_pairIdxA = i;
      s_pairIdxB = (newIdx >= 0 && newIdx != i) ? newIdx : ((i == 0) ? 1 : 0);
      if (s_pairIdxB == s_pairIdxA) {
        for (int j = 0; j < XOR_CACHE_SIZE; j++) {
          if (j != s_pairIdxA && !s_cache[j].inUse) { s_pairIdxB = j; break; }
        }
      }
      s_cache[s_pairIdxB].inUse = true;
      memcpy(s_cache[s_pairIdxB].from, from, protocol::NODE_ID_LEN);
      memcpy(s_cache[s_pairIdxB].to, to, protocol::NODE_ID_LEN);
      s_cache[s_pairIdxB].pktId = hdr.pktId;
      memcpy(s_cache[s_pairIdxB].pkt, pkt, len);
      s_cache[s_pairIdxB].len = (uint16_t)len;
      s_cache[s_pairIdxB].payloadHash = ph;
      return true;
    }
  }

  if (newIdx < 0) newIdx = 0;
  s_cache[newIdx].inUse = true;
  memcpy(s_cache[newIdx].from, from, protocol::NODE_ID_LEN);
  memcpy(s_cache[newIdx].to, to, protocol::NODE_ID_LEN);
  s_cache[newIdx].pktId = hdr.pktId;
  memcpy(s_cache[newIdx].pkt, pkt, len);
  s_cache[newIdx].len = (uint16_t)len;
  s_cache[newIdx].payloadHash = ph;
  return false;
}

bool getXorPacket(uint8_t* out, size_t maxOut, size_t* lenOut) {
  if (!s_inited || s_pairIdxA < 0 || s_pairIdxB < 0 || !out || !lenOut) return false;

  XorCacheEntry& a = s_cache[s_pairIdxA];
  XorCacheEntry& b = s_cache[s_pairIdxB];
  if (!a.inUse || !b.inUse) return false;

  protocol::PacketHeader hdrA, hdrB;
  const uint8_t* plA;
  const uint8_t* plB;
  size_t lenA, lenB;
  if (!protocol::parsePacket(a.pkt, a.len, &hdrA, &plA, &lenA)) return false;
  if (!protocol::parsePacket(b.pkt, b.len, &hdrB, &plB, &lenB)) return false;

  size_t xorLen = (lenA > lenB) ? lenA : lenB;
  if (maxOut < protocol::SYNC_LEN + protocol::HEADER_LEN_BROADCAST + XOR_META_LEN + xorLen)
    return false;

  uint8_t meta[XOR_META_LEN];
  meta[0] = (uint8_t)(a.pktId & 0xFF);
  meta[1] = (uint8_t)(a.pktId >> 8);
  meta[2] = (uint8_t)(b.pktId & 0xFF);
  meta[3] = (uint8_t)(b.pktId >> 8);
  memcpy(meta + 4, a.from, protocol::NODE_ID_LEN);
  memcpy(meta + 12, a.to, protocol::NODE_ID_LEN);
  memcpy(meta + 20, b.from, protocol::NODE_ID_LEN);
  memcpy(meta + 28, b.to, protocol::NODE_ID_LEN);

  uint8_t xorPayload[protocol::MAX_PAYLOAD];
  for (size_t i = 0; i < xorLen; i++) {
    uint8_t va = (i < lenA) ? plA[i] : 0;
    uint8_t vb = (i < lenB) ? plB[i] : 0;
    xorPayload[i] = va ^ vb;
  }

  uint8_t fullPayload[XOR_META_LEN + protocol::MAX_PAYLOAD];
  memcpy(fullPayload, meta, XOR_META_LEN);
  memcpy(fullPayload + XOR_META_LEN, xorPayload, xorLen);

  size_t pktLen = protocol::buildPacket(out, maxOut,
      node::getId(), protocol::BROADCAST_ID,
      31, protocol::OP_XOR_RELAY,
      fullPayload, XOR_META_LEN + xorLen, false, false, false, protocol::CHANNEL_DEFAULT, 0);

  if (pktLen == 0) return false;

  *lenOut = pktLen;
  a.inUse = false;
  b.inUse = false;
  s_pairIdxA = s_pairIdxB = -1;
  return true;
}

void getLastPairOther(uint8_t* fromOut, uint32_t* payloadHashOut) {
  if (!fromOut || !payloadHashOut) return;
  if (s_pairIdxA < 0 || s_pairIdxB < 0) return;
  XorCacheEntry& a = s_cache[s_pairIdxA];
  XorCacheEntry& b = s_cache[s_pairIdxB];
  if (!a.inUse || !b.inUse) return;
  memcpy(fromOut, a.from, protocol::NODE_ID_LEN);
  *payloadHashOut = a.payloadHash;
}

bool onXorRelayReceived(const uint8_t* buf, size_t len, uint8_t* decodedOut, size_t* decodedLenOut) {
  if (!s_inited || !buf || len < protocol::SYNC_LEN + protocol::HEADER_LEN_BROADCAST + XOR_META_LEN)
    return false;

  protocol::PacketHeader hdr;
  const uint8_t* payload;
  size_t payloadLen;
  if (!protocol::parsePacket(buf, len, &hdr, &payload, &payloadLen)) return false;
  if (hdr.opcode != protocol::OP_XOR_RELAY || payloadLen < XOR_META_LEN) return false;

  uint16_t pktIdA = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
  uint16_t pktIdB = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
  const uint8_t* fromA = payload + 4;
  const uint8_t* toA = payload + 12;
  const uint8_t* fromB = payload + 20;
  const uint8_t* toB = payload + 28;
  const uint8_t* xorPl = payload + XOR_META_LEN;
  size_t xorLen = payloadLen - XOR_META_LEN;

  int matchIdx = -1;
  bool matchA = true;
  for (int i = 0; i < XOR_CACHE_SIZE; i++) {
    if (!s_cache[i].inUse) continue;
    if (memcmp(s_cache[i].from, fromA, protocol::NODE_ID_LEN) == 0 &&
        memcmp(s_cache[i].to, toA, protocol::NODE_ID_LEN) == 0 && s_cache[i].pktId == pktIdA) {
      matchIdx = i;
      matchA = true;
      break;
    }
    if (memcmp(s_cache[i].from, fromB, protocol::NODE_ID_LEN) == 0 &&
        memcmp(s_cache[i].to, toB, protocol::NODE_ID_LEN) == 0 && s_cache[i].pktId == pktIdB) {
      matchIdx = i;
      matchA = false;
      break;
    }
  }

  if (matchIdx < 0) {
    for (int i = 0; i < PENDING_XOR_SIZE; i++) {
      if (!s_pending[i].inUse) {
        s_pending[i].inUse = true;
        s_pending[i].pktIdA = pktIdA;
        s_pending[i].pktIdB = pktIdB;
        memcpy(s_pending[i].fromA, fromA, protocol::NODE_ID_LEN);
        memcpy(s_pending[i].toA, toA, protocol::NODE_ID_LEN);
        memcpy(s_pending[i].fromB, fromB, protocol::NODE_ID_LEN);
        memcpy(s_pending[i].toB, toB, protocol::NODE_ID_LEN);
        s_pending[i].xorLen = (xorLen < protocol::MAX_PAYLOAD) ? (uint16_t)xorLen : protocol::MAX_PAYLOAD;
        memcpy(s_pending[i].xorPayload, xorPl, s_pending[i].xorLen);
        break;
      }
    }
    return false;
  }

  XorCacheEntry& known = s_cache[matchIdx];
  protocol::PacketHeader hdrKnown;
  const uint8_t* plKnown;
  size_t lenKnown;
  if (!protocol::parsePacket(known.pkt, known.len, &hdrKnown, &plKnown, &lenKnown)) return false;

  size_t outLen = (xorLen > lenKnown) ? xorLen : lenKnown;
  if (outLen > 256) return false;

  uint8_t decPayload[protocol::MAX_PAYLOAD];
  for (size_t i = 0; i < outLen; i++) {
    uint8_t x = (i < xorLen) ? xorPl[i] : 0;
    uint8_t k = (i < lenKnown) ? plKnown[i] : 0;
    decPayload[i] = x ^ k;
  }

  const uint8_t* decFrom = matchA ? fromB : fromA;
  const uint8_t* decTo = matchA ? toB : toA;
  uint16_t decPktId = matchA ? pktIdB : pktIdA;

  size_t decFullLen = protocol::buildPacket(decodedOut, 256,
      decFrom, decTo, 31, protocol::OP_MSG,
      decPayload, outLen,
      true, false, false,
      protocol::CHANNEL_DEFAULT, decPktId);

  if (decFullLen == 0) return false;

  *decodedLenOut = decFullLen;
  known.inUse = false;

  for (int i = 0; i < PENDING_XOR_SIZE; i++) {
    if (s_pending[i].inUse) {
      bool match = (memcmp(s_pending[i].fromA, decFrom, protocol::NODE_ID_LEN) == 0 &&
                   memcmp(s_pending[i].toA, decTo, protocol::NODE_ID_LEN) == 0 &&
                   s_pending[i].pktIdA == decPktId) ||
                  (memcmp(s_pending[i].fromB, decFrom, protocol::NODE_ID_LEN) == 0 &&
                   memcmp(s_pending[i].toB, decTo, protocol::NODE_ID_LEN) == 0 &&
                   s_pending[i].pktIdB == decPktId);
      if (match) s_pending[i].inUse = false;
    }
  }
  return true;
}

bool getDecodedFromPending(const uint8_t* pkt, size_t len, const uint8_t* from, const uint8_t* to, uint16_t pktId,
                           uint8_t* decodedOut, size_t* decodedLenOut) {
  if (!s_inited || !pkt || !decodedOut || !decodedLenOut) return false;

  protocol::PacketHeader hdr;
  const uint8_t* plKnown;
  size_t lenKnown;
  if (!protocol::parsePacket(pkt, len, &hdr, &plKnown, &lenKnown)) return false;

  for (int i = 0; i < PENDING_XOR_SIZE; i++) {
    if (!s_pending[i].inUse) continue;
    PendingXorEntry& pe = s_pending[i];
    bool matchA = (memcmp(pe.fromA, from, protocol::NODE_ID_LEN) == 0 &&
                   memcmp(pe.toA, to, protocol::NODE_ID_LEN) == 0 && pe.pktIdA == pktId);
    bool matchB = (memcmp(pe.fromB, from, protocol::NODE_ID_LEN) == 0 &&
                   memcmp(pe.toB, to, protocol::NODE_ID_LEN) == 0 && pe.pktIdB == pktId);
    if (!matchA && !matchB) continue;

    size_t xorLen = pe.xorLen;
    size_t outLen = (xorLen > lenKnown) ? xorLen : lenKnown;
    if (outLen > protocol::MAX_PAYLOAD) continue;

    uint8_t decPayload[protocol::MAX_PAYLOAD];
    for (size_t j = 0; j < outLen; j++) {
      uint8_t x = (j < xorLen) ? pe.xorPayload[j] : 0;
      uint8_t k = (j < lenKnown) ? plKnown[j] : 0;
      decPayload[j] = x ^ k;
    }

    const uint8_t* decFrom = matchA ? pe.fromB : pe.fromA;
    const uint8_t* decTo = matchA ? pe.toB : pe.toA;
    uint16_t decPktId = matchA ? pe.pktIdB : pe.pktIdA;

    size_t decFullLen = protocol::buildPacket(decodedOut, 256,
        decFrom, decTo, 31, protocol::OP_MSG,
        decPayload, outLen, true, false, false,
        protocol::CHANNEL_DEFAULT, decPktId);
    if (decFullLen == 0) continue;

    *decodedLenOut = decFullLen;
    pe.inUse = false;
    return true;
  }
  return false;
}

}  // namespace network_coding
