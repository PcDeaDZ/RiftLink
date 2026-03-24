/**
 * RiftLink (RL) Firmware — Фаза 1
 * Heltec WiFi LoRa 32 (V3/V4): ESP32-S3, SX1262, OLED, BLE
 *
 * План: docs/CUSTOM_PROTOCOL_PLAN.md
 */

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <esp_random.h>

// Fallback: loopTask stack — build_flags -DARDUINO_LOOP_STACK_SIZE может не применяться к framework
#if defined(ESP_LOOP_TASK_STACK_SIZE)
ESP_LOOP_TASK_STACK_SIZE(32768);
#elif defined(SET_LOOP_TASK_STACK_SIZE)
SET_LOOP_TASK_STACK_SIZE(32768);
#endif
#include <nvs_flash.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>

#include "radio/radio.h"
#include "protocol/packet.h"
#include "node/node.h"
#include "ui/display.h"
#include "crypto/crypto.h"
#include "ble/ble.h"
#include "msg_queue/msg_queue.h"
#include "mab/mab.h"
#include "compress/compress.h"
#include "frag/frag.h"
#include "telemetry/telemetry.h"
#include "region/region.h"
#include "collision_slots/collision_slots.h"
#include "beacon_sync/beacon_sync.h"
#include "clock_drift/clock_drift.h"
#include "bls_n/bls_n.h"
#include "esp_now_slots/esp_now_slots.h"
#include "packet_fusion/packet_fusion.h"
#include "ack_coalesce/ack_coalesce.h"
#include "network_coding/network_coding.h"
#include "neighbors/neighbors.h"
#include "groups/groups.h"
#include "offline_queue/offline_queue.h"
#include "routing/routing.h"
#include "x25519_keys/x25519_keys.h"
#include "wifi/wifi.h"
#include "locale/locale.h"
#include "voice_frag/voice_frag.h"
#include "gps/gps.h"
#include "selftest/selftest.h"
#include "powersave/powersave.h"
#include "async_queues.h"
#include "async_tasks.h"
#include "memory_diag/memory_diag.h"
#include "send_overflow/send_overflow.h"
#include "led/led.h"
#include "duty_cycle/duty_cycle.h"
#include "pkt_cache/pkt_cache.h"
#include "radio_mode/radio_mode.h"
#include "ws_server/ws_server.h"
#include "log.h"

#if defined(ARDUINO_LILYGO_T_LORA_PAGER)
#include "board/lilygo_tpager.h"
#define BUTTON_PIN 7   // энкодер center (wiki)
#define LED_PIN 0      // нет отдельного LED как на Heltec; GPIO35 = SPI SCK на T-Pager
#else
#define BUTTON_PIN 0   // Heltec USER_SW
#define LED_PIN 35     // Heltec V3/V4
#endif
#define MIN_PRESS_MS       80
#if defined(USE_EINK)
#define LONG_PRESS_MS      900  // e-ink: 500ms мало — пользователь ждёт отрисовку, часто ложно long
#else
#define LONG_PRESS_MS      500
#endif
#define RADIO_MODE_PRESS_MS 3000   // очень длинное нажатие — переключение BLE ↔ WiFi
#define SHUTDOWN_PRESS_MS   5000   // 5с — выключение устройства (deep sleep)
#define POST_PRESS_DEBOUNCE_MS 400  // игнор дребезга после обработки — против двойного long press
#define PENDING_REDRAW_RETRY_MS 2500  // retry смены вкладки, если дисплей не принял за 2.5с

#define HELLO_INTERVAL_MS  36000  // 36с — спокойный режим (реже лишний beacon в эфире)
#define HELLO_INTERVAL_AGGRESSIVE_MS 12000  // 12с — discovery при 0 соседях (раньше 9с — слишком плотно)
#define HELLO_INTERVAL_ONE_NEIGH_MS 24000   // 24с при одном соседе
#define HELLO_JITTER_MS    3000   // ±3с — при 1+ соседях
#define HELLO_JITTER_ZERO_MS 500  // ±0.5с при 0 соседях — меньше разброс для быстрого discovery
/** Жёсткий rate-limit HELLO: даже при баге дедлайна не шумим чаще, чем раз в 10с. */
#define HELLO_HARD_MIN_INTERVAL_MS 10000
/** После drop (очередь/CAD) не долбить sendHello каждые ~10 ms */
#define HELLO_DROP_RETRY_MS 350
#define HELLO_QUIET_AFTER_KEY_MS 2500  // после KEY дать эфиру окно под ответный KEY
#define HANDSHAKE_TRAFFIC_QUIET_MS 3000  // на время handshake приглушаем HELLO/telemetry
#define HELLO_STALE_KEY_REFRESH_MS 15000  // HELLO после заметной паузы: возможный reboot peer, push KEY refresh
static uint32_t s_lastHelloAirMs = 0;
static uint32_t s_nextHelloAfterDropMs = 0;
static uint32_t s_handshakeQuietUntilMs = 0;
static uint32_t s_lastObservedKeyTxMs = 0;
/** HELLO «сразу при первом соседе» — только из loop(), не из packetTask (TX только через очередь). */
static std::atomic<bool> s_pendingDiscoveryHello{false};
/** HELLO в эфир только из radioSchedulerTask (loop лишь ставит флаг). */
static std::atomic<bool> s_helloScheduled{false};
static inline void scheduleHelloTx() {
  s_helloScheduled.store(true, std::memory_order_release);
}
#define TELEM_INTERVAL_MS  60000
#define GPS_LOC_INTERVAL_MS 60000  // интервал отправки локации с GPS
#define SF_ADAPT_INTERVAL_MS 30000 // адаптивный SF по качеству связи
#define POLL_INTERVAL_MS   5000    // RIT: «присылайте пакеты для меня» каждые 5с (pipelining)
#define POLL_JITTER_BASE_MS 20
#define POLL_JITTER_SPAN_MS 220
// Quiet 2-node profile: disable periodic service traffic unless explicitly needed.
#define AUTO_POLL_ENABLED 1
#define AUTO_TELEMETRY_ENABLED 1

static uint32_t lastHello = 0;
/** Абсолютное время следующего HELLO — джиттер задаётся один раз на период (не каждый проход loop). */
static uint32_t s_nextHelloDueMs = 0;
static int s_helloPlannerLastN = -999;
static uint32_t lastPoll = 0;
static uint32_t lastTelemetry = 0;
static uint32_t lastGpsLoc = 0;
static uint32_t s_zeroNeighSince = 0;  // для HELLO backoff при 0 соседях: 8s -> 15s -> 30s
static uint32_t s_oneNeighSince = 0;   // bootstrap при 1 соседе: HELLO чаще первые 2 минуты
static uint32_t s_bootTime = 0;       // millis() при старте — агрессивный discovery первые 2 мин
static uint32_t s_lastKeyRetry = 0;   // retry KEY_EXCHANGE каждые 30с для соседей без ключа
// Фиксированный SF: задаётся пользователем через BLE-команду "sf", значение по умолчанию = 7.
static const uint8_t SF_DEFAULT = 7;
static uint32_t s_loopCooldownUntil = 0;  // вместо delay(10) — throttle OTA и конец loop
static uint8_t s_bootPhase = 0;           // 0=ожидание 4с, 1=готов
static uint32_t s_bootPhaseStart = 0;
static uint32_t s_lastDiagSnapshotMs = 0;
static uint32_t s_lastBatCheckMs = 0;
static uint8_t s_lowBatCount = 0;
#define LOW_BAT_MV          3000
#define LOW_BAT_CHECK_MS    30000
#define LOW_BAT_SHUTOFF_CNT 3
static uint8_t rxBuf[protocol::SYNC_LEN + protocol::HEADER_LEN + protocol::MAX_PAYLOAD + crypto::OVERHEAD];

static inline bool isHandshakeQuietActive() {
  return (int32_t)(s_handshakeQuietUntilMs - millis()) > 0;
}

static inline uint8_t helloTxFreeSlots() {
  return asyncTxQueueFree();
}

static inline uint8_t helloTxWaitingSlots() {
  return asyncTxQueueWaiting();
}

static inline void extendHandshakeQuiet(const char* cause, uint32_t durMs = HANDSHAKE_TRAFFIC_QUIET_MS) {
  uint32_t now = millis();
  uint32_t until = now + durMs;
  if ((int32_t)(until - s_handshakeQuietUntilMs) > 0) {
    s_handshakeQuietUntilMs = until;
  }
  RIFTLINK_DIAG("HELLO", "event=HANDSHAKE_QUIET cause=%s quiet_ms=%lu",
      cause ? cause : "-", (unsigned long)(s_handshakeQuietUntilMs - now));
}

#define BOOT_PHASE_WAIT_4S  0
#define BOOT_PHASE_DONE     1

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

/** Текущий фиксированный SF (общий для discovery и mesh). Вызывается из radioSchedulerTask. */
uint8_t getDiscoverySf() {
  uint8_t sf = radio::getSpreadingFactor();
  if (sf < 7 || sf > 12) sf = SF_DEFAULT;
  return sf;
}

/** Параметры следующего RX-слота для rxTask. Вызывать из rxTask. */
void getNextRxSlotParams(uint8_t* sfOut, uint32_t* slotMsOut) {
  int nNeigh = neighbors::getCount();
#if !defined(SF_FORCE_7)
  uint8_t sf = getDiscoverySf();
  if (sfOut) *sfOut = sf;
  uint32_t slot = (sf >= 12) ? 1200 : ((sf >= 10) ? 400 : 120);
  // SF7–9: базовый слот 120 ms — при плотном TX/очереди легко не поймать HELLO (2 мин OFF в neighbors).
  // При 0–1 соседе слушаем дольше — лучше перекрытие с периодом HELLO партнёра без раздувания слота при SF12.
  if (nNeigh <= 1 && sf < 10 && slot < 400) {
    slot = 400;
  }
  if (slotMsOut) *slotMsOut = slot;
#else
  if (sfOut) *sfOut = 7;
  if (slotMsOut) *slotMsOut = (nNeigh == 0) ? 800 : 200;
#endif
}

// Буферы для handlePacket (packetTask) — s_fragOutBuf/s_voiceOutBuf слишком велики для стека
static uint8_t s_fragOutBuf[frag::MAX_MSG_PLAIN];
static uint8_t s_voiceOutBuf[voice_frag::MAX_VOICE_PLAIN + 1024];

static bool s_lastButton = false;
static uint32_t s_pressStart = 0;
static uint32_t s_lastProcessedMs = 0;
static uint8_t s_pendingScreen = 0xFF;   // ожидаемая вкладка после REDRAW
static uint32_t s_pendingScreenTime = 0;

static void pollButtonAndQueue() {
  if (!displayQueue) return;
  uint32_t now = millis();

  // Retry: если дисплей не принял смену вкладки за 2.5с — отправить повторно
  if (s_pendingScreen != 0xFF) {
    if (displayGetCurrentScreen() == s_pendingScreen) {
      s_pendingScreen = 0xFF;
    } else if ((now - s_pendingScreenTime) >= PENDING_REDRAW_RETRY_MS) {
      queueDisplayRedraw(s_pendingScreen, true);
      s_pendingScreenTime = now;
    }
  }

  if (displayIsMenuActive()) { s_lastButton = false; return; }

  bool btn = (digitalRead(BUTTON_PIN) == LOW);  // active low
  if (btn) {
    if (!s_lastButton) { s_lastButton = true; s_pressStart = now; }
  } else if (s_lastButton) {
    uint32_t hold = now - s_pressStart;
    s_lastButton = false;
    if (hold < MIN_PRESS_MS) return;
    if (now - s_lastProcessedMs < POST_PRESS_DEBOUNCE_MS) return;  // дребезг — игнор
    s_lastProcessedMs = now;
    if (displayIsSleeping()) {
      queueDisplayWake();
      return;
    }
    int cur = displayGetCurrentScreen();
    if (hold >= SHUTDOWN_PRESS_MS) {
      powersave::deepSleep();
    } else if (hold >= RADIO_MODE_PRESS_MS) {
      if (radio_mode::current() == radio_mode::BLE) {
        if (wifi::hasCredentials()) {
          radio_mode::switchTo(radio_mode::WIFI, radio_mode::STA);
        } else {
          Serial.println("[RadioMode] Button switch skipped: no STA credentials");
        }
      } else {
        radio_mode::switchTo(radio_mode::BLE);
      }
    } else if (hold >= LONG_PRESS_MS) {
      s_pendingScreen = 0xFF;  // long press — не смена вкладки
      queueDisplayLongPress((uint8_t)cur);
    } else {
      uint8_t next = (uint8_t)displayGetNextScreen(cur);
      queueDisplayRedraw(next, true);  // priority — смена вкладки кнопкой в начало очереди
      s_pendingScreen = next;
      s_pendingScreenTime = now;
    }
    queueDisplayLedBlink();
  }
}

