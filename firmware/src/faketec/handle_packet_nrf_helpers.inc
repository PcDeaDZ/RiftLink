#define BC_DEDUP_SIZE 32
struct BcDedupEntry { uint8_t from[protocol::NODE_ID_LEN]; uint32_t msgId; };
static BcDedupEntry s_bcDedup[BC_DEDUP_SIZE];
static uint8_t s_bcDedupIdx = 0;

static bool bcDedupSeen(const uint8_t* from, uint32_t msgId) {
  for (int i = 0; i < BC_DEDUP_SIZE; i++) {
    if (memcmp(s_bcDedup[i].from, from, protocol::NODE_ID_LEN) == 0 && s_bcDedup[i].msgId == msgId)
      return true;
  }
  return false;
}
static void bcDedupAdd(const uint8_t* from, uint32_t msgId) {
  memcpy(s_bcDedup[s_bcDedupIdx].from, from, protocol::NODE_ID_LEN);
  s_bcDedup[s_bcDedupIdx].msgId = msgId;
  s_bcDedupIdx = (s_bcDedupIdx + 1) % BC_DEDUP_SIZE;
}

#define PING_REPLY_GUARD_SIZE 16
#define PING_REPLY_GUARD_MS 400
struct PingReplyGuardEntry {
  uint8_t from[protocol::NODE_ID_LEN];
  uint16_t pktId;
  uint32_t seenMs;
};
static PingReplyGuardEntry s_pingReplyGuard[PING_REPLY_GUARD_SIZE];
static uint8_t s_pingReplyGuardIdx = 0;

static bool pingReplySeenRecently(const uint8_t* from, uint16_t pktId, uint32_t nowMs) {
  for (int i = 0; i < PING_REPLY_GUARD_SIZE; i++) {
    if (memcmp(s_pingReplyGuard[i].from, from, protocol::NODE_ID_LEN) != 0) continue;
    if (s_pingReplyGuard[i].pktId != pktId) continue;
    if ((nowMs - s_pingReplyGuard[i].seenMs) <= PING_REPLY_GUARD_MS) return true;
  }
  return false;
}

static void pingReplyMarkSeen(const uint8_t* from, uint16_t pktId, uint32_t nowMs) {
  memcpy(s_pingReplyGuard[s_pingReplyGuardIdx].from, from, protocol::NODE_ID_LEN);
  s_pingReplyGuard[s_pingReplyGuardIdx].pktId = pktId;
  s_pingReplyGuard[s_pingReplyGuardIdx].seenMs = nowMs;
  s_pingReplyGuardIdx = (s_pingReplyGuardIdx + 1) % PING_REPLY_GUARD_SIZE;
}

#define UC_DEDUP_SIZE 32
struct UcDedupEntry { uint8_t from[protocol::NODE_ID_LEN]; uint32_t msgId; };
static UcDedupEntry s_ucDedup[UC_DEDUP_SIZE];

// Unified RX anti-noise guard for control/data duplicates.
// Drops short-window reflections/replays before heavy decrypt/relay paths.
#define RX_NOISE_GUARD_SIZE 96
struct RxNoiseGuardEntry {
  uint8_t from[protocol::NODE_ID_LEN];
  uint8_t opcode;
  uint16_t pktId;
  uint32_t payloadSig;
  uint32_t seenMs;
  bool used;
};
static RxNoiseGuardEntry s_rxNoiseGuard[RX_NOISE_GUARD_SIZE];
static uint8_t s_rxNoiseDropAck = 0;
static uint8_t s_rxNoiseDropMsg = 0;
static uint8_t s_rxNoiseDropRelay = 0;
static uint8_t s_rxNoiseDropOther = 0;
static uint32_t s_rxNoiseLastLogMs = 0;

static uint32_t rxNoisePayloadSig(const uint8_t* payload, size_t payloadLen) {
  if (!payload || payloadLen == 0) return 0;
  uint32_t h = 2166136261u;
  size_t lim = (payloadLen < 32) ? payloadLen : 32;
  for (size_t i = 0; i < lim; i++) {
    h ^= payload[i];
    h *= 16777619u;
  }
  h ^= (uint32_t)payloadLen;
  return h;
}

static uint16_t rxNoiseWindowMs(const protocol::PacketHeader& hdr) {
  bool toMeOrBc = node::isForMe(hdr.to) || node::isBroadcast(hdr.to);
  switch (hdr.opcode) {
    case protocol::OP_ACK:
    case protocol::OP_ACK_BATCH:
      return 300;
    case protocol::OP_NACK:
    case protocol::OP_ECHO:
      return 1800;
    case protocol::OP_XOR_RELAY:
      return 2200;
    case protocol::OP_MSG:
    case protocol::OP_GROUP_MSG:
    case protocol::OP_MSG_BATCH:
    case protocol::OP_MSG_FRAG:
    case protocol::OP_VOICE_MSG:
    case protocol::OP_SOS:
      // Keep message path sensitive: suppress only near-instant duplicates.
      return toMeOrBc ? 260 : 1200;
    case protocol::OP_HELLO:
      return 350;
    case protocol::OP_KEY_EXCHANGE:
      return 1200;
    default:
      return 0;
  }
}