struct HelloPlan {
  uint32_t intervalMs;
  uint32_t jitterMs;
  uint32_t phaseOffset;
};

static uint16_t idHash16(const uint8_t* id) {
  if (!id) return 0;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    h ^= id[i];
    h *= 16777619u;
  }
  return (uint16_t)((h >> 16) ^ (h & 0xFFFFu));
}

static uint32_t computePollJitterMs() {
  const uint8_t* self = node::getId();
  uint16_t sid = idHash16(self);
  uint32_t salt = (uint32_t)(self[2] ^ self[5] ^ (sid & 0xFF));
  return POLL_JITTER_BASE_MS + (salt % POLL_JITTER_SPAN_MS);
}

/** Параметры периода HELLO (должны совпадать с логикой в loop до вызова sendHello). */
static HelloPlan computeHelloPlan() {
  HelloPlan p;
  p.intervalMs = HELLO_INTERVAL_MS;
  p.jitterMs = HELLO_JITTER_MS;
  p.phaseOffset = 0;
  int nNeigh = neighbors::getCount();
  if (nNeigh == 0) {
    uint32_t zeroElapsed = (s_zeroNeighSince == 0) ? 0 : (millis() - s_zeroNeighSince);
    if (zeroElapsed >= 300000) p.intervalMs = 18000;
    else if (zeroElapsed >= 120000) p.intervalMs = 15000;
    else p.intervalMs = HELLO_INTERVAL_AGGRESSIVE_MS;
    p.jitterMs = HELLO_JITTER_ZERO_MS;
    p.phaseOffset = beacon_sync::getSlotFor(node::getId()) * 400u;
  } else if (nNeigh == 1) {
    p.intervalMs = HELLO_INTERVAL_ONE_NEIGH_MS;
    p.phaseOffset = beacon_sync::getSlotFor(node::getId()) * 240u;
  } else if (nNeigh >= 6) {
    p.intervalMs = 30000;
    p.phaseOffset = beacon_sync::getSlotFor(node::getId()) * 140u;
  } else {
    p.phaseOffset = beacon_sync::getSlotFor(node::getId()) * 180u;
  }
  return p;
}

static void armNextHelloDeadlineAfterSuccessfulSend() {
  HelloPlan pl = computeHelloPlan();
  int32_t jitter = (int32_t)(esp_random() % (pl.jitterMs * 2)) - (int32_t)pl.jitterMs;
  int64_t due = (int64_t)millis() + (int64_t)pl.intervalMs + (int64_t)pl.phaseOffset + (int64_t)jitter;
  if (due < 0) due = 0;
  s_nextHelloDueMs = (uint32_t)due;
}

/** CAD/queue deferred TX — для HELLO это не «drop». */
static bool helloTxReasonIsCadDefer(const char* sepReason) {
  return sepReason &&
      (strstr(sepReason, "cad_defer") != nullptr || strstr(sepReason, "queue_defer") != nullptr);
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

/** @return true если пакет ушёл в radio (или в очередь TX), false если слишком рано после предыдущего HELLO */
static bool sendHello() {
  uint32_t now = millis();
  int n = neighbors::getCount();
  uint32_t lastKeyTxMs = x25519_keys::getLastKeyTxReadyMs();
  if (lastKeyTxMs != 0 && (now - lastKeyTxMs) < HELLO_QUIET_AFTER_KEY_MS) {
    s_nextHelloDueMs = lastKeyTxMs + HELLO_QUIET_AFTER_KEY_MS;
    RIFTLINK_DIAG("HELLO", "event=HELLO_HOLD cause=recent_key quiet_ms=%lu",
        (unsigned long)(s_nextHelloDueMs - now));
    return false;
  }
  // Дедлайн уже наступил, но ждём retry после drop — сдвинуть due, иначе loop долбит sendHello каждый тик без лога.
  if (now < s_nextHelloAfterDropMs) {
    s_nextHelloDueMs = s_nextHelloAfterDropMs;
    return false;
  }
  // Абсолютный лимит HELLO, чтобы исключить любой шторм при пересчётах дедлайна.
  if (s_lastHelloAirMs != 0 && (now - s_lastHelloAirMs) < HELLO_HARD_MIN_INTERVAL_MS) {
    s_nextHelloDueMs = s_lastHelloAirMs + HELLO_HARD_MIN_INTERVAL_MS;
    RIFTLINK_DIAG("HELLO", "event=HELLO_HOLD cause=hard_rate_limit quiet_ms=%lu",
        (unsigned long)(s_nextHelloDueMs - now));
    return false;
  }
  // Quiet HELLO under active traffic: prioritize ACK/MSG/control first.
  // Prevent HELLO from stealing airtime when queue is tight and CAD busy bursts are present.
  // IMPORTANT: in discovery/bootstrap (0/1 neighbor) HELLO must not be throttled by queue pressure,
  // otherwise two nearby nodes can stay "blind" for minutes after link loss/reboot.
  if (n > 1) {
    uint8_t txFree = helloTxFreeSlots();
    uint8_t txWaiting = helloTxWaitingSlots();
    uint8_t congestion = radio::getCongestionLevel();
    bool tightQueue = (txFree <= 2);
    bool cadBusyBurst = (congestion >= 2);
    if (tightQueue && cadBusyBurst) {
      uint32_t quietMs = 220 + (esp_random() % 220);
      s_nextHelloDueMs = now + quietMs;
      RIFTLINK_DIAG("HELLO", "event=HELLO_HOLD cause=tx_pressure_cad quiet_ms=%lu tx_free=%u tx_waiting=%u congestion=%u",
          (unsigned long)quietMs, (unsigned)txFree, (unsigned)txWaiting, (unsigned)congestion);
      return false;
    }
    if (txWaiting >= 3 && txFree <= 4) {
      uint32_t quietMs = 120 + (esp_random() % 120);
      s_nextHelloDueMs = now + quietMs;
      RIFTLINK_DIAG("HELLO", "event=HELLO_HOLD cause=tx_pressure quiet_ms=%lu tx_free=%u tx_waiting=%u",
          (unsigned long)quietMs, (unsigned)txFree, (unsigned)txWaiting);
      return false;
    }
  }
  uint8_t helloPayload[2];
  uint16_t helloTag = helloSenderTag16(node::getId());
  helloPayload[0] = (uint8_t)(helloTag & 0xFF);
  helloPayload[1] = (uint8_t)(helloTag >> 8);
  uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN + sizeof(helloPayload)];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID,
      31, protocol::OP_HELLO, helloPayload, sizeof(helloPayload));
  if (len == 0) return false;

  bool manyNeighbors = (n >= 6);
  // Bootstrap: при 0/1 соседе HELLO должен проходить в эфир приоритетно,
  // иначе узлы могут "видеть" друг друга только в одну сторону.
  bool priority = manyNeighbors || (n <= 1);

  char helloWhy[56] = {0};
  bool helloSent = false;
#if defined(SF_FORCE_7)
  (void)manyNeighbors;
  if (queueTxPacket(pkt, len, 7, priority, TxRequestClass::control, helloWhy, sizeof helloWhy)) {
    helloSent = true;
  } else {
    queueDeferredSend(pkt, len, 7, 60, true);
    strncpy(helloWhy, "queue_defer", sizeof(helloWhy) - 1);
  }
#else
  if (n > 0) {
    uint8_t txSf = getDiscoverySf();
    if (queueTxPacket(pkt, len, txSf, priority, TxRequestClass::control, helloWhy, sizeof helloWhy)) {
      helloSent = true;
    } else {
      queueDeferredSend(pkt, len, txSf, 60, true);
      strncpy(helloWhy, "queue_defer", sizeof(helloWhy) - 1);
    }
    (void)manyNeighbors;
  } else {
    // Discovery и mesh работают на едином фиксированном SF.
    uint8_t txSf = getDiscoverySf();
    if (queueTxPacket(pkt, len, txSf, priority, TxRequestClass::control, helloWhy, sizeof helloWhy)) {
      helloSent = true;
    } else {
      queueDeferredSend(pkt, len, txSf, 60, true);
      strncpy(helloWhy, "queue_defer", sizeof(helloWhy) - 1);
    }
  }