static bool rxNoiseSeenRecently(const protocol::PacketHeader& hdr, const uint8_t* payload, size_t payloadLen, uint32_t nowMs) {
  uint16_t wnd = rxNoiseWindowMs(hdr);
  if (wnd == 0) return false;
  uint32_t sig = rxNoisePayloadSig(payload, payloadLen);
  int freeIdx = -1;
  int oldestIdx = 0;
  uint32_t oldestTs = s_rxNoiseGuard[0].seenMs;
  for (int i = 0; i < RX_NOISE_GUARD_SIZE; i++) {
    RxNoiseGuardEntry& e = s_rxNoiseGuard[i];
    if (!e.used) {
      if (freeIdx < 0) freeIdx = i;
      continue;
    }
    if (e.seenMs < oldestTs) {
      oldestTs = e.seenMs;
      oldestIdx = i;
    }
    if (e.opcode != hdr.opcode || e.pktId != hdr.pktId || e.payloadSig != sig) continue;
    if (memcmp(e.from, hdr.from, protocol::NODE_ID_LEN) != 0) continue;
    if ((uint32_t)(nowMs - e.seenMs) <= (uint32_t)wnd) {
      e.seenMs = nowMs;
      return true;
    }
    e.seenMs = nowMs;
    return false;
  }
  int putIdx = (freeIdx >= 0) ? freeIdx : oldestIdx;
  RxNoiseGuardEntry& dst = s_rxNoiseGuard[putIdx];
  memcpy(dst.from, hdr.from, protocol::NODE_ID_LEN);
  dst.opcode = hdr.opcode;
  dst.pktId = hdr.pktId;
  dst.payloadSig = sig;
  dst.seenMs = nowMs;
  dst.used = true;
  return false;
}

static void rxNoiseDropAccount(uint8_t opcode) {
  if (opcode == protocol::OP_ACK || opcode == protocol::OP_ACK_BATCH ||
      opcode == protocol::OP_NACK || opcode == protocol::OP_ECHO) {
    if (s_rxNoiseDropAck < 255) s_rxNoiseDropAck++;
  } else if (opcode == protocol::OP_MSG || opcode == protocol::OP_GROUP_MSG ||
             opcode == protocol::OP_MSG_BATCH || opcode == protocol::OP_MSG_FRAG ||
             opcode == protocol::OP_VOICE_MSG || opcode == protocol::OP_SOS) {
    if (s_rxNoiseDropMsg < 255) s_rxNoiseDropMsg++;
  } else if (opcode == protocol::OP_XOR_RELAY) {
    if (s_rxNoiseDropRelay < 255) s_rxNoiseDropRelay++;
  } else {
    if (s_rxNoiseDropOther < 255) s_rxNoiseDropOther++;
  }
}

static void rxNoiseMaybeLogSummary(uint32_t nowMs) {
  uint16_t total = (uint16_t)s_rxNoiseDropAck + (uint16_t)s_rxNoiseDropMsg +
                   (uint16_t)s_rxNoiseDropRelay + (uint16_t)s_rxNoiseDropOther;
  if (total == 0) return;
  if ((uint32_t)(nowMs - s_rxNoiseLastLogMs) < 1500) return;
  RIFTLINK_DIAG("RADIO", "event=RX_NOISE_SUMMARY ack=%u msg=%u relay=%u other=%u",
      (unsigned)s_rxNoiseDropAck, (unsigned)s_rxNoiseDropMsg,
      (unsigned)s_rxNoiseDropRelay, (unsigned)s_rxNoiseDropOther);
  s_rxNoiseDropAck = 0;
  s_rxNoiseDropMsg = 0;
  s_rxNoiseDropRelay = 0;
  s_rxNoiseDropOther = 0;
  s_rxNoiseLastLogMs = nowMs;
}

// Relay dedup + rate limit (mesh storm protection)
#define RELAY_DEDUP_SIZE 24
#define RELAY_RATE_MAX 3
#define RELAY_RATE_WINDOW_MS 1000
#define SOS_RELAY_RATE_MAX 5
#define SOS_RELAY_RATE_WINDOW_MS 2000
#define SOS_RELAY_PER_SRC_MAX 2
#define SOS_RELAY_PER_SRC_WINDOW_MS 2500
struct RelayDedupEntry { uint8_t from[protocol::NODE_ID_LEN]; uint32_t payloadHash; };
static RelayDedupEntry s_relayDedup[RELAY_DEDUP_SIZE];
static uint8_t s_relayDedupIdx = 0;
static uint32_t s_relayRateWindowStart = 0;
static uint8_t s_relayRateCount = 0;
static uint32_t s_sosRelayRateWindowStart = 0;
static uint8_t s_sosRelayRateCount = 0;
struct SosRelaySrcQuota {
  uint8_t from[protocol::NODE_ID_LEN];
  uint32_t windowStartMs;
  uint8_t count;
  bool used;
};
static SosRelaySrcQuota s_sosSrcQuota[8];

static uint32_t relayPayloadHash(const uint8_t* p, size_t n) {
  uint32_t h = 5381;
  for (size_t i = 0; i < n && i < 128; i++) h = ((h << 5) + h) + p[i];
  return h;
}
static bool relayDedupSeen(const uint8_t* from, uint32_t hash) {
  for (int i = 0; i < RELAY_DEDUP_SIZE; i++) {
    if (memcmp(s_relayDedup[i].from, from, protocol::NODE_ID_LEN) == 0 && s_relayDedup[i].payloadHash == hash)
      return true;
  }
  return false;
}
static void relayDedupAdd(const uint8_t* from, uint32_t hash) {
  memcpy(s_relayDedup[s_relayDedupIdx].from, from, protocol::NODE_ID_LEN);
  s_relayDedup[s_relayDedupIdx].payloadHash = hash;
  s_relayDedupIdx = (s_relayDedupIdx + 1) % RELAY_DEDUP_SIZE;
}
static bool relayRateLimitExceeded() {
  uint32_t now = millis();
  if (now - s_relayRateWindowStart >= RELAY_RATE_WINDOW_MS) {
    s_relayRateWindowStart = now;
    s_relayRateCount = 0;
  }
  return s_relayRateCount >= RELAY_RATE_MAX;
}
static void relayRateRecord() { s_relayRateCount++; }
static bool relaySosRateLimitExceeded() {
  uint32_t now = millis();
  if (now - s_sosRelayRateWindowStart >= SOS_RELAY_RATE_WINDOW_MS) {
    s_sosRelayRateWindowStart = now;
    s_sosRelayRateCount = 0;
  }
  return s_sosRelayRateCount >= SOS_RELAY_RATE_MAX;
}
static void relaySosRateRecord() { s_sosRelayRateCount++; }
static int findSosSrcQuota(const uint8_t* from) {
  for (int i = 0; i < 8; i++) {
    if (s_sosSrcQuota[i].used &&
        memcmp(s_sosSrcQuota[i].from, from, protocol::NODE_ID_LEN) == 0) return i;
  }
  return -1;
}
static int findFreeSosSrcQuota() {
  uint32_t now = millis();
  for (int i = 0; i < 8; i++) {
    if (!s_sosSrcQuota[i].used) return i;
    if (now - s_sosSrcQuota[i].windowStartMs >= SOS_RELAY_PER_SRC_WINDOW_MS) return i;
  }
  int oldest = 0;
  uint32_t oldestTs = s_sosSrcQuota[0].windowStartMs;
  for (int i = 1; i < 8; i++) {
    if (s_sosSrcQuota[i].windowStartMs < oldestTs) {
      oldestTs = s_sosSrcQuota[i].windowStartMs;
      oldest = i;
    }
  }
  return oldest;
}
static bool relaySosPerSourceLimitExceeded(const uint8_t* from) {
  uint32_t now = millis();
  int idx = findSosSrcQuota(from);
  if (idx < 0) {
    idx = findFreeSosSrcQuota();
    memcpy(s_sosSrcQuota[idx].from, from, protocol::NODE_ID_LEN);
    s_sosSrcQuota[idx].windowStartMs = now;
    s_sosSrcQuota[idx].count = 0;
    s_sosSrcQuota[idx].used = true;
  }
  if (now - s_sosSrcQuota[idx].windowStartMs >= SOS_RELAY_PER_SRC_WINDOW_MS) {
    s_sosSrcQuota[idx].windowStartMs = now;
    s_sosSrcQuota[idx].count = 0;
  }
  return s_sosSrcQuota[idx].count >= SOS_RELAY_PER_SRC_MAX;
}
static void relaySosPerSourceRecord(const uint8_t* from) {
  int idx = findSosSrcQuota(from);
  if (idx < 0) {
    idx = findFreeSosSrcQuota();
    memcpy(s_sosSrcQuota[idx].from, from, protocol::NODE_ID_LEN);
    s_sosSrcQuota[idx].windowStartMs = millis();
    s_sosSrcQuota[idx].count = 0;
    s_sosSrcQuota[idx].used = true;
  }
  s_sosSrcQuota[idx].count++;
}
static uint8_t s_ucDedupIdx = 0;