#endif
  bool viaCadDefer = false;
  if (!helloSent && helloTxReasonIsCadDefer(helloWhy)) {
    helloSent = true;
    viaCadDefer = true;
  }
  if (helloSent) {
    const char* tag = viaCadDefer ? " (deferred/CAD)" : "";
    RIFTLINK_DIAG("HELLO", "event=HELLO_TX ok=1 via=%s sf=%u neighbors=%d reason=%s heap=%u",
        viaCadDefer ? "cad_defer" : "direct", (unsigned)getDiscoverySf(), n,
        helloWhy[0] ? helloWhy : "-", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    Serial.printf("[RiftLink] HELLO sent%s n=%d heap=%u\n", tag,
        n, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  } else {
    RIFTLINK_DIAG("HELLO", "event=HELLO_TX ok=0 sf=%u neighbors=%d cause=%s heap=%u",
        (unsigned)getDiscoverySf(), n, helloWhy[0] ? helloWhy : "unknown",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    Serial.printf("[RiftLink] HELLO drop why=%s n=%d heap=%u\n",
        helloWhy[0] ? helloWhy : "unknown",
        n, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  }
  if (helloSent) {
    s_lastHelloAirMs = millis();
    s_nextHelloAfterDropMs = 0;
    return true;
  }
  // drop: lastHello в loop не сдвигаем; короткая пауза перед следующей попыткой
  s_nextHelloAfterDropMs = now + HELLO_DROP_RETRY_MS;
  return false;
}

/** Только из `radioSchedulerTask` — снимает флаг и выполняет одну попытку HELLO. */
void mainDrainHelloFromScheduler(void) {
  if (!s_helloScheduled.exchange(false, std::memory_order_acq_rel)) return;
  if (!sendHello()) return;
  lastHello = millis();
  armNextHelloDeadlineAfterSuccessfulSend();
}

void sendPoll() {
  uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN_BROADCAST];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_POLL, nullptr, 0);
  if (len == 0) return;
  uint8_t txSf = getDiscoverySf();
  uint32_t jitterMs = computePollJitterMs();
  queueDeferredSend(pkt, len, txSf, jitterMs);
  RIFTLINK_DIAG("HELLO", "event=POLL_TX_DEFER sf=%u delay_ms=%lu cause=scheduled_jitter",
      (unsigned)txSf, (unsigned long)jitterMs);
}

void sendMsg(const uint8_t* to, const char* text, uint8_t ttlMinutes = 0,
    bool critical = false, uint8_t triggerType = msg_queue::TRIGGER_NONE,
    uint32_t triggerValueMs = 0, bool isSos = false) {
  if (text == nullptr || strlen(text) == 0) {
    ble::notifyError("send_empty", "Пустое сообщение не отправляется");
    return;
  }
  const bool isBroadcastTo = (to && memcmp(to, protocol::BROADCAST_ID, protocol::NODE_ID_LEN) == 0);
  // Важно: не блокируем отправку ранним hasKeyFor(), иначе при mutex contention получаем ложный waiting_key.
  // Сначала пытаемся enqueue/шифрование, затем по факту классифицируем причину fail.
  bool ok = isSos ? msg_queue::enqueueSos(text)
                  : msg_queue::enqueue(to, text, ttlMinutes, critical,
                      (msg_queue::TriggerType)triggerType, triggerValueMs);
  if (!ok) {
    msg_queue::SendFailReason reason = msg_queue::getLastSendFailReason();
    if (!isSos && !isBroadcastTo && to &&
        reason == msg_queue::SEND_FAIL_NO_KEY) {
      x25519_keys::sendKeyExchange(to, true, false, "send_wait_key");
      ble::notifyWaitingKey(to);
    } else if (!isSos && !isBroadcastTo && to &&
        reason == msg_queue::SEND_FAIL_KEY_BUSY) {
      ble::notifyError("send_key_busy", "Ключ занят, повторите отправку");
    } else {
      ble::notifyError("send_queue_busy", "Очередь отправки занята, повторите");
    }
  }
  // broadcast "sent" с msgId — через setOnBroadcastSent (сопоставление с delivery)
}

void sendLocation(float lat, float lon, int16_t alt, uint16_t radiusM = 0, uint32_t expiryEpochSec = 0) {
  // Payload: lat (int32), lon (int32), alt (int16), radiusM (u16), expiryEpochSec (u32)
  int32_t lat7 = (int32_t)(lat * 1e7f);
  int32_t lon7 = (int32_t)(lon * 1e7f);
  uint8_t plain[16];
  memcpy(plain, &lat7, 4);
  memcpy(plain + 4, &lon7, 4);
  memcpy(plain + 8, &alt, 2);
  memcpy(plain + 10, &radiusM, 2);
  memcpy(plain + 12, &expiryEpochSec, 4);

  uint8_t encBuf[protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t encLen = sizeof(encBuf);
  if (!crypto::encrypt(plain, sizeof(plain), encBuf, &encLen)) {
    RIFTLINK_LOG_ERR("[RiftLink] Location encrypt FAILED\n");
    ble::notifyError("location_encrypt", "Шифрование локации не удалось");
    return;
  }

  uint8_t pkt[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_LOCATION,
      encBuf, encLen, true, false, false);
  if (len > 0) {
    uint8_t txSf = neighbors::rssiToSf(neighbors::getMinRssi());
    char reasonBuf[40];
    if (!queueTxPacket(pkt, len, txSf, false, TxRequestClass::data, reasonBuf, sizeof(reasonBuf))) {
      queueDeferredSend(pkt, len, txSf, 120, true);
      RIFTLINK_DIAG("GEO", "event=LOCATION_TX_DEFER cause=%s", reasonBuf[0] ? reasonBuf : "?");
    }
  }
}

void handlePacket(const uint8_t* buf, size_t len, int rssi, uint8_t sf) {
  uint8_t decBuf[256];
  uint8_t tmpBuf[256];
  char msgStrBuf[256];
  uint8_t fwdBuf[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];

  protocol::PacketHeader hdr;
  const uint8_t* payload = nullptr;
  size_t payloadLen = 0;
  protocol::ParseResult parseRes;
  static uint32_t s_rxLenMismatchCount = 0;
  if (!protocol::parsePacketEx(buf, len, &hdr, &payload, &payloadLen, &parseRes)) {
    // Strict mode: many short/broken self-echo fragments can trigger parse fail but are not real channel congestion.
    // Escalate congestion only for "substantial" malformed frames that look like real on-air collisions.
    bool hasStrictSync = (len >= 2 && buf[0] == protocol::SYNC_BYTE &&
        (((buf[1] & 0xF0) == protocol::VERSION_STRICT) || ((buf[1] & 0xF0) == protocol::VERSION_V2_PKTID)));
    bool doubleSyncNoise = (len >= 2 && buf[0] == protocol::SYNC_BYTE && buf[1] == protocol::SYNC_BYTE);
    bool substantialFrame = (len >= protocol::HEADER_LEN_BROADCAST + 12);
    bool likelyCollision = hasStrictSync && substantialFrame && !doubleSyncNoise &&
                           ((parseRes.status == protocol::ParseStatus::len_mismatch) ||
                            (parseRes.status == protocol::ParseStatus::payload_range));
    if (likelyCollision) {
      radio::notifyCongestion();
      region::switchChannelOnCongestion();
      collision_slots::recordCollision();
    }
    if (parseRes.status == protocol::ParseStatus::len_mismatch) {
      s_rxLenMismatchCount++;
    }
    RIFTLINK_DIAG("PARSE", "event=RX_PARSE_FAIL status=%s len=%u expected=%u off=%u op=0x%02X pktId=%u rssi=%d sf=%u len_mismatch_total=%lu",
        protocol::parseStatusToString(parseRes.status), (unsigned)len, (unsigned)parseRes.expectedLen,
        (unsigned)parseRes.startOffset, (unsigned)parseRes.opcode, (unsigned)parseRes.pktId, rssi, (unsigned)sf,
        (unsigned long)s_rxLenMismatchCount);
    {
      static const char HEX_CHARS[] = "0123456789ABCDEF";
      constexpr size_t DUMP_BYTES = 24;
      char hexBuf[DUMP_BYTES * 2 + 4];
      size_t nDump = (len < DUMP_BYTES) ? len : DUMP_BYTES;
      size_t w = 0;
      for (size_t i = 0; i < nDump && (w + 2) < sizeof(hexBuf); i++) {
        uint8_t b = buf[i];
        hexBuf[w++] = HEX_CHARS[(b >> 4) & 0x0F];
        hexBuf[w++] = HEX_CHARS[b & 0x0F];
      }
      if (len > nDump && (w + 3) < sizeof(hexBuf)) {
        hexBuf[w++] = '.';
        hexBuf[w++] = '.';
        hexBuf[w++] = '.';
      }
      hexBuf[w] = '\0';
      RIFTLINK_DIAG("PARSE", "event=RX_PARSE_FAIL_HEX len=%u dump=%s",
          (unsigned)len, hexBuf);
    }
#if defined(DEBUG_PACKET_DUMP)
    Serial.printf("[RiftLink] Parse FAIL status=%s len=%u rssi=%d hex=",
        protocol::parseStatusToString(parseRes.status), (unsigned)len, rssi);
    for (size_t i = 0; i < len && i < 32; i++) Serial.printf("%02X", buf[i]);
    Serial.println();
#endif
    return;
  }

  size_t frameOffset = parseRes.startOffset;
  const uint8_t* frameBuf = buf + frameOffset;
  size_t frameLen = len - frameOffset;

  if (hdr.opcode == protocol::OP_KEY_EXCHANGE &&
      memcmp(hdr.from, node::getId(), protocol::NODE_ID_LEN) == 0) {
    return;
  }

#if defined(DEBUG_PACKET_DUMP)
  // Только успешный parsePacket: HELLO=13 B, KEY v2.1=55 B. Если в эфире только len=13 — полный KEY до парсера не дошёл (коллизия/очередь/не принят).
  Serial.printf("[PKT] len=%u rssi=%d sf=%u hex=", (unsigned)frameLen, rssi, sf);
  for (size_t i = 0; i < frameLen && i < 64; i++) Serial.printf("%02X", frameBuf[i]);
  if (frameLen > 64) Serial.print("...");
  Serial.println();
#endif

  // from=broadcast — некорректно, отбрасываем
  if (node::isBroadcast(hdr.from) || node::isInvalidNodeId(hdr.from)) return;
  // Любые self-echo из эфира игнорируем централизованно, чтобы не разгонять ping/hello/key loops.
  if (node::isForMe(hdr.from)) {
    RIFTLINK_DIAG("RADIO", "event=RX_DROP_DUP cause=self_from op=0x%02X pktId=%u",
        (unsigned)hdr.opcode, (unsigned)hdr.pktId);
    return;
  }
  uint32_t nowRxMs = millis();
  if (rxNoiseSeenRecently(hdr, payload, payloadLen, nowRxMs)) {
    rxNoiseDropAccount(hdr.opcode);
    RIFTLINK_DIAG("RADIO", "event=RX_DROP_NOISE op=0x%02X pktId=%u len=%u from=%02X%02X",
        (unsigned)hdr.opcode, (unsigned)hdr.pktId, (unsigned)payloadLen, hdr.from[0], hdr.from[1]);
    rxNoiseMaybeLogSummary(nowRxMs);
    return;
  }

  // OP_XOR_RELAY: попытка декодировать (если есть один из пакетов в кэше)
  if (hdr.opcode == protocol::OP_XOR_RELAY) {
    uint8_t decodedBuf[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
    size_t decodedLen = 0;
    if (network_coding::onXorRelayReceived(frameBuf, frameLen, decodedBuf, &decodedLen) && decodedLen > 0) {
      if (packetQueue && packetPool.ready() && decodedLen <= PACKET_BUF_SIZE) {
        PacketQueueItem* slot = packetPool.alloc();
        if (slot) {
          memcpy(slot->buf, decodedBuf, decodedLen);
          slot->len = (uint16_t)decodedLen;
          slot->rssi = (int8_t)rssi;
          slot->sf = sf;
          if (xQueueSend(packetQueue, &slot, 0) != pdTRUE) {
            packetPool.free(slot);
            RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_queue action=fallback queue=packetQueue reason=full len=%u op=0x%02X rssi=%d sf=%u",
                (unsigned)decodedLen, (unsigned)protocol::OP_XOR_RELAY, rssi, (unsigned)sf);
            handlePacket(decodedBuf, decodedLen, rssi, sf);
          }
        } else {
          handlePacket(decodedBuf, decodedLen, rssi, sf);
        }
      } else {
        handlePacket(decodedBuf, decodedLen, rssi, sf);
      }
    }
    return;
  }

  // HELLO всегда broadcast — иначе сдвиг/коррупция (ghost-соседи)
  if (hdr.opcode == protocol::OP_HELLO && !node::isBroadcast(hdr.to)) return;

  // Relay: unicast не для нас, или GROUP_MSG (broadcast) с TTL>0
  // HELLO — всегда broadcast, не ретранслируем (защита от парсинга с перепутанным to)
  // ROUTE_REQ/REPLY обрабатываются модулем routing
  // Broadcast relay только при 2+ соседях — иначе ping-pong между двумя устройствами, HELLO не проходят
  bool needRelay = (hdr.ttl > 0) && (hdr.opcode != protocol::OP_ROUTE_REQ) &&
      (hdr.opcode != protocol::OP_ROUTE_REPLY) && (hdr.opcode != protocol::OP_HELLO) &&
      (hdr.opcode != protocol::OP_SF_BEACON) && (hdr.opcode != protocol::OP_NACK) && (
      (!node::isForMe(hdr.to) && !node::isBroadcast(hdr.to)) ||
      ((hdr.opcode == protocol::OP_GROUP_MSG || hdr.opcode == protocol::OP_VOICE_MSG || hdr.opcode == protocol::OP_SOS) &&
       neighbors::getCount() >= 2));
  uint32_t relayPayloadHashVal = 0;
  if (needRelay) {
    relayPayloadHashVal = relayPayloadHash(payload ? payload : (const uint8_t*)"", payloadLen);
    if (relayDedupSeen(hdr.from, relayPayloadHashVal)) {
      needRelay = false;
      relayHeard(hdr.from, relayPayloadHashVal);
    }
    if (needRelay) {
      if (hdr.opcode == protocol::OP_SOS) {
        if (relaySosRateLimitExceeded() || relaySosPerSourceLimitExceeded(hdr.from)) needRelay = false;
      } else if (relayRateLimitExceeded()) {
        needRelay = false;
      }
    }
  }
  if (needRelay) {
    relayDedupAdd(hdr.from, relayPayloadHashVal);
    if (hdr.opcode == protocol::OP_SOS) {
      relaySosRateRecord();
      relaySosPerSourceRecord(hdr.from);
    } else relayRateRecord();
    if (hdr.opcode == protocol::OP_MSG && hdr.pktId != 0 && !node::isBroadcast(hdr.to)) {
      pkt_cache::addOverheard(hdr.from, hdr.to, hdr.pktId, frameBuf, frameLen);
      offline_queue::enqueueCourier(frameBuf, frameLen);  // SCF courier для разорванных сегментов
    }

    uint8_t txSf = 0;
    if (node::isBroadcast(hdr.to)) {
      int minRssi = neighbors::getMinRssi();
      txSf = neighbors::rssiToSf(minRssi);
    } else {
      uint8_t nextHop[protocol::NODE_ID_LEN];
      if (routing::getNextHop(hdr.to, nextHop)) {
        int r = neighbors::getRssiFor(nextHop);
        txSf = neighbors::rssiToSf(r);
      }
      if (txSf == 0) txSf = 12;
    }
    int rssiClamp = (rssi < -120) ? -120 : (rssi > -40 ? -40 : rssi);
    int32_t d = 10 * (rssiClamp + 100);
    uint32_t delayMs = (d <= 0) ? 0 : ((d > 500) ? 500 : (uint32_t)d);
    if (hdr.opcode == protocol::OP_SOS) {
      delayMs += 60 + (uint32_t)(esp_random() % 90);  // anti-storm backoff
    }

    bool xorSent = false;
    if (network_coding::addForXor(frameBuf, frameLen, hdr.from, hdr.to)) {
      uint8_t otherFrom[protocol::NODE_ID_LEN];
      uint32_t otherHash;
      network_coding::getLastPairOther(otherFrom, &otherHash);
      relayHeard(otherFrom, otherHash);
      uint8_t xorBuf[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
      size_t xorLen = 0;
      if (network_coding::getXorPacket(xorBuf, sizeof(xorBuf), &xorLen)) {
        queueDeferredSend(xorBuf, xorLen, txSf, delayMs);
        xorSent = true;
      }
    }
    if (!xorSent) {
      uint8_t decodedBuf[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
      size_t decodedLen = 0;
        if (network_coding::getDecodedFromPending(frameBuf, frameLen, hdr.from, hdr.to, hdr.pktId,
                                                  decodedBuf, &decodedLen) && decodedLen > 0) {
        if (packetQueue && packetPool.ready() && decodedLen <= PACKET_BUF_SIZE) {
          PacketQueueItem* slot = packetPool.alloc();
          if (slot) {
            memcpy(slot->buf, decodedBuf, decodedLen);
            slot->len = (uint16_t)decodedLen;
            slot->rssi = (int8_t)rssi;
            slot->sf = sf;
            if (xQueueSend(packetQueue, &slot, 0) != pdTRUE) {
              packetPool.free(slot);
              RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_queue action=fallback queue=packetQueue reason=full len=%u op=0x%02X rssi=%d sf=%u",
                  (unsigned)decodedLen, (unsigned)protocol::OP_XOR_RELAY, rssi, (unsigned)sf);
              handlePacket(decodedBuf, decodedLen, rssi, sf);
            }
          } else {
            handlePacket(decodedBuf, decodedLen, rssi, sf);
          }
        } else {
          handlePacket(decodedBuf, decodedLen, rssi, sf);
        }
      }
      memcpy(fwdBuf, frameBuf, frameLen);
      size_t ttlOff = node::isBroadcast(hdr.to)
          ? ((hdr.pktId != 0) ? 13 : 11)
          : ((hdr.pktId != 0) ? 21 : 19);
      if (ttlOff < frameLen) fwdBuf[ttlOff]--;
      queueDeferredRelay(fwdBuf, frameLen, txSf, delayMs, hdr.from, relayPayloadHashVal);
      if ((esp_random() % 100) < 35) {  // Proof-of-Relay Lite sampling
        ble::notifyRelayProof(node::getId(), hdr.from, hdr.to, hdr.pktId, hdr.opcode);
      }
    }
  }

  if (!node::isForMe(hdr.to) && !node::isBroadcast(hdr.to)) {
    if (hdr.opcode == protocol::OP_NACK && payloadLen >= 2) {
      if (neighbors::isOnline(hdr.from) && !node::isInvalidNodeId(hdr.to)) {
        uint16_t pktId = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
        if (pkt_cache::retransmitOverheard(hdr.from, hdr.to, pktId)) {
          extendHandshakeQuiet("nack_retransmit_overheard");
          RIFTLINK_LOG_EVENT("[RiftLink] Overhear retransmit pktId=%u\n", (unsigned)pktId);
        }
      } else {
        RIFTLINK_DIAG("NACK", "event=NACK_REJECT reason=overhear_source_invalid from=%02X%02X to=%02X%02X",
            hdr.from[0], hdr.from[1], hdr.to[0], hdr.to[1]);
      }
    }
    if (hdr.opcode == protocol::OP_ROUTE_REQ) {
      routing::onRouteReq(hdr.from, payload, payloadLen);
    } else if (hdr.opcode == protocol::OP_ROUTE_REPLY) {
      routing::onRouteReply(hdr.from, hdr.to, payload, payloadLen);
    } else if (hdr.opcode == protocol::OP_KEY_EXCHANGE && payloadLen == 32) {
      // Strict business logic: ignore KEY_EXCHANGE frames not addressed to this node.
      // Replying here can create unnecessary key chatter and delivery regressions.
      RIFTLINK_DIAG("KEY", "event=KEY_RX_SKIP reason=not_for_me from=%02X%02X to=%02X%02X pktId=%u",
          hdr.from[0], hdr.from[1], hdr.to[0], hdr.to[1], (unsigned)hdr.pktId);
    } else if (hdr.opcode == protocol::OP_KEY_EXCHANGE) {
      RIFTLINK_DIAG("KEY", "event=KEY_RX_PARSE_FAIL cause=payload_len_ne_32 from=%02X%02X payload=%u pktId=%u",
          hdr.from[0], hdr.from[1], (unsigned)payloadLen, (unsigned)hdr.pktId);
    } else if (hdr.opcode == protocol::OP_HELLO) {
      if (!validateHelloSenderTag(hdr, payload, payloadLen)) {
        RIFTLINK_DIAG("HELLO", "event=HELLO_DROP reason=tag_invalid from=%02X%02X len=%u",
            hdr.from[0], hdr.from[1], (unsigned)payloadLen);
        return;
      }
      beacon_sync::onBeaconReceived(hdr.from);
      offline_queue::onNodeOnline(hdr.from);
      if (neighbors::onHello(hdr.from, rssi)) {
        ble::requestNeighborsNotify();
        queueDisplayRequestInfoRedraw();  // Paper: обновить вкладку Info при новом соседе
        RIFTLINK_LOG_EVENT("[RiftLink] Neighbor: %02X%02X\n", hdr.from[0], hdr.from[1]);
      }
      if (!x25519_keys::hasKeyFor(hdr.from)) {
        x25519_keys::sendKeyExchange(hdr.from, false, false, "hello_fwd");  // троттл 60с в x25519_keys
      }
    }
    return;
  }

  neighbors::updateRssi(hdr.from, rssi);

  switch (hdr.opcode) {
    case protocol::OP_KEY_EXCHANGE:
      RIFTLINK_DIAG("KEY", "event=KEY_RX_RAW from=%02X%02X len=%u rssi=%d sf=%u pktId=%u",
          hdr.from[0], hdr.from[1], (unsigned)payloadLen, rssi, (unsigned)sf, (unsigned)hdr.pktId);
      if (payloadLen == 32) {
        if (neighbors::onHello(hdr.from, rssi)) {
          ble::requestNeighborsNotify();
          queueDisplayRequestInfoRedraw();  // Paper: обновить вкладку Info
          RIFTLINK_LOG_EVENT("[RiftLink] Neighbor: %02X%02X\n", hdr.from[0], hdr.from[1]);
        }
        bool keyMismatch = x25519_keys::isPeerPubKeyMismatch(hdr.from, payload);
        if (keyMismatch) {
          RIFTLINK_DIAG("KEY", "event=KEY_STORE_FAIL cause=pubkey_mismatch peer=%02X%02X pktId=%u",
              hdr.from[0], hdr.from[1], (unsigned)hdr.pktId);
          RIFTLINK_LOG_ERR("[RiftLink] KEY_EXCHANGE mismatch for %02X%02X — possible key substitution\n",
              hdr.from[0], hdr.from[1]);
          ble::notifyError("invite_peer_key_mismatch", "Peer public key mismatch");
          break;
        }
        bool hadKey = x25519_keys::hasKeyFor(hdr.from);
        if (hadKey) {
          RIFTLINK_DIAG("KEY", "event=KEY_RX_DUP from=%02X%02X pktId=%u action=reply_with_throttle",
              hdr.from[0], hdr.from[1], (unsigned)hdr.pktId);
          extendHandshakeQuiet("key_rx_dup");
          // Peer may have lost pairwise state while keeping the same pubkey.
          // Reply with throttled KEY_EXCHANGE to heal asymmetry without KEY storm.
          x25519_keys::sendKeyExchange(hdr.from, true, true, "key_rx_dup");
          break;
        }
        extendHandshakeQuiet("key_rx_parsed");
        RIFTLINK_DIAG("KEY", "event=KEY_RX_PARSED_OK from=%02X%02X payload=%u pktId=%u hadKey=%u",
            hdr.from[0], hdr.from[1], (unsigned)payloadLen, (unsigned)hdr.pktId, (unsigned)hadKey);
        RIFTLINK_LOG_EVENT("[RiftLink] KEY_EXCHANGE rx parsed payload=%u from %02X%02X (→ onKeyExchange)\n",
            (unsigned)payloadLen, hdr.from[0], hdr.from[1]);
        x25519_keys::onKeyExchange(hdr.from, payload);
        if (x25519_keys::hasKeyFor(hdr.from)) {
          ble::requestNeighborsNotify();  // приложение: обновить hasKey (ожидание ключа → готов)
          x25519_keys::sendKeyExchange(hdr.from, true, false, "key_rx");
        }
      }
      else {
        RIFTLINK_DIAG("KEY", "event=KEY_RX_PARSE_FAIL cause=payload_len_ne_32 from=%02X%02X payload=%u pktId=%u",
            hdr.from[0], hdr.from[1], (unsigned)payloadLen, (unsigned)hdr.pktId);
      }
      break;

    case protocol::OP_SF_BEACON:
      // SF beacon не используется в режиме фиксированного SF.
      break;

    case protocol::OP_HELLO: {
      if (!validateHelloSenderTag(hdr, payload, payloadLen)) {
        RIFTLINK_DIAG("HELLO", "event=HELLO_DROP reason=tag_invalid from=%02X%02X len=%u",
            hdr.from[0], hdr.from[1], (unsigned)payloadLen);
        break;
      }
      RIFTLINK_DIAG("HELLO", "event=HELLO_RX from=%02X%02X rssi=%d sf=%u heap=%u",
          hdr.from[0], hdr.from[1], rssi, (unsigned)sf, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      Serial.printf("[RiftLink] HELLO rx from %02X%02X rssi=%d heap=%u\n",
          hdr.from[0], hdr.from[1], rssi, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      uint32_t freshnessBeforeMs = neighbors::getFreshnessMs(hdr.from);
      bool staleSeenBefore = (freshnessBeforeMs != UINT32_MAX) && (freshnessBeforeMs >= HELLO_STALE_KEY_REFRESH_MS);
      clock_drift::onHelloReceived(hdr.from);
      offline_queue::onNodeOnline(hdr.from);
      bool rediscoveredPeer = false;
      {
        bool wasOnlineBefore = neighbors::isOnline(hdr.from);
        int nBefore = neighbors::getCount();
        if (neighbors::onHello(hdr.from, rssi)) {
          Serial.printf("[RiftLink] Discovery: new neighbor %02X%02X heap=%u\n",
              hdr.from[0], hdr.from[1], (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
          if (nBefore == 0) {
            // Не вызывать sendHello() здесь (packetTask) — только очередь TX из loop
            // и долго держит s_radioMutex — radioScheduler в это время ждёт mutex → «мертвый» mesh, loop залипает.
            s_pendingDiscoveryHello.store(true, std::memory_order_release);
          }
          ble::requestNeighborsNotify();
          queueDisplayRequestInfoRedraw();  // Paper: обновить вкладку Info при новом соседе
          RIFTLINK_LOG_EVENT("[RiftLink] Neighbor: %02X%02X\n", hdr.from[0], hdr.from[1]);
        } else if (!wasOnlineBefore) {
          ble::requestNeighborsNotify();
          rediscoveredPeer = true;
          RIFTLINK_DIAG("NEIGH", "event=NEIGHBOR_REDISCOVER peer=%02X%02X had_key=%u",
              hdr.from[0], hdr.from[1], (unsigned)x25519_keys::hasKeyFor(hdr.from));
        }
      }
      if (!x25519_keys::hasKeyFor(hdr.from)) {
        x25519_keys::sendKeyExchange(hdr.from, false, false, "hello");  // троттл 60с в x25519_keys
      } else if (rediscoveredPeer) {
        RIFTLINK_DIAG("KEY", "event=KEY_REUSE_ATTEMPT peer=%02X%02X reason=hello_rediscover",
            hdr.from[0], hdr.from[1]);
        // Rediscover after peer reboot: push our pubkey immediately to heal asymmetry.
        // Use force first-response path to bypass long throttle/debounce window.
        x25519_keys::sendKeyExchange(hdr.from, true, false, "hello_rediscover");
      } else if (staleSeenBefore) {
        // Peer looked stale right before HELLO refresh; this often means reboot on the other side.
        // Proactively re-announce our pubkey to recover one-sided "A has key, B doesn't" state.
        RIFTLINK_DIAG("KEY", "event=KEY_REUSE_ATTEMPT peer=%02X%02X reason=hello_stale_refresh age_ms=%lu",
            hdr.from[0], hdr.from[1], (unsigned long)freshnessBeforeMs);
        x25519_keys::sendKeyExchange(hdr.from, true, true, "hello_stale_refresh");
      }
      break;
    }

    case protocol::OP_MSG:
      if (payloadLen > 0) {
        RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_packet action=op_msg from=%02X%02X len=%u rssi=%d sf=%u encrypted=%u",
            hdr.from[0], hdr.from[1], (unsigned)payloadLen, rssi, (unsigned)sf, (unsigned)protocol::isEncrypted(hdr));
        if (protocol::isEncrypted(hdr)) {
          size_t decLen = 0;
          if (!crypto::decryptFrom(hdr.from, payload, payloadLen, decBuf, &decLen) || decLen >= 256) {
            RIFTLINK_LOG_ERR("[RiftLink] Decrypt FAILED (from %02X%02X — no key?)\n", hdr.from[0], hdr.from[1]);
            bool hasKeyNow = x25519_keys::hasKeyFor(hdr.from);
            RIFTLINK_DIAG("KEY", "event=KEY_DECRYPT_FAIL type=op_msg from=%02X%02X has_key=%u len=%u pktId=%u",
                hdr.from[0], hdr.from[1], (unsigned)hasKeyNow, (unsigned)payloadLen, (unsigned)hdr.pktId);
            // Self-heal both paths:
            // - no key locally
            // - key exists, but pairwise state may be stale/asymmetric
            x25519_keys::sendKeyExchange(hdr.from, true, hasKeyNow,
                hasKeyNow ? "decrypt_fail_msg" : "msg_no_key");
            break;
          }
          if (protocol::isCompressed(hdr)) {
            size_t d = compress::decompress(decBuf, decLen, tmpBuf, sizeof(tmpBuf));
            if (d == 0 || d >= 256) {
              RIFTLINK_LOG_ERR("[RiftLink] Decompress FAILED\n");
              break;
            }
            memcpy(decBuf, tmpBuf, d);
            decLen = d;
          }
          const char* msg;
          size_t msgLen;
          uint32_t msgId = 0;
          const bool isBroadcastMsg = node::isBroadcast(hdr.to);
          const bool ackEligible = protocol::isAckReq(hdr) &&
                                   decLen >= msg_queue::MSG_ID_LEN &&
                                   node::isForMe(hdr.to) &&
                                   !isBroadcastMsg;
          if (ackEligible) {
            memcpy(&msgId, decBuf, msg_queue::MSG_ID_LEN);
            uint8_t txSf = neighbors::rssiToSfOrthogonal(hdr.from);
            if (txSf == 0) txSf = 12;
            ack_coalesce::add(hdr.from, msgId, txSf);
            // Unicast: ACK only. ECHO witness is reserved for broadcast/group paths.
            msg = (const char*)(decBuf + msg_queue::MSG_ID_LEN);
            msgLen = decLen - msg_queue::MSG_ID_LEN;
          } else {
            if (protocol::isAckReq(hdr) && isBroadcastMsg) {
              RIFTLINK_DIAG("ACK", "event=ACK_SUPPRESSED type=op_msg mode=broadcast_echo_only from=%02X%02X",
                  hdr.from[0], hdr.from[1]);
            }
            msg = (const char*)(decBuf);
            msgLen = decLen;
          }
          if (msgLen < 256) {
            bool skipDisplay = (msgId != 0 && ucDedupSeen(hdr.from, msgId));
            if (!skipDisplay) {
              if (msgId != 0) ucDedupAdd(hdr.from, msgId);
              if (msgLen > 0) {
                memcpy(msgStrBuf, msg, msgLen);
                msgStrBuf[msgLen] = '\0';
              } else {
                strncpy(msgStrBuf, "(пустое)", sizeof(msgStrBuf) - 1);
                msgStrBuf[sizeof(msgStrBuf) - 1] = '\0';
              }
              ble::requestMsgNotify(hdr.from, msgStrBuf, msgId, rssi, 0,
                  (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal", "text");
              RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_packet action=request_msg_notify from=%02X%02X msgId=%u len=%u lane=%s type=text",
                  hdr.from[0], hdr.from[1], (unsigned)msgId, (unsigned)msgLen,
                  (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal");
              char fromHex[17];
              snprintf(fromHex, sizeof(fromHex), "%02X%02X%02X%02X%02X%02X%02X%02X",
                  hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3], hdr.from[4], hdr.from[5], hdr.from[6], hdr.from[7]);
              queueDisplayLastMsg(fromHex, msgStrBuf);
            }
          }
        } else {
          RIFTLINK_DIAG("PARSE", "event=RX_DROP_STRICT reason=msg_not_encrypted from=%02X%02X pktId=%u",
              hdr.from[0], hdr.from[1], (unsigned)hdr.pktId);
        }
      }
      break;

    case protocol::OP_MSG_BATCH:
      if (payloadLen >= 1 && node::isForMe(hdr.to)) {
        uint8_t count = payload[0];
        if (count >= 1 && count <= 4) {
          uint32_t ackMsgIds[4];
          uint8_t ackCount = 0;
          size_t off = 1;
          for (uint8_t i = 0; i < count && off + 2 <= payloadLen; i++) {
            uint16_t encLen = (uint16_t)payload[off] | ((uint16_t)payload[off + 1] << 8);
            off += 2;
            if (encLen == 0 || off + encLen > payloadLen) break;
            const uint8_t* encPtr = payload + off;
            off += encLen;
            size_t decLen = 0;
            if (!crypto::decryptFrom(hdr.from, encPtr, encLen, decBuf, &decLen) || decLen >= 256) {
              bool hasKeyNow = x25519_keys::hasKeyFor(hdr.from);
              RIFTLINK_DIAG("KEY", "event=KEY_DECRYPT_FAIL type=op_msg_batch from=%02X%02X has_key=%u len=%u pktId=%u",
                  hdr.from[0], hdr.from[1], (unsigned)hasKeyNow, (unsigned)encLen, (unsigned)hdr.pktId);
              x25519_keys::sendKeyExchange(hdr.from, true, hasKeyNow,
                  hasKeyNow ? "decrypt_fail_batch" : "msg_batch_no_key");
              continue;
            }
            size_t d = compress::decompress(decBuf, decLen, tmpBuf, sizeof(tmpBuf));
            if (d > 0 && d < 256) {
              memcpy(decBuf, tmpBuf, d);
              decLen = d;
            }
            if (decLen >= msg_queue::MSG_ID_LEN) {
              size_t msgOff = 0;
              uint32_t msgId = 0;
              memcpy(&msgId, decBuf + msgOff, msg_queue::MSG_ID_LEN);
              msgOff += msg_queue::MSG_ID_LEN;
              if (ackCount < 4) ackMsgIds[ackCount++] = msgId;
              size_t msgLen = decLen - msgOff;
              if (msgLen < 256 && !ucDedupSeen(hdr.from, msgId)) {
                ucDedupAdd(hdr.from, msgId);
                memcpy(msgStrBuf, decBuf + msgOff, msgLen);
                msgStrBuf[msgLen] = '\0';
                ble::requestMsgNotify(hdr.from, msgStrBuf, msgId, rssi, 0,
                    (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal", "text");
                RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_packet action=request_msg_notify from=%02X%02X msgId=%u len=%u lane=%s type=text",
                    hdr.from[0], hdr.from[1], (unsigned)msgId, (unsigned)msgLen,
                    (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal");
                char fromHex[17];
                snprintf(fromHex, sizeof(fromHex), "%02X%02X%02X%02X%02X%02X%02X%02X",
                    hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3], hdr.from[4], hdr.from[5], hdr.from[6], hdr.from[7]);
                queueDisplayLastMsg(fromHex, msgStrBuf);
              }
            }
          }
          if (ackCount > 0) {
            uint8_t ackBatchPayload[1 + 4 * 4];
            ackBatchPayload[0] = ackCount;
            for (uint8_t j = 0; j < ackCount; j++)
              memcpy(ackBatchPayload + 1 + j * msg_queue::MSG_ID_LEN, &ackMsgIds[j], msg_queue::MSG_ID_LEN);
            size_t ackBatchPlainLen = 1 + ackCount * msg_queue::MSG_ID_LEN;
            uint8_t ackBatchEnc[1 + 4 * 4 + crypto::OVERHEAD];
            size_t ackBatchEncLen = sizeof(ackBatchEnc);
            if (!crypto::encryptFor(hdr.from, ackBatchPayload, ackBatchPlainLen, ackBatchEnc, &ackBatchEncLen)) {
              RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=encrypt_fail from=%02X%02X type=batch",
                  hdr.from[0], hdr.from[1]);
              break;
            }
            uint8_t ackPkt[protocol::PAYLOAD_OFFSET + 96];
            size_t ackLen = protocol::buildPacket(ackPkt, sizeof(ackPkt),
                node::getId(), hdr.from, 31, protocol::OP_ACK_BATCH,
                ackBatchEnc, ackBatchEncLen, true, false);
            if (ackLen > 0) {
              uint8_t txSf = neighbors::rssiToSfOrthogonal(hdr.from);
              if (txSf == 0) txSf = 12;
              queueDeferredAck(ackPkt, ackLen, txSf, 50);
            }
          }
        }
      }
      break;

    case protocol::OP_ACK:
      if (!node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) {
        RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=bad_direction from=%02X%02X", hdr.from[0], hdr.from[1]);
        break;
      }
      if (!protocol::isEncrypted(hdr)) {
        RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=strict_unencrypted_reject from=%02X%02X", hdr.from[0], hdr.from[1]);
        break;
      }
      if (payloadLen > 0) {
        uint8_t ackPlain[32];
        size_t ackPlainLen = sizeof(ackPlain);
        if (!crypto::decryptFrom(hdr.from, payload, payloadLen, ackPlain, &ackPlainLen) ||
            ackPlainLen != msg_queue::MSG_ID_LEN) {
          RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=decrypt_or_len from=%02X%02X len=%u",
              hdr.from[0], hdr.from[1], (unsigned)payloadLen);
          bool hasKeyNow = x25519_keys::hasKeyFor(hdr.from);
          x25519_keys::sendKeyExchange(hdr.from, true, hasKeyNow,
              hasKeyNow ? "decrypt_fail_ack" : "ack_no_key");
          break;
        }
        uint32_t msgId = 0;
        memcpy(&msgId, ackPlain, msg_queue::MSG_ID_LEN);
        if (msg_queue::onAckReceived(hdr.from, ackPlain, ackPlainLen, false, true, true)) {
          ble::notifyDelivered(hdr.from, msgId, rssi);
        }
      }
      break;

    case protocol::OP_ACK_BATCH:
      if (!node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) {
        RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=batch_bad_direction from=%02X%02X", hdr.from[0], hdr.from[1]);
        break;
      }
      if (!protocol::isEncrypted(hdr)) {
        RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=strict_unencrypted_reject_batch from=%02X%02X", hdr.from[0], hdr.from[1]);
        break;
      }
      if (payloadLen > 0) {
        uint8_t batchPlain[64];
        size_t batchPlainLen = sizeof(batchPlain);
        if (!crypto::decryptFrom(hdr.from, payload, payloadLen, batchPlain, &batchPlainLen)) {
          RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=batch_decrypt_fail from=%02X%02X len=%u",
              hdr.from[0], hdr.from[1], (unsigned)payloadLen);
          bool hasKeyNow = x25519_keys::hasKeyFor(hdr.from);
          x25519_keys::sendKeyExchange(hdr.from, true, hasKeyNow,
              hasKeyNow ? "decrypt_fail_ack_batch" : "ack_batch_no_key");
          break;
        }
        msg_queue::onAckBatchReceived(hdr.from, batchPlain, batchPlainLen, rssi, ble::notifyDelivered);
      }
      break;

    case protocol::OP_ECHO:
      // Echo Protocol: broadcast msgId+originalFrom — отправитель принимает как ACK
      if (payloadLen >= 12 && memcmp(payload + msg_queue::MSG_ID_LEN, node::getId(), protocol::NODE_ID_LEN) == 0) {
        uint32_t msgId;
        memcpy(&msgId, payload, msg_queue::MSG_ID_LEN);
        (void)msg_queue::onBroadcastAckWitness(hdr.from, msgId, false);
      } else {
        RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=echo_payload_or_target from=%02X%02X len=%u",
            hdr.from[0], hdr.from[1], (unsigned)payloadLen);
      }
      break;

    case protocol::OP_POLL:
      // RIT: получатель шлёт «присылайте пакеты» — ускоряем отправку pending для него
      msg_queue::onPollReceived(hdr.from);
      break;

    case protocol::OP_READ:
      if (payloadLen >= msg_queue::MSG_ID_LEN) {
        uint32_t msgId;
        memcpy(&msgId, payload, msg_queue::MSG_ID_LEN);
        ble::notifyRead(hdr.from, msgId, rssi);
      }
      break;

    case protocol::OP_TELEMETRY:
      if (payloadLen > 0 && protocol::isEncrypted(hdr)) {
        uint8_t tbuf[64];
        size_t tlen = 0;
        if (crypto::decrypt(payload, payloadLen, tbuf, &tlen) && tlen >= 4) {
          uint16_t batteryMv = 0;
          uint16_t heapKb = 0;
          memcpy(&batteryMv, tbuf, 2);
          memcpy(&heapKb, tbuf + 2, 2);
          neighbors::updateBattery(hdr.from, batteryMv);
          if (node::isForMe(hdr.from)) {
            ble::notifyTelemetry(hdr.from, batteryMv, heapKb, rssi);
          } else {
            RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_filter action=skip evt=telemetry from=%02X%02X",
                hdr.from[0], hdr.from[1]);
          }
        }
      }
      break;

    case protocol::OP_LOCATION:
      if (payloadLen > 0 && protocol::isEncrypted(hdr)) {
        uint8_t decBuf[32];
        size_t decLen = 0;
        if (crypto::decrypt(payload, payloadLen, decBuf, &decLen) && decLen >= 10) {
          int32_t lat7, lon7;
          int16_t alt;
          memcpy(&lat7, decBuf, 4);
          memcpy(&lon7, decBuf + 4, 4);
          memcpy(&alt, decBuf + 8, 2);
          float lat = float(lat7) / 1e7f;
          float lon = float(lon7) / 1e7f;
          uint32_t nowMs = millis();
          if (isLocationSpoofLike(hdr.from, lat, lon, nowMs)) {
            RIFTLINK_LOG_ERR("[RiftLink] LOCATION dropped (anti-spoof) from %02X%02X lat=%.5f lon=%.5f\n",
                hdr.from[0], hdr.from[1], lat, lon);
            break;
          }
          bool inGeo = true;
          if (decLen >= 16) {
            uint16_t radiusM = 0;
            uint32_t expiryEpochSec = 0;
            memcpy(&radiusM, decBuf + 10, 2);
            memcpy(&expiryEpochSec, decBuf + 12, 4);
            if (radiusM > GEOFENCE_RADIUS_MAX_M) {
              RIFTLINK_LOG_ERR("[RiftLink] LOCATION dropped (radius too large=%u) from %02X%02X\n",
                  (unsigned)radiusM, hdr.from[0], hdr.from[1]);
              break;
            }
            if (expiryEpochSec > 0 && expiryEpochSec < 1700000000UL) {
              RIFTLINK_LOG_ERR("[RiftLink] LOCATION dropped (bad expiry=%u) from %02X%02X\n",
                  (unsigned)expiryEpochSec, hdr.from[0], hdr.from[1]);
              break;
            }
            if (expiryEpochSec > 0) {
              uint32_t nowEpoch = 0;
              if (gps::getEpochSec(&nowEpoch)) {
                if (nowEpoch > expiryEpochSec) inGeo = false;
              } else {
                inGeo = true;  // без epoch-времени не отбрасываем пакет
              }
            }
            if (inGeo && radiusM > 0 && gps::hasFix()) {
              float dLat = (gps::getLat() - lat) * 111000.0f;
              float dLon = (gps::getLon() - lon) * 111000.0f;
              float dist2 = dLat * dLat + dLon * dLon;
              inGeo = dist2 <= (float)radiusM * (float)radiusM;
            }
          }
          if (inGeo) {
            rememberLocationTrust(hdr.from, lat, lon, nowMs);
            if (node::isForMe(hdr.from)) {
              ble::notifyLocation(hdr.from, lat, lon, alt, rssi);
            } else {
              RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_filter action=skip evt=location from=%02X%02X",
                  hdr.from[0], hdr.from[1]);
            }
          }
        }
      }
      break;

    case protocol::OP_SOS:
      if (payloadLen > 0 && protocol::isEncrypted(hdr)) {
        size_t decLen = 0;
        if (crypto::decrypt(payload, payloadLen, decBuf, &decLen) && decLen >= msg_queue::MSG_ID_LEN) {
          uint32_t msgId = 0;
          memcpy(&msgId, decBuf, msg_queue::MSG_ID_LEN);
          size_t msgLen = decLen - msg_queue::MSG_ID_LEN;
          if (msgLen > 0 && msgLen < sizeof(msgStrBuf)) {
            memcpy(msgStrBuf, decBuf + msg_queue::MSG_ID_LEN, msgLen);
            msgStrBuf[msgLen] = '\0';
            ble::requestMsgNotify(hdr.from, msgStrBuf, msgId, rssi, 0, "critical", "sos");
            RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_packet action=request_msg_notify from=%02X%02X msgId=%u len=%u lane=critical type=sos",
                hdr.from[0], hdr.from[1], (unsigned)msgId, (unsigned)msgLen);
          }
        }
      }
      break;

    case protocol::OP_PING:
      if (node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) {
        uint32_t nowMs = millis();
        uint16_t pingKey = hdr.pktId;
        // Для ping без pktId используем deterministic fallback-ключ, чтобы не отвечать штормом.
        if (pingKey == 0) {
          pingKey = (uint16_t)(((uint16_t)hdr.from[0] << 8) | (uint16_t)hdr.from[1]);
        }
        if (pingReplySeenRecently(hdr.from, pingKey, nowMs)) {
          RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=ping_dup_guard from=%02X%02X pktId=%u",
              hdr.from[0], hdr.from[1], (unsigned)pingKey);
          break;
        }
        pingReplyMarkSeen(hdr.from, pingKey, nowMs);
        // Эхо pktId из OP_PING в OP_PONG — на приёмнике совпадение с исходным пингом по эфиру.
        uint8_t pongPkt[protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID];
        size_t pongLen = protocol::buildPacket(pongPkt, sizeof(pongPkt),
            node::getId(), hdr.from, 31, protocol::OP_PONG, nullptr, 0,
            false, false, false, protocol::CHANNEL_DEFAULT, hdr.pktId);
        if (pongLen > 0) {
          uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(hdr.from));
          if (txSf == 0) txSf = 12;
          char reasonBuf[40];
          if (!queueTxPacket(pongPkt, pongLen, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
            queueDeferredSend(pongPkt, pongLen, txSf, 50, true);  // deferred send при полной radioCmdQueue
            RIFTLINK_DIAG("PING", "event=PONG_TX_DEFER to=%02X%02X cause=%s",
                hdr.from[0], hdr.from[1], reasonBuf[0] ? reasonBuf : "?");
          }
        }
      }
      break;

    case protocol::OP_ROUTE_REQ:
      routing::onRouteReq(hdr.from, payload, payloadLen);
      break;

    case protocol::OP_ROUTE_REPLY:
      if (routing::onRouteReply(hdr.from, hdr.to, payload, payloadLen) && node::isForMe(hdr.to)) {
        ble::notifyRoutes();
      }
      break;

    case protocol::OP_PONG:
      ble::clearPingRetryForPeer(hdr.from);
      ble::notifyPong(hdr.from, rssi, hdr.pktId);
      break;

    case protocol::OP_NACK:
      if (payloadLen >= 2 && node::isForMe(hdr.to)) {
        if (!neighbors::isOnline(hdr.from)) {
          RIFTLINK_DIAG("NACK", "event=NACK_REJECT reason=from_offline from=%02X%02X", hdr.from[0], hdr.from[1]);
          break;
        }
        uint16_t pktId = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
        if (pkt_cache::retransmitOnNack(hdr.from, pktId)) {
          extendHandshakeQuiet("nack_retransmit");
          region::switchChannelOnCongestion();  // Channel Hopping при NACK
          RIFTLINK_LOG_EVENT("[RiftLink] NACK retransmit pktId=%u to %02X%02X\n",
              (unsigned)pktId, hdr.from[0], hdr.from[1]);
        }
      } else if (payloadLen >= 2) {
        RIFTLINK_DIAG("NACK", "event=NACK_REJECT reason=bad_direction from=%02X%02X", hdr.from[0], hdr.from[1]);
      }
      break;

    case protocol::OP_GROUP_MSG:
      if (payloadLen > 0 && protocol::isEncrypted(hdr)) {
        if (node::isForMe(hdr.from)) break;  // своё сообщение (relay echo) — пропуск
        size_t decLen = 0;
        bool decrypted = crypto::decrypt(payload, payloadLen, decBuf, &decLen);
        if (!decrypted) {
          // V2-only: пробуем только активные V2 group keys.
          const int v2Count = groups::getV2Count();
          for (int gi = 0; gi < v2Count; gi++) {
            char groupUid[groups::GROUP_UID_MAX_LEN + 1] = {0};
            char groupTag[groups::GROUP_TAG_MAX_LEN + 1] = {0};
            uint32_t channelId32 = 0;
            if (!groups::getV2At(gi, groupUid, sizeof(groupUid), &channelId32, groupTag, sizeof(groupTag), nullptr, 0, nullptr, nullptr, nullptr, nullptr)) continue;
            if (channelId32 == 0) continue;
            uint8_t gk[32];
            if (!groups::getGroupKeyV2(groupUid, gk, nullptr)) continue;
            size_t tryLen = 0;
            if (!crypto::decryptWithGroupKey(gk, payload, payloadLen, tmpBuf, &tryLen) || tryLen < GROUP_ID_LEN) continue;
            size_t plainLen = tryLen;
            uint8_t* plainPtr = tmpBuf;
            if (protocol::isCompressed(hdr)) {
              size_t d = compress::decompress(tmpBuf, tryLen, decBuf, sizeof(decBuf));
              if (d == 0 || d < GROUP_ID_LEN) continue;
              plainPtr = decBuf;
              plainLen = d;
            }
            uint32_t testGroupId = 0;
            memcpy(&testGroupId, plainPtr, GROUP_ID_LEN);
            if (testGroupId != channelId32) continue;
            memcpy(decBuf, plainPtr, plainLen);
            decLen = plainLen;
            decrypted = true;
            break;
          }
        } else if (protocol::isCompressed(hdr)) {
          size_t d = compress::decompress(decBuf, decLen, tmpBuf, sizeof(tmpBuf));
          if (d == 0 || d < GROUP_ID_LEN) break;
          memcpy(decBuf, tmpBuf, d);
          decLen = d;
        }
        if (!decrypted || decLen < GROUP_ID_LEN) break;
        uint32_t groupId;
        memcpy(&groupId, decBuf, GROUP_ID_LEN);
        char groupUid[groups::GROUP_UID_MAX_LEN + 1] = {0};
        // GROUP_ALL — служебный id широковещательных сообщений; не хранится в списке подписок
        if (groupId != groups::GROUP_ALL &&
            !groups::findGroupUidByChannelV2(groupId, groupUid, sizeof(groupUid))) {
          break;
        }
        const char* msg;
        size_t msgLen;
        uint32_t appMsgId = 0;
        if (decLen >= GROUP_ID_LEN + msg_queue::MSG_ID_LEN) {
          uint32_t bcMsgId;
          memcpy(&bcMsgId, decBuf + GROUP_ID_LEN, msg_queue::MSG_ID_LEN);
          if (bcDedupSeen(hdr.from, bcMsgId)) break;  // дубликат broadcast (3x repeat)
          bcDedupAdd(hdr.from, bcMsgId);
          appMsgId = bcMsgId;
          msg = (const char*)(decBuf + GROUP_ID_LEN + msg_queue::MSG_ID_LEN);
          msgLen = decLen - GROUP_ID_LEN - msg_queue::MSG_ID_LEN;
          // strict mode: для group/broadcast подтверждение только через ECHO witness (без ACK).
          uint8_t echoPayload[12];
          memcpy(echoPayload, &bcMsgId, msg_queue::MSG_ID_LEN);
          memcpy(echoPayload + msg_queue::MSG_ID_LEN, hdr.from, protocol::NODE_ID_LEN);
          uint8_t echoPkt[protocol::PAYLOAD_OFFSET + 32];
          size_t echoLen = protocol::buildPacket(echoPkt, sizeof(echoPkt),
              node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_ECHO,
              echoPayload, sizeof(echoPayload), false, false);
          if (echoLen > 0) {
            uint8_t echoSf = neighbors::rssiToSf(neighbors::getMinRssi());
            if (echoSf == 0) echoSf = 12;
            uint32_t echoDelay = 220 + (esp_random() % 160);
            queueDeferredSend(echoPkt, echoLen, echoSf, echoDelay);
            RIFTLINK_DIAG("ACK", "event=ACK_SUPPRESSED type=group_msg mode=echo_only from=%02X%02X delay=%lu",
                hdr.from[0], hdr.from[1], (unsigned long)echoDelay);
          }
        } else {
          msg = (const char*)(decBuf + GROUP_ID_LEN);
          msgLen = decLen - GROUP_ID_LEN;
        }
        if (msgLen < 256) {
          memcpy(msgStrBuf, msg, msgLen);
          msgStrBuf[msgLen] = '\0';
          ble::requestMsgNotify(hdr.from, msgStrBuf, appMsgId, rssi, 0,
              (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal", "text",
              groupId, groupUid[0] ? groupUid : nullptr);
          RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_packet action=request_msg_notify from=%02X%02X msgId=%u len=%u lane=%s type=text",
              hdr.from[0], hdr.from[1], (unsigned)appMsgId, (unsigned)msgLen,
              (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal");
          char fromHex[17];
          snprintf(fromHex, sizeof(fromHex), "grp%u %02X%02X", (unsigned)groupId, hdr.from[0], hdr.from[1]);
          queueDisplayLastMsg(fromHex, msgStrBuf);
        }
      }
      break;

    case protocol::OP_MSG_FRAG:
      if (payloadLen >= 6) {
        size_t outLen = 0;
        uint32_t fragMsgId = 0;
        if (frag::onFragment(hdr.from, hdr.to, payload, payloadLen,
                             protocol::isCompressed(hdr), s_fragOutBuf, sizeof(s_fragOutBuf), &outLen, &fragMsgId)) {
          if (outLen > 0 && outLen < sizeof(s_fragOutBuf)) {
            s_fragOutBuf[outLen] = '\0';
            const char* msgStr = (const char*)s_fragOutBuf;
            if (fragMsgId == 0) {
              RIFTLINK_DIAG("FRAG", "event=FRAG_RX_DROP reason=msg_id_missing from=%02X%02X len=%u",
                  hdr.from[0], hdr.from[1], (unsigned)outLen);
              break;
            }
            ble::requestMsgNotify(hdr.from, msgStr, fragMsgId, rssi, 0,
                (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal", "text");
            RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_packet action=request_msg_notify from=%02X%02X msgId=%u len=%u lane=%s type=text",
                hdr.from[0], hdr.from[1], (unsigned)fragMsgId, (unsigned)outLen,
                (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal");
            char fromHex[17];
            snprintf(fromHex, sizeof(fromHex), "%02X%02X%02X%02X%02X%02X%02X%02X",
                hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3], hdr.from[4], hdr.from[5], hdr.from[6], hdr.from[7]);
            queueDisplayLastMsg(fromHex, msgStr);
          }
        }
      }
      break;

    case protocol::OP_VOICE_MSG:
      if (payloadLen >= 6 && node::isForMe(hdr.to) && ble::isConnected()) {
        size_t outLen = 0;
        if (voice_frag::onFragment(hdr.from, hdr.to, payload, payloadLen,
                                  s_voiceOutBuf, sizeof(s_voiceOutBuf), &outLen)) {
          if (outLen > 0) {
            ble::notifyVoice(hdr.from, s_voiceOutBuf, outLen);
          }
        }
      }
      break;

    default:
      break;
  }
}

/** Callback из radioSchedulerTask: после TX на SF12 — следующий RX слот на SF12 (V4). */
void onRadioSchedulerTxSf12(void) {
  // Фиксированный SF: отдельный "подхват SF12 после TX" не нужен.
}

#if defined(USE_EINK)
#define VEXT_PIN 45
#define VEXT_ON_LEVEL LOW
#endif

/** Задержка с yield — не блокировать планировщик */
static void delayYield(uint32_t ms) {
  for (uint32_t t = millis(); millis() - t < ms;) yield();
}

void setup() {
  Serial.begin(115200);
  delayYield(300);  // дать USB CDC / UART стабилизироваться
  Serial.println("[RiftLink] boot...");
#if defined(ARDUINO_LILYGO_T_LORA_PAGER)
  lilygoTpagerEarlyInit();
#endif
  pinMode(BUTTON_PIN, INPUT_PULLUP);  // USER_SW — до displayInit, чтобы кнопка работала на Paper
  ledInit(LED_PIN);
#if defined(USE_EINK)
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, VEXT_ON_LEVEL);
  delayYield(300);  // стабилизация питания E-Ink
#endif
#if LED_PIN
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH);
    delayYield(100);
    digitalWrite(LED_PIN, LOW);
    delayYield(100);
  }
#endif
  delayYield(200);

  // Default event loop до любого esp_wifi_* — иначе "failed to post WiFi event=2 ret=259" (WIFI_EVENT_259_ANALYSIS.md)
  esp_err_t ev = esp_event_loop_create_default();
  if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) {
    Serial.printf("[RiftLink] Event loop: %s\n", esp_err_to_name(ev));
  }

  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs = nvs_flash_init();
  }
  if (nvs != ESP_OK) {
    RIFTLINK_LOG_ERR("[RiftLink] NVS init FAILED: %s (0x%x) — настройки не сохранятся\n",
        esp_err_to_name(nvs), (unsigned)nvs);
  }
  // Meshtastic: malloc > порога уходит в SPIRAM — меньше «дырявого» internal до NimBLE/esp_wifi (Arduino 3 / IDF 5).
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
    // Меньше порог → больше аллокаций в SPIRAM до NimBLE/Wi‑Fi; после async остаётся internal под UART (GPS)
    heap_caps_malloc_extmem_enable(128);
  }
  // Time-sharing: WiFi НЕ запускается при загрузке (Mode A: BLE-only, ~55K free heap).
  // WiFi инициализируется on-demand через radio_mode::switchTo(WIFI).
  locale::init();
  displayInit();
  Serial.printf("[RiftLink] Heap after displayInit: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  displayShowBootScreen();
  s_bootPhase = BOOT_PHASE_WAIT_4S;
  s_bootPhaseStart = millis();
  // 4s ожидание перенесено в loop — displayUpdate() во время загрузки
}

static void runBootStateMachine() {
  if (s_bootPhase != BOOT_PHASE_WAIT_4S) return;
  if (millis() - s_bootPhaseStart < 4000) {
    displayUpdate();
    return;
  }
  s_bootPhase = BOOT_PHASE_DONE;
  if (!locale::isSet()) {
    displayShowLanguagePicker();
  }
  displayClear();
  displaySetTextSize(1);
  displayText(0, 0, locale::getForDisplay("init"));
  displayShow();
  node::init();
  region::init();
  crypto::init();
  x25519_keys::init();
  if (!radio::init()) {
    RIFTLINK_LOG_ERR("[RiftLink] Radio init FAILED\n");
    displayText(0, 10, locale::getForDisplay("radio_fail"));
    displayShow();
  } else {
    displayText(0, 10, locale::getForDisplay("radio_ok"));
    displayShow();
  }
  Serial.printf("[RiftLink] Heap after radio::init: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  if (!selftest::quickAntennaCheck()) {
    displayShowWarning(locale::getForDisplay("antenna_warn"), locale::getForDisplay("antenna_check"), 3000);
  }
  if (!region::isSet()) {
    displayShowRegionPicker();
  }
  if (!ble::init()) {
    RIFTLINK_LOG_ERR("[RiftLink] BLE init FAILED — устройство не будет видно в скане\n");
  }
  Serial.printf("[RiftLink] Heap after ble::init: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  memoryDiagLog("ble");
  ble::setOnSend(sendMsg);
  ble::setOnLocation(sendLocation);
  msg_queue::init();
  ack_coalesce::init();
  mab::init();
  collision_slots::init();
  beacon_sync::init();
  clock_drift::init();
  bls_n::init();
  packet_fusion::init();
  packet_fusion::setOnBatchSent([](const uint8_t* to, const uint32_t* msgIds, int count) {
    msg_queue::registerBatchSent(to, msgIds, count);
  });
  packet_fusion::setOnSingleFlush([](const uint8_t* to, uint32_t msgId, const uint8_t* pkt, size_t pktLen, uint8_t txSf) {
    return msg_queue::registerPendingFromFusion(to, msgId, pkt, pktLen, txSf);
  });
  network_coding::init();
  Serial.printf("[RiftLink] Heap after packet_fusion+network_coding: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  msg_queue::setOnUnicastSent([](const uint8_t* to, uint32_t msgId) { ble::notifySent(to, msgId); });
  msg_queue::setOnUnicastUndelivered([](const uint8_t* to, uint32_t msgId) {
    radio::notifyCongestion();
    ble::notifyUndelivered(to, msgId);
  });
  msg_queue::setOnBroadcastSent([](uint32_t msgId) { ble::notifySent(protocol::BROADCAST_ID, msgId); });
  msg_queue::setOnBroadcastDelivery([](uint32_t msgId, int d, int t) { ble::notifyBroadcastDelivery(msgId, d, t); });
  msg_queue::setOnTimeCapsuleReleased([](const uint8_t* to, uint32_t msgId, uint8_t triggerType) {
    ble::notifyTimeCapsuleReleased(to, msgId, triggerType);
  });
  frag::init();
  Serial.printf("[RiftLink] Heap after frag::init: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  telemetry::init();
  neighbors::init();
  Serial.printf("[RiftLink] Heap after neighbors::init: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  pkt_cache::init();
  groups::init();
  offline_queue::init();
  routing::init();
  Serial.printf("[RiftLink] Heap after routing::init: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  // voice_frag: lazy-init при первом VOICE_MSG (send / onFragment); deinit() при необходимости освободит слоты
  powersave::init();
  // Очереди + packet/display/radio tasks — сразу после send_overflow (V3/V4/Paper): иначе первый TX/NACK на
  // обрезанный KEY вызывал asyncInfraEnsure() в середине discovery (heap «прыгал», окно RX терялось).
  send_overflow::init();
  Serial.printf("[RiftLink] Heap after send_overflow::init: %u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  Serial.printf("[RiftLink] Heap before async infra: %u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  if (!asyncInfraEnsure()) {
    RIFTLINK_LOG_ERR("[RiftLink] Async infra init FAILED at boot\n");
  }
  memoryDiagLog("async_infra");
  asyncMemoryDiagLogStacks();
  // ESP-NOW и WiFi connect — только в WiFi-режиме (Mode B), не при загрузке.
  memoryDiagLog("boot_done");
  gps::init();
  displayShowScreenForceFull(0);
  s_bootTime = millis();
  s_lastKeyRetry = millis() + (node::getId()[0] % 16) * 500;
}

void loop() {
  if (s_bootPhase != BOOT_PHASE_DONE) {
    runBootStateMachine();
    return;
  }

  // HELLO: сначала смена плана по числу соседей — до pending discovery. Иначе pending делает
  // sendHello+armNextHelloDeadline (~+8с), а блок ниже перезаписывал s_nextHelloDueMs=millis() и во втором
  // sendHello в том же проходе срабатывал HELLO_MIN_AIR_GAP без лога («HELLO один раз и тишина»).
  int nNeigh = neighbors::getCount();
  uint32_t keyTxMs = x25519_keys::getLastKeyTxReadyMs();
  if (keyTxMs != 0 && keyTxMs != s_lastObservedKeyTxMs) {
    s_lastObservedKeyTxMs = keyTxMs;
    uint32_t now = millis();
    uint32_t until = keyTxMs + HANDSHAKE_TRAFFIC_QUIET_MS;
    if ((int32_t)(until - s_handshakeQuietUntilMs) > 0) {
      s_handshakeQuietUntilMs = until;
      RIFTLINK_DIAG("HELLO", "event=HANDSHAKE_QUIET cause=key_tx quiet_ms=%lu",
          (unsigned long)(s_handshakeQuietUntilMs - now));
    }
  }
  if (nNeigh == 0) {
    if (s_zeroNeighSince == 0) s_zeroNeighSince = millis();
    s_oneNeighSince = 0;
  } else if (nNeigh == 1) {
    s_zeroNeighSince = 0;
    if (s_oneNeighSince == 0) s_oneNeighSince = millis();
  } else {
    s_zeroNeighSince = 0;  // появился сосед — сбросить backoff
    s_oneNeighSince = 0;
  }
  if (nNeigh != s_helloPlannerLastN) s_helloPlannerLastN = nNeigh;

  // Отложенный discovery HELLO (см. OP_HELLO в handlePacket) — постановка в radioSchedulerTask.
  if (s_pendingDiscoveryHello.exchange(false, std::memory_order_acq_rel)) {
    scheduleHelloTx();
  }
  if (millis() < s_loopCooldownUntil) {
    vTaskDelay(pdMS_TO_TICKS(1));
    return;
  }
  // Во время handoff BLE<->WiFi убираем периодический mesh-трафик:
  // это снижает фрагментацию heap и повышает шанс успешного BLE re-init.
  if (!radio_mode::isSwitching()) {
    // HELLO: 0 сосед — backoff 8с -> 15с -> 30с; 1+ сосед — 30с; 6+ — 24с (меньше спама)
    // Фазирование по слоту (beacon_sync): при 0 соседях каждый узел смещён на свой слот — меньше storm
    // Дедлайн следующего HELLO + один джиттер на период. Раньше: новый esp_random() каждый проход loop
    // → при millis()-lastHello чуть выше минимума ~1/6 попыток каждые 10 ms проходили → шторм «HELLO sent».
    if (!isHandshakeQuietActive() && (int32_t)(millis() - s_nextHelloDueMs) >= 0) {
      scheduleHelloTx();
    }
    // POLL нужен в mesh. При 0 соседях он только зашумляет эфир и мешает discovery на SF12.
    if (AUTO_POLL_ENABLED && !isHandshakeQuietActive() && nNeigh > 1 && millis() - lastPoll > POLL_INTERVAL_MS) {
      sendPoll();
      lastPoll = millis();
    }
  }
  if (AUTO_TELEMETRY_ENABLED && !isHandshakeQuietActive() && millis() - lastTelemetry > TELEM_INTERVAL_MS) {
    telemetry::send();
    lastTelemetry = millis();
  }

  // SF фиксированный: не меняем автоматически и не шлём SF beacon.

  // Time-sharing: ESP-NOW tick только в WiFi-режиме
  if (radio_mode::current() == radio_mode::WIFI) {
    esp_now_slots::tickAdaptive();
  }

  // Radio mode switch (deferred from BLE callback / button)
  radio_mode::update();

  // WebSocket server tick (WiFi mode)
  ws_server::update();

  gps::update();
  if (gps::isPresent() && gps::isEnabled() && gps::hasFix() &&
      millis() - lastGpsLoc > GPS_LOC_INTERVAL_MS) {
    sendLocation(gps::getLat(), gps::getLon(), gps::getAlt());
    lastGpsLoc = millis();
  }

  // Serial: "send <text>" = broadcast; "send <hex16> <text>" = to node
  if (Serial.available()) {
    Serial.setTimeout(50);  // не блокировать 1s — displayUpdate должен вызываться чаще
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    auto parseNodeIdHex16 = [](const String& hex, uint8_t out[protocol::NODE_ID_LEN]) -> bool {
      if (hex.length() != (int)(protocol::NODE_ID_LEN * 2)) return false;
      for (int i = 0; i < (int)hex.length(); i++) {
        char c = hex.charAt(i);
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
      }
      for (int i = 0; i < (int)protocol::NODE_ID_LEN; i++) {
        out[i] = (uint8_t)strtoul(hex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
      }
      return true;
    };
    if (cmd.startsWith("send ")) {
      String rest = cmd.substring(5);
      int sp = rest.indexOf(' ');
      String tok1 = sp >= 0 ? rest.substring(0, sp) : rest;
      String text = sp >= 0 ? rest.substring(sp + 1) : "";
      uint8_t to[protocol::NODE_ID_LEN];
      bool isHex16 = parseNodeIdHex16(tok1, to);
      if (isHex16 && text.length() > 0) {
        sendMsg(to, text.c_str(), 0);
      } else if (rest.length() > 0) {
        sendMsg(protocol::BROADCAST_ID, rest.c_str(), 0);
      }
    } else if (cmd.startsWith("ping ")) {
      String hex16 = cmd.substring(5);
      hex16.trim();
      uint8_t to[protocol::NODE_ID_LEN];
      if (parseNodeIdHex16(hex16, to)) {
        uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN];
        size_t len = protocol::buildPacket(pkt, sizeof(pkt),
            node::getId(), to, 31, protocol::OP_PING, nullptr, 0);
        bool pingSent = false;
        if (len > 0) {
          uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(to));
          char reasonBuf[40];
          if (queueTxPacket(pkt, len, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
            pingSent = true;
          } else {
            queueDeferredSend(pkt, len, txSf, 60, true);
            RIFTLINK_DIAG("PING", "event=PING_TX_DEFER mode=serial to=%02X%02X cause=%s",
                to[0], to[1], reasonBuf[0] ? reasonBuf : "?");
            pingSent = true;  // deferred fallback scheduled
          }
        }
        if (pingSent) {
          Serial.printf("[RiftLink] PING sent to %s\n", hex16.c_str());
        } else {
          Serial.println("[RiftLink] PING failed");
        }
      } else {
        Serial.println("[RiftLink] ping <hex16>");
      }
    } else if (cmd.startsWith("region ")) {
      String r = cmd.substring(7);
      r.trim();
      if (region::setRegion(r.c_str())) {
        Serial.printf("[RiftLink] Region: %s (%.1f MHz)\n", region::getCode(), region::getFreq());
        queueDisplayRedraw(displayGetCurrentScreen());
      } else {
        Serial.println("[RiftLink] Region: EU|UK|RU|US|AU");
      }
    } else if (cmd.startsWith("channel ")) {
      if (region::getChannelCount() > 0) {
        int ch = cmd.substring(8).toInt();
        if (ch >= 0 && ch <= 2 && region::setChannel(ch)) {
          Serial.printf("[RiftLink] Channel: %d (%.1f MHz)\n", ch, region::getFreq());
          queueDisplayRedraw(displayGetCurrentScreen());
        } else {
          Serial.println("[RiftLink] channel 0|1|2 (EU/UK only)");
        }
      } else {
        Serial.println("[RiftLink] channel 0|1|2 (EU/UK only)");
      }
    } else if (cmd.startsWith("espnow ")) {
      String sub = cmd.substring(7);
      sub.trim();
      if (sub.startsWith("channel ")) {
        int ch = sub.substring(8).toInt();
        if (ch >= 1 && ch <= 13 && esp_now_slots::setChannel((uint8_t)ch)) {
          esp_now_slots::setAdaptive(false);  // ручная установка → фиксированный режим
          Serial.printf("[RiftLink] ESP-NOW channel: %d (fixed)\n", ch);
        } else {
          Serial.println("[RiftLink] espnow channel 1-13");
        }
      } else if (sub == "adaptive" || sub == "adaptive on") {
        if (esp_now_slots::setAdaptive(true)) {
          Serial.println("[RiftLink] ESP-NOW adaptive on");
        }
      } else if (sub == "adaptive off") {
        if (esp_now_slots::setAdaptive(false)) {
          Serial.println("[RiftLink] ESP-NOW adaptive off (fixed)");
        }
      } else {
        Serial.printf("[RiftLink] ESP-NOW channel: %u %s\n",
            (unsigned)esp_now_slots::getChannel(),
            esp_now_slots::isAdaptive() ? "(adaptive)" : "(fixed)");
      }
    } else if (cmd.startsWith("nickname ")) {
      String nick = cmd.substring(9);
      nick.trim();
      if (nick.length() <= 16 && node::setNickname(nick.c_str())) {
        Serial.printf("[RiftLink] Nickname: %s\n", nick.c_str());
      } else {
        Serial.println("[RiftLink] nickname <name> (max 16 chars)");
      }
    } else if (cmd.startsWith("route ")) {
      String hex16 = cmd.substring(6);
      hex16.trim();
      uint8_t target[protocol::NODE_ID_LEN];
      if (parseNodeIdHex16(hex16, target)) {
        routing::requestRoute(target);
      } else {
        Serial.println("[RiftLink] route <hex16>");
      }
    } else if (cmd.startsWith("lang ")) {
      String l = cmd.substring(5);
      l.trim();
      l.toLowerCase();
      int lang = (l == "ru") ? LANG_RU : LANG_EN;
      if (l == "en" || l == "ru") {
        locale::setLang(lang);
        Serial.printf("[RiftLink] Language: %s\n", l.c_str());
        queueDisplayRedraw(displayGetCurrentScreen());
      } else {
        Serial.println("[RiftLink] lang en|ru");
      }
    } else if (cmd == "gps") {
      int rx, tx, en;
      gps::getPins(&rx, &tx, &en);
      Serial.printf("[RiftLink] GPS: %s %s rx=%d tx=%d en=%d\n",
          gps::isPresent() ? "present" : "absent",
          gps::isEnabled() ? "on" : "off", rx, tx, en);
      if (gps::hasFix()) {
        Serial.printf("[RiftLink] Fix: %.5f, %.5f\n", gps::getLat(), gps::getLon());
      }
    } else if (cmd == "gps on") {
      gps::setEnabled(true);
      Serial.println("[RiftLink] GPS on");
      queueDisplayRedraw(displayGetCurrentScreen());
    } else if (cmd == "gps off") {
      gps::setEnabled(false);
      Serial.println("[RiftLink] GPS off");
      queueDisplayRedraw(displayGetCurrentScreen());
    } else if (cmd.startsWith("gps pins ")) {
      int rx = -1, tx = -1, en = -1;
      if (sscanf(cmd.c_str() + 9, "%d %d %d", &rx, &tx, &en) >= 2) {
        gps::setPins(rx, tx, en);
        gps::saveConfig();
        Serial.printf("[RiftLink] GPS pins rx=%d tx=%d en=%d\n", rx, tx, en);
      } else {
        Serial.println("[RiftLink] gps pins <rx> <tx> [en]");
      }
    } else if (cmd == "powersave") {
      Serial.printf("[RiftLink] Power save: %s (enabled=%s, BLE %s)\n",
          powersave::canSleep() ? "active" : "inactive",
          powersave::isEnabled() ? "yes" : "no",
          ble::isConnected() ? "connected" : "disconnected");
    } else if (cmd == "powersave on") {
      powersave::setEnabled(true);
      Serial.println("[RiftLink] Power save enabled");
    } else if (cmd == "powersave off") {
      powersave::setEnabled(false);
      Serial.println("[RiftLink] Power save disabled");
    } else if (cmd == "selftest" || cmd == "test") {
      selftest::Result r;
      selftest::run(&r);
      queueDisplayRedraw(displayGetCurrentScreen());
    } else if (cmd == "sf" || cmd == "radio") {
      Serial.printf("[RiftLink] SF=%u, %.1f MHz, neighbors=%d\n",
          (unsigned)radio::getSpreadingFactor(), region::getFreq(), neighbors::getCount());
    }
  }

  pollButtonAndQueue();  // drain в radioSchedulerTask — loop не блокируется

  // Fallback: loop помогает drain packetQueue только если packetTask не создан.
  // Иначе получаем два параллельных исполнителя handlePacket() и гонки по shared-буферам.
  if (!asyncHasPacketTask() && packetQueue && uxQueueMessagesWaiting(packetQueue) > 8) {
    constexpr int kLoopDrainMax = 2;
    for (int i = 0; i < kLoopDrainMax; i++) {
      PacketQueueItem* pitem = nullptr;
      if (xQueueReceive(packetQueue, &pitem, 0) != pdTRUE || !pitem) break;
      handlePacket(pitem->buf, pitem->len, (int)pitem->rssi, pitem->sf);
      packetPool.free(pitem);
      pollButtonAndQueue();
      vTaskDelay(1);
    }
  }

  // Retry KEY_EXCHANGE: каждые 45–60с — меньше flood, MSG успевают проходить
  #define KEY_RETRY_BASE_MS  45000
  #define KEY_RETRY_JITTER_MS 15000
  #define KEY_RETRY_EXTRA_JITTER_MS 3000
  static uint32_t s_keyRetryCooldownUntil = 0;  // без блокировки loop
  if (millis() - s_lastDiagSnapshotMs >= 60000) {
    s_lastDiagSnapshotMs = millis();
    RIFTLINK_DIAG("STATE", "event=MODEM_SNAPSHOT region=%s freq_mhz=%.1f sf=%u bw=%.1f cr=%u neighbors=%d ps_enabled=%u ps_can=%u",
        region::getCode(), region::getFreq(), (unsigned)radio::getSpreadingFactor(),
        radio::getBandwidth(), (unsigned)radio::getCodingRate(), neighbors::getCount(),
        (unsigned)powersave::isEnabled(), (unsigned)powersave::canSleep());
  }
  if (millis() >= s_lastKeyRetry && millis() >= s_keyRetryCooldownUntil) {
    uint32_t phase = (node::getId()[0] % 16) * 500;  // 0–8 с смещение по ID
    uint32_t extraJitter = esp_random() % KEY_RETRY_EXTRA_JITTER_MS;
    s_lastKeyRetry = millis() + KEY_RETRY_BASE_MS + (esp_random() % KEY_RETRY_JITTER_MS) + phase + extraJitter;
    int n = neighbors::getCount();
    RIFTLINK_DIAG("KEY", "event=KEY_RETRY_TICK neighbors=%d next_retry_in_ms=%lu cooldown_until=%lu",
        n, (unsigned long)(s_lastKeyRetry - millis()), (unsigned long)s_keyRetryCooldownUntil);
    for (int i = 0; i < n; i++) {
      uint8_t peerId[protocol::NODE_ID_LEN];
      if (neighbors::getId(i, peerId) && !x25519_keys::hasKeyFor(peerId)) {
        RIFTLINK_DIAG("KEY", "event=KEY_RETRY_TARGET peer=%02X%02X idx=%d",
            peerId[0], peerId[1], i);
        x25519_keys::sendKeyExchange(peerId, false, false, "retry");
        s_keyRetryCooldownUntil = millis() + 800 + (esp_random() % 400);  // 0.8–1.2 с, без delay()
        break;
      }
    }
  }

  // RX и powersave RX — только в radioSchedulerTask (loop без radio::takeMutex).

  // Shutdown requested by BLE command
  if (powersave::isShutdownRequested()) {
    powersave::deepSleep();
  }

  // Auto-shutdown on low battery
  if (millis() - s_lastBatCheckMs >= LOW_BAT_CHECK_MS) {
    s_lastBatCheckMs = millis();
    uint16_t mv = telemetry::readBatteryMv();
    if (mv > 0 && mv < LOW_BAT_MV && !telemetry::isCharging()) {
      s_lowBatCount++;
      Serial.printf("[bat] LOW %umV  count=%u/%u\n", mv, s_lowBatCount, LOW_BAT_SHUTOFF_CNT);
      if (s_lowBatCount >= LOW_BAT_SHUTOFF_CNT) {
        Serial.printf("[bat] SHUTDOWN — battery critically low\n");
        displayShowWarning(locale::getForDisplay("low_battery"), locale::getForDisplay("shutting_down"), 3000);
        powersave::deepSleep();
      }
    } else {
      if (s_lowBatCount > 0) Serial.printf("[bat] OK %umV  reset count\n", mv);
      s_lowBatCount = 0;
    }
  }

  ble::update();  // processes pending flags; routes to BLE or WS based on mode
  msg_queue::update();
  routing::update();
  offline_queue::update();
  pollButtonAndQueue();
  if (!displayQueue) displayUpdate();
  if (!powersave::canSleep()) {
    s_loopCooldownUntil = millis() + 10;
  }
}