// GeoFence anti-spoof baseline: sanity check координат и нереалистичных скачков позиции.
#define LOCATION_TRUST_SLOTS 12
#define LOCATION_MAX_SPEED_MPS 90.0f
#define LOCATION_BASE_JITTER_M 250.0f
#define GEOFENCE_RADIUS_MAX_M 50000
struct LocationTrustEntry {
  uint8_t from[protocol::NODE_ID_LEN];
  float lat;
  float lon;
  uint32_t lastSeenMs;
  bool used;
};
static LocationTrustEntry s_locationTrust[LOCATION_TRUST_SLOTS];

static int findLocationTrust(const uint8_t* from) {
  for (int i = 0; i < LOCATION_TRUST_SLOTS; i++) {
    if (s_locationTrust[i].used &&
        memcmp(s_locationTrust[i].from, from, protocol::NODE_ID_LEN) == 0) return i;
  }
  return -1;
}

static int findLocationTrustSlot() {
  for (int i = 0; i < LOCATION_TRUST_SLOTS; i++) {
    if (!s_locationTrust[i].used) return i;
  }
  int oldest = 0;
  uint32_t oldestMs = s_locationTrust[0].lastSeenMs;
  for (int i = 1; i < LOCATION_TRUST_SLOTS; i++) {
    if (s_locationTrust[i].lastSeenMs < oldestMs) {
      oldestMs = s_locationTrust[i].lastSeenMs;
      oldest = i;
    }
  }
  return oldest;
}

static bool isLocationSpoofLike(const uint8_t* from, float lat, float lon, uint32_t nowMs) {
  if (!from) return true;
  if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) return true;
  int idx = findLocationTrust(from);
  if (idx < 0) return false;
  uint32_t dtMs = nowMs - s_locationTrust[idx].lastSeenMs;
  if (dtMs == 0) return false;
  float dLat = (lat - s_locationTrust[idx].lat) * 111000.0f;
  float dLon = (lon - s_locationTrust[idx].lon) * 111000.0f;
  float dist2 = dLat * dLat + dLon * dLon;
  float maxDist = LOCATION_BASE_JITTER_M + LOCATION_MAX_SPEED_MPS * ((float)dtMs / 1000.0f);
  return dist2 > (maxDist * maxDist);
}

static void rememberLocationTrust(const uint8_t* from, float lat, float lon, uint32_t nowMs) {
  int idx = findLocationTrust(from);
  if (idx < 0) idx = findLocationTrustSlot();
  memcpy(s_locationTrust[idx].from, from, protocol::NODE_ID_LEN);
  s_locationTrust[idx].lat = lat;
  s_locationTrust[idx].lon = lon;
  s_locationTrust[idx].lastSeenMs = nowMs;
  s_locationTrust[idx].used = true;
}
static bool ucDedupSeen(const uint8_t* from, uint32_t msgId) {
  for (int i = 0; i < UC_DEDUP_SIZE; i++) {
    if (memcmp(s_ucDedup[i].from, from, protocol::NODE_ID_LEN) == 0 && s_ucDedup[i].msgId == msgId)
      return true;
  }
  return false;
}
static void ucDedupAdd(const uint8_t* from, uint32_t msgId) {
  memcpy(s_ucDedup[s_ucDedupIdx].from, from, protocol::NODE_ID_LEN);
  s_ucDedup[s_ucDedupIdx].msgId = msgId;
  s_ucDedupIdx = (s_ucDedupIdx + 1) % UC_DEDUP_SIZE;
}
// Lightweight sender-tag for HELLO: detects from-ID corruption without crypto handshake.
// Tag is deterministic from full NODE_ID and validated by receiver when payloadLen==2.
static inline uint16_t helloSenderTag16(const uint8_t* nodeId) {
  if (!nodeId) return 0;
  uint32_t h = 2166136261u;  // FNV-1a 32
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    h ^= nodeId[i];
    h *= 16777619u;
  }
  return (uint16_t)((h >> 16) ^ (h & 0xFFFFu));
}

static bool validateHelloSenderTag(const protocol::PacketHeader& hdr, const uint8_t* payload, size_t payloadLen) {
  if (!payload || payloadLen != 2) return false;
  uint16_t got = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
  uint16_t expected = helloSenderTag16(hdr.from);
  if (got == expected) return true;
  RIFTLINK_DIAG("HELLO", "event=HELLO_TAG_MISMATCH from=%02X%02X got=%u expected=%u",
      hdr.from[0], hdr.from[1], (unsigned)got, (unsigned)expected);
  return false;
}
