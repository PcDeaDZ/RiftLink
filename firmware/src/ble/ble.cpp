/**
 * RiftLink BLE — GATT сервис для Flutter
 * Протокол: JSON {"cmd":"send","text":"..."} / {"evt":"msg","from":"...","text":"..."}
 */

#include "ble.h"
#include <string.h>
#include <string_view>
#include <new>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include "protocol/packet.h"
#include "node/node.h"
#include "wifi/wifi.h"
#include "locale/locale.h"
#include "radio/radio.h"
#include "region/region.h"
#include "neighbors/neighbors.h"
#include "routing/routing.h"
#include "powersave/powersave.h"
#include "groups/groups.h"
#include "x25519_keys/x25519_keys.h"
#include "msg_queue/msg_queue.h"
#include "offline_queue/offline_queue.h"
#include "frag/frag.h"
#include "voice_frag/voice_frag.h"
#include "gps/gps.h"
#include "telemetry/telemetry.h"
#include "selftest/selftest.h"
#include "ui/display.h"
#include "async_tasks.h"
#include "version.h"
#include "log.h"
#include "bls_n/bls_n.h"
#include "crypto/crypto.h"
#include "esp_now_slots/esp_now_slots.h"
#include "radio_mode/radio_mode.h"
#include "ws_server/ws_server.h"
#include "ble_ota/ble_ota.h"
#include <mbedtls/base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <nvs.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <sodium.h>

#define NVS_BLE_NAMESPACE "riftlink"
#define NVS_KEY_BLE_PIN   "ble_pin"
#define NVS_KEY_GROUP_OWNER_SIGN_PK "gpk1"
#define NVS_KEY_GROUP_OWNER_SIGN_SK "gsk1"

static uint32_t s_passkey = 0;
static uint8_t s_groupOwnerSignPk[crypto_sign_PUBLICKEYBYTES] = {0};
static uint8_t s_groupOwnerSignSk[crypto_sign_SECRETKEYBYTES] = {0};
static bool s_groupOwnerSignReady = false;

static void loadOrGeneratePasskey() {
  nvs_handle_t h;
  if (nvs_open(NVS_BLE_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    uint32_t pin = 0;
    if (nvs_get_u32(h, NVS_KEY_BLE_PIN, &pin) == ESP_OK && pin >= 100000 && pin <= 999999) {
      s_passkey = pin;
    } else {
      s_passkey = esp_random() % 900000 + 100000;
      nvs_set_u32(h, NVS_KEY_BLE_PIN, s_passkey);
      nvs_commit(h);
    }
    nvs_close(h);
  } else {
    s_passkey = esp_random() % 900000 + 100000;
  }
}

static bool loadOrGenerateGroupOwnerSigningKey() {
  if (s_groupOwnerSignReady) return true;
  if (sodium_init() < 0) return false;
  nvs_handle_t h;
  if (nvs_open(NVS_BLE_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  size_t pkLen = sizeof(s_groupOwnerSignPk);
  size_t skLen = sizeof(s_groupOwnerSignSk);
  const bool hasPk = (nvs_get_blob(h, NVS_KEY_GROUP_OWNER_SIGN_PK, s_groupOwnerSignPk, &pkLen) == ESP_OK && pkLen == sizeof(s_groupOwnerSignPk));
  const bool hasSk = (nvs_get_blob(h, NVS_KEY_GROUP_OWNER_SIGN_SK, s_groupOwnerSignSk, &skLen) == ESP_OK && skLen == sizeof(s_groupOwnerSignSk));
  if (!hasPk || !hasSk) {
    if (crypto_sign_keypair(s_groupOwnerSignPk, s_groupOwnerSignSk) != 0) {
      nvs_close(h);
      return false;
    }
    nvs_set_blob(h, NVS_KEY_GROUP_OWNER_SIGN_PK, s_groupOwnerSignPk, sizeof(s_groupOwnerSignPk));
    nvs_set_blob(h, NVS_KEY_GROUP_OWNER_SIGN_SK, s_groupOwnerSignSk, sizeof(s_groupOwnerSignSk));
    nvs_commit(h);
  }
  nvs_close(h);
  s_groupOwnerSignReady = true;
  return true;
}

#define SERVICE_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHAR_TX_UUID        "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // write from app
#define CHAR_RX_UUID        "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // notify to app
#define DEVICE_NAME         "RiftLink"
/**
 * Один JSON на запись в GATT TX / одно notify: ≤ BLE_ATT_MAX_JSON_BYTES.
 * После JSON в notify добавляется `\n` (NDJSON) — приложение режет поток по строкам; при len==512 байт `\n` не добавляется (редкий край).
 * NimBLE: BLE_ATT_ATTR_MAX_LEN и CONFIG_NIMBLE_CPP_ATT_VALUE_INIT_LENGTH — макс. 512 байт на значение атрибута.
 * Поэтому «512» — не произвольный порог, а потолок стека BLE.
 *
 * Оценка voice: {"cmd":"voice","to":"HH...","chunk":n,"total":t,"data":"<b64>"} — оболочка ~85–95 B,
 * на base64 остаётся ~417 B → сырой Opus ≤ ~312 B на чанк (см. BLE_VOICE_CHUNK_RAW_MAX).
 *
 * ArduinoJson 7: JsonDocument с кастомным Allocator — предпочтительно SPIRAM, иначе malloc (платы без PSRAM).
 * При переполнении — doc.overflowed().
 */
static constexpr size_t BLE_ATT_MAX_JSON_BYTES = 512;
/** Сырой Opus на один BLE-чанк voice (вход/выход), чтобы весь JSON уместился в 512 B. */
static constexpr size_t BLE_VOICE_CHUNK_RAW_MAX = 300;
static constexpr size_t BLE_VOICE_CHUNK_B64_BUF = 400;  // ceil(300/3)*4
static constexpr size_t kBleJsonProtocolMaxBytes = BLE_ATT_MAX_JSON_BYTES;
static constexpr size_t BLE_PENDING_MSG_TEXT_MAX = frag::MAX_MSG_PLAIN;
static_assert(kBleJsonProtocolMaxBytes == 512, "согласовано с BLE_ATT_ATTR_MAX_LEN");

namespace {
struct BleJsonAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
      void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
      if (p) return p;
    }
    return malloc(size);
  }
  void deallocate(void* pointer) override { free(pointer); }
  void* reallocate(void* pointer, size_t new_size) override {
    if (!pointer) return allocate(new_size);
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
      void* p = heap_caps_realloc(pointer, new_size, MALLOC_CAP_SPIRAM);
      if (p) return p;
    }
    return realloc(pointer, new_size);
  }
};
static BleJsonAllocator s_bleJsonAllocator;
}  // namespace

class RiftTxCharacteristic;

static NimBLEServer* pServer = nullptr;
static NimBLECharacteristic* pRxChar = nullptr;
static RiftTxCharacteristic* pTxChar = nullptr;
static bool s_connected = false;
static bool s_bleInited = false;
static bool s_bleDeinitInProgress = false;
static uint32_t s_advRetryNotBeforeMs = 0;
static constexpr uint32_t BLE_ADV_RESTART_BACKOFF_MS = 2000;
// Отложенные ответы — тяжёлые notify вызываем из main loop, не из callback (Stack canary)
static volatile bool s_pendingInfo = false;
static uint32_t s_pendingInfoNotBeforeMs = 0;
static uint32_t s_pendingInfoCmdId = 0;
static volatile bool s_pendingGroups = false;
static uint32_t s_pendingGroupsCmdId = 0;
static volatile bool s_pendingRoutes = false;
static uint32_t s_pendingRoutesCmdId = 0;
static volatile bool s_pendingNeighbors = false;
static uint32_t s_pendingNeighborsCmdId = 0;
static volatile bool s_pendingInvite = false;
static uint32_t s_pendingInviteCmdId = 0;
static volatile bool s_pendingSelftest = false;
static uint32_t s_pendingSelftestCmdId = 0;
static volatile bool s_pendingGroupSend = false;
static uint32_t s_pendingGroupId = 0;
static char s_pendingGroupText[256] = {0};
static uint16_t s_diagPktIdCounter = 0;  // pktId for BLE diagnostic traffic (ping/signalTest)
static volatile bool s_pendingMsg = false;
static uint8_t s_pendingMsgFrom[8] = {0};
static char s_pendingMsgText[BLE_PENDING_MSG_TEXT_MAX + 1] = {0};
static uint32_t s_pendingMsgId = 0;
static int s_pendingMsgRssi = 0;
static uint8_t s_pendingMsgTtl = 0;
static char s_pendingMsgLane[10] = "normal";
static char s_pendingMsgType[10] = "text";
static uint32_t s_pendingMsgGroupId = 0;
static char s_pendingMsgGroupUid[groups::GROUP_UID_MAX_LEN + 1] = {0};
static volatile bool s_pendingOversizeCmdError = false;
static uint16_t s_pendingOversizeCmdLen = 0;
static volatile bool s_pendingNickname = false;
static char s_pendingNicknameBuf[33] = {0};
static volatile bool s_pendingGps = false;
static uint32_t s_pendingGpsCmdId = 0;
static bool s_pendingGpsHasEnabled = false;
static bool s_pendingGpsEnabled = false;
static bool s_pendingGpsHasPins = false;
static int s_pendingGpsRx = -1, s_pendingGpsTx = -1, s_pendingGpsEn = -1;
static void (*s_onSend)(const uint8_t* to, const char* text, uint8_t ttlMinutes,
    bool critical, uint8_t triggerType, uint32_t triggerValueMs, bool isSos) = nullptr;
static void (*s_onLocation)(float lat, float lon, int16_t alt, uint16_t radiusM, uint32_t expiryEpochSec) = nullptr;
static uint32_t s_inviteExpiryMs = 0;
static uint8_t s_inviteToken[8] = {0};
static bool s_inviteTokenValid = false;

static int parseModemPresetValue(const JsonVariantConst& presetVar, const JsonVariantConst& valueVar) {
  auto parseTextPreset = [](const char* s) -> int {
    if (!s || !s[0]) return -1;
    if (strcmp(s, "speed") == 0 || strcmp(s, "spaid") == 0) return 0;
    if (strcmp(s, "normal") == 0) return 1;
    if (strcmp(s, "range") == 0) return 2;
    if (strcmp(s, "maxrange") == 0 || strcmp(s, "max_range") == 0) return 3;
    return -1;
  };

  int p = -1;
  if (presetVar.is<int>()) p = presetVar.as<int>();
  else if (valueVar.is<int>()) p = valueVar.as<int>();
  if (p >= 0 && p < 4) return p;

  const char* presetText = presetVar.is<const char*>() ? presetVar.as<const char*>() : nullptr;
  p = parseTextPreset(presetText);
  if (p >= 0) return p;
  const char* valueText = valueVar.is<const char*>() ? valueVar.as<const char*>() : nullptr;
  return parseTextPreset(valueText);
}

static bool parseFullNodeIdHex(const char* hexId, uint8_t out[protocol::NODE_ID_LEN]) {
  if (!hexId || !out) return false;
  if (strlen(hexId) != protocol::NODE_ID_LEN * 2) return false;
  for (int i = 0; i < protocol::NODE_ID_LEN; i++) {
    char hi = hexId[i * 2];
    char lo = hexId[i * 2 + 1];
    bool hiOk = (hi >= '0' && hi <= '9') || (hi >= 'a' && hi <= 'f') || (hi >= 'A' && hi <= 'F');
    bool loOk = (lo >= '0' && lo <= '9') || (lo >= 'a' && lo <= 'f') || (lo >= 'A' && lo <= 'F');
    if (!hiOk || !loOk) return false;
    char hex[3] = { hi, lo, 0 };
    out[i] = (uint8_t)strtoul(hex, nullptr, 16);
  }
  return true;
}

static void nodeIdToHex(const uint8_t in[protocol::NODE_ID_LEN], char out[17]) {
  for (int i = 0; i < protocol::NODE_ID_LEN; i++) snprintf(out + i * 2, 3, "%02X", in[i]);
  out[16] = '\0';
}

static groups::GroupRole parseGroupRole(const char* role) {
  if (!role || !role[0]) return groups::GroupRole::None;
  if (strcmp(role, "owner") == 0) return groups::GroupRole::Owner;
  if (strcmp(role, "admin") == 0) return groups::GroupRole::Admin;
  if (strcmp(role, "member") == 0) return groups::GroupRole::Member;
  return groups::GroupRole::None;
}

static const char* groupRoleToStr(groups::GroupRole role) {
  switch (role) {
    case groups::GroupRole::Owner: return "owner";
    case groups::GroupRole::Admin: return "admin";
    case groups::GroupRole::Member: return "member";
    default: return "none";
  }
}

static bool isSelfNodeHex(const char* hexId) {
  uint8_t id[protocol::NODE_ID_LEN];
  if (!parseFullNodeIdHex(hexId, id)) return false;
  return memcmp(id, node::getId(), protocol::NODE_ID_LEN) == 0;
}

struct PendingPingCmdId {
  bool used = false;
  uint8_t to[protocol::NODE_ID_LEN] = {0};
  uint32_t cmdId = 0;
  uint32_t expiresAtMs = 0;
};
static PendingPingCmdId s_pendingPingCmdIds[8];

static inline uint32_t parseCmdIdFromDoc(const JsonDocument& doc) {
  if (!doc["cmdId"].is<uint32_t>() && !doc["cmdId"].is<int>() &&
      !doc["cmdId"].is<int64_t>() && !doc["cmdId"].is<uint64_t>()) {
    return 0;
  }
  uint64_t raw = doc["cmdId"] | 0;
  if (raw == 0 || raw > 0xFFFFFFFFULL) return 0;
  return (uint32_t)raw;
}

static uint32_t s_activeCmdId = 0;

class ActiveCmdScope {
 public:
  explicit ActiveCmdScope(uint32_t cmdId) { s_activeCmdId = cmdId; }
  ~ActiveCmdScope() { s_activeCmdId = 0; }
};

static inline void scheduleInfoNotify(uint32_t delayMs = 0, uint32_t cmdId = 0) {
  s_pendingInfo = true;
  if (cmdId != 0) s_pendingInfoCmdId = cmdId;
  s_pendingInfoNotBeforeMs = millis() + delayMs;
}

static void rememberPingCmdId(const uint8_t to[protocol::NODE_ID_LEN], uint32_t cmdId) {
  if (!to || cmdId == 0 || memcmp(to, protocol::BROADCAST_ID, protocol::NODE_ID_LEN) == 0) return;
  const uint32_t now = millis();
  int freeIdx = -1;
  int replaceIdx = 0;
  uint32_t oldest = 0xFFFFFFFFUL;
  for (int i = 0; i < (int)(sizeof(s_pendingPingCmdIds) / sizeof(s_pendingPingCmdIds[0])); i++) {
    auto& e = s_pendingPingCmdIds[i];
    if (e.used && (int32_t)(now - e.expiresAtMs) >= 0) e.used = false;
    if (e.used && memcmp(e.to, to, protocol::NODE_ID_LEN) == 0) {
      e.cmdId = cmdId;
      e.expiresAtMs = now + 30000UL;
      return;
    }
    if (!e.used && freeIdx < 0) freeIdx = i;
    if (e.expiresAtMs < oldest) {
      oldest = e.expiresAtMs;
      replaceIdx = i;
    }
  }
  const int idx = (freeIdx >= 0) ? freeIdx : replaceIdx;
  auto& dst = s_pendingPingCmdIds[idx];
  dst.used = true;
  memcpy(dst.to, to, protocol::NODE_ID_LEN);
  dst.cmdId = cmdId;
  dst.expiresAtMs = now + 30000UL;
}

static uint32_t takePingCmdIdForFrom(const uint8_t from[protocol::NODE_ID_LEN]) {
  if (!from) return 0;
  const uint32_t now = millis();
  for (int i = 0; i < (int)(sizeof(s_pendingPingCmdIds) / sizeof(s_pendingPingCmdIds[0])); i++) {
    auto& e = s_pendingPingCmdIds[i];
    if (!e.used) continue;
    if ((int32_t)(now - e.expiresAtMs) >= 0) {
      e.used = false;
      continue;
    }
    if (memcmp(e.to, from, protocol::NODE_ID_LEN) == 0) {
      const uint32_t cmdId = e.cmdId;
      e.used = false;
      return cmdId;
    }
  }
  return 0;
}

#define VOICE_BUF_MAX (voice_frag::MAX_VOICE_PLAIN + 1024)
static uint8_t s_voiceBuf[VOICE_BUF_MAX];
static size_t s_voiceBufLen = 0;
static int s_voiceChunkTotal = -1;
static uint8_t s_voiceTo[protocol::NODE_ID_LEN];

static uint32_t s_emitInfoCmdId = 0;
static uint32_t s_emitGroupsCmdId = 0;
static uint32_t s_emitRoutesCmdId = 0;
static uint32_t s_emitNeighborsCmdId = 0;
static uint32_t s_emitInviteCmdId = 0;
static uint32_t s_emitGpsCmdId = 0;

#ifndef BLE_LOG_TX_JSON
#define BLE_LOG_TX_JSON 1
#endif

#if !defined(RIFTLINK_DISABLE_BLS_N)
// BLS-N: BLE scan для приёма RTS при подключённом телефоне
static bool s_blsScanActive = false;
static bool s_blsScanEnded = false;
static uint32_t s_blsScanLastStart = 0;
// Policy: keep BLS scan enabled during active GATT session.
static constexpr bool BLS_SCAN_DURING_GATT_SESSION = true;
#define BLS_SCAN_DURATION_SEC 15
#define BLS_SCAN_RESTART_DELAY_MS 500
/** NimBLE scan резервирует internal heap; при ~1KB после Wi‑Fi+async — BLE_INIT: Malloc failed */
#define BLS_SCAN_MIN_FREE_INTERNAL 5120
#define BLS_SCAN_MIN_LARGEST_INTERNAL 4096

static bool blsScanHeapOk() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= BLS_SCAN_MIN_FREE_INTERNAL &&
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= BLS_SCAN_MIN_LARGEST_INTERNAL;
}

/** Manufacturer Specific Data (AD type 0xFF), индекс как у NimBLE getManufacturerData(index) — без std::string. */
static bool bleAdvGetMfgData(const uint8_t* pl, size_t plLen, uint8_t index,
                             const uint8_t** outData, size_t* outLen) {
  constexpr uint8_t kMfgType = 0xFF;
  size_t off = 0;
  uint8_t seen = 0;
  while (plLen - off > 2) {
    const uint8_t L = pl[off];
    const size_t remaining = plLen - off;
    if (L >= remaining) return false;
    const uint8_t type = pl[off + 1];
    if (type == kMfgType) {
      if (seen == index) {
        if (L > 1) {
          *outData = pl + off + 2;
          *outLen = (size_t)L - 1;
          return true;
        }
        return false;
      }
      seen++;
    }
    off += 1 + L;
  }
  return false;
}
#endif /* !RIFTLINK_DISABLE_BLS_N */

static uint8_t s_notifyTxBuf[BLE_ATT_MAX_JSON_BYTES];
static constexpr UBaseType_t kBleCmdQueueDepth = 12;
static constexpr TickType_t kBleCmdEnqueueTimeoutTicks = pdMS_TO_TICKS(5);
static constexpr uint8_t kBleCmdConsumePerTick = 6;
static constexpr TickType_t kNotifyMutexTryTicks = pdMS_TO_TICKS(20);
static constexpr uint8_t kNotifyMutexMaxAttempts = 3;
struct BleCmdQueueItem {
  uint16_t len;
  uint8_t data[BLE_ATT_MAX_JSON_BYTES];
};
static QueueHandle_t s_bleCmdQueue = nullptr;
static SemaphoreHandle_t s_notifyMutex = nullptr;
struct BleDiagCounters {
  uint32_t cmdEnqueued = 0;
  uint32_t cmdDroppedQueueFull = 0;
  uint32_t cmdConsumed = 0;
  uint32_t cmdQueueHighWater = 0;
  uint32_t notifySent = 0;
  uint32_t notifySkipNoTransport = 0;
  uint32_t notifyMutexBusyDrop = 0;
  uint32_t notifyBytes = 0;
  uint32_t notifyChunks = 0;
  uint32_t statusSent = 0;
  uint32_t statusDelivered = 0;
  uint32_t statusRead = 0;
  uint32_t statusUndelivered = 0;
  uint32_t statusBroadcastDelivery = 0;
};
static BleDiagCounters s_bleDiag;
static uint32_t s_bleDiagLastSnapshotMs = 0;

static void bleDiagMaybeSnapshot(const char* reason, uint32_t minIntervalMs = 10000) {
  const uint32_t now = millis();
  if (now - s_bleDiagLastSnapshotMs < minIntervalMs) return;
  s_bleDiagLastSnapshotMs = now;
  const uint32_t waiting = s_bleCmdQueue ? (uint32_t)uxQueueMessagesWaiting(s_bleCmdQueue) : 0;
  Serial.printf("[BLE_CHAIN] stage=fw_diag action=snapshot reason=%s cmd_enq=%u cmd_drop=%u cmd_consume=%u cmdq_wait=%u cmdq_hwm=%u notify_sent=%u notify_skip=%u notify_mutex_drop=%u notify_bytes=%u notify_chunks=%u st_sent=%u st_delivered=%u st_read=%u st_undelivered=%u st_broadcast=%u\n",
      reason ? reason : "-",
      (unsigned)s_bleDiag.cmdEnqueued,
      (unsigned)s_bleDiag.cmdDroppedQueueFull,
      (unsigned)s_bleDiag.cmdConsumed,
      (unsigned)waiting,
      (unsigned)s_bleDiag.cmdQueueHighWater,
      (unsigned)s_bleDiag.notifySent,
      (unsigned)s_bleDiag.notifySkipNoTransport,
      (unsigned)s_bleDiag.notifyMutexBusyDrop,
      (unsigned)s_bleDiag.notifyBytes,
      (unsigned)s_bleDiag.notifyChunks,
      (unsigned)s_bleDiag.statusSent,
      (unsigned)s_bleDiag.statusDelivered,
      (unsigned)s_bleDiag.statusRead,
      (unsigned)s_bleDiag.statusUndelivered,
      (unsigned)s_bleDiag.statusBroadcastDelivery);
}

static const char* transportModeTag() {
  return (radio_mode::current() == radio_mode::WIFI) ? "wifi" : "ble";
}

static void extractEvtForLog(const char* payload, size_t len, char* outEvt, size_t outEvtLen) {
  if (!outEvt || outEvtLen == 0) return;
  outEvt[0] = '\0';
  if (!payload || len == 0) return;
  const char* key = "\"evt\":\"";
  const char* pos = strstr(payload, key);
  if (!pos) return;
  pos += strlen(key);
  size_t i = 0;
  while (i + 1 < outEvtLen && (size_t)(pos - payload) < len && pos[i] && pos[i] != '"') {
    outEvt[i] = pos[i];
    i++;
  }
  outEvt[i] = '\0';
}

static bool hasActiveTransport() {
  if (radio_mode::current() == radio_mode::WIFI)
    return ws_server::hasClient();
  return s_connected && pRxChar;
}

/** Макс. payload одного BLE notify = negotiated MTU - 3 (ATT header). */
static size_t bleNotifyMtu() {
  if (!pServer || pServer->getConnectedCount() == 0) return BLE_ATT_MAX_JSON_BYTES;
  uint16_t mtu = pServer->getPeerMTU(pServer->getPeerInfo(0).getConnHandle());
  return (mtu > 3) ? (mtu - 3) : BLE_ATT_MAX_JSON_BYTES;
}

static void notifyJsonToApp(const char* payload, size_t len) {
  if (!payload || len == 0) return;
  bool locked = false;
  for (uint8_t attempt = 0; attempt < kNotifyMutexMaxAttempts; attempt++) {
    if (s_notifyMutex && xSemaphoreTake(s_notifyMutex, kNotifyMutexTryTicks) == pdTRUE) {
      locked = true;
      break;
    }
    vTaskDelay(1);
  }
  if (!locked) {
    s_bleDiag.notifyMutexBusyDrop++;
    Serial.printf("[BLE_CHAIN] stage=fw_notify_json action=drop reason=notify_mutex_busy len=%u\n", (unsigned)len);
    bleDiagMaybeSnapshot("notify_mutex_drop", 1500);
    return;
  }
  char evtTag[24];
  extractEvtForLog(payload, len, evtTag, sizeof(evtTag));
  const char* evt = evtTag[0] ? evtTag : "unknown";

  // WiFi mode: route to WebSocket (no size limit)
  if (radio_mode::current() == radio_mode::WIFI) {
    Serial.printf("[BLE_CHAIN] stage=fw_notify_json action=send mode=wifi evt=%s len=%u\n",
        evt, (unsigned)len);
#if BLE_LOG_TX_JSON
    Serial.print("[WS->APP] ");
    Serial.write((const uint8_t*)payload, len);
    Serial.println();
#endif
    ws_server::sendEvent(payload, (int)len);
    s_bleDiag.notifySent++;
    s_bleDiag.notifyBytes += (uint32_t)len;
    s_bleDiag.notifyChunks++;
    Serial.printf("[BLE_CHAIN] stage=fw_notify_json action=sent mode=wifi evt=%s len=%u\n",
        evt, (unsigned)len);
    bleDiagMaybeSnapshot("notify_wifi", 8000);
    xSemaphoreGive(s_notifyMutex);
    return;
  }

  if (!pRxChar || !s_connected) {
    s_bleDiag.notifySkipNoTransport++;
    Serial.printf("[BLE_CHAIN] stage=fw_notify_json action=skip reason=no_transport mode=%s evt=%s len=%u\n",
        transportModeTag(), evt, (unsigned)len);
    bleDiagMaybeSnapshot("notify_skip", 1500);
    xSemaphoreGive(s_notifyMutex);
    return;
  }

#if BLE_LOG_TX_JSON
  Serial.print("[BLE->APP] ");
  Serial.write((const uint8_t*)payload, len);
  Serial.println();
#endif

  const size_t mtuChunk = bleNotifyMtu();
  const size_t chunkMax = (mtuChunk < sizeof(s_notifyTxBuf)) ? mtuChunk : sizeof(s_notifyTxBuf);
  if (mtuChunk > sizeof(s_notifyTxBuf)) {
    Serial.printf("[BLE_CHAIN] stage=fw_notify_json action=clamp reason=tx_buf_limit mtuChunk=%u txBuf=%u\n",
        (unsigned)mtuChunk, (unsigned)sizeof(s_notifyTxBuf));
  }
  Serial.printf("[BLE_CHAIN] stage=fw_notify_json action=send mode=ble evt=%s len=%u chunkMax=%u\n",
      evt, (unsigned)len, (unsigned)chunkMax);
  size_t off = 0;
  uint32_t chunks = 0;
  while (off < len) {
    size_t remain = len - off;
    bool lastChunk = (remain <= chunkMax);
    size_t chunk = lastChunk ? remain : chunkMax;

    memcpy(s_notifyTxBuf, payload + off, chunk);
    size_t sendLen = chunk;
    // NDJSON newline delimiter only on the very last chunk
    if (lastChunk && chunk < sizeof(s_notifyTxBuf)) {
      s_notifyTxBuf[chunk] = '\n';
      sendLen = chunk + 1;
    }

    pRxChar->setValue(s_notifyTxBuf, sendLen);
    pRxChar->notify();
    off += chunk;
    chunks++;

    if (!lastChunk) vTaskDelay(pdMS_TO_TICKS(12));
  }
  s_bleDiag.notifySent++;
  s_bleDiag.notifyBytes += (uint32_t)len;
  s_bleDiag.notifyChunks += chunks;
  Serial.printf("[BLE_CHAIN] stage=fw_notify_json action=sent mode=ble evt=%s len=%u chunks=%u\n",
      evt, (unsigned)len, (unsigned)chunks);
  bleDiagMaybeSnapshot("notify_ble", 8000);
  xSemaphoreGive(s_notifyMutex);
}

#if !defined(RIFTLINK_DISABLE_BLS_N)
class BlsScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (!advertisedDevice->haveManufacturerData()) return;
    const auto& pl = advertisedDevice->getPayload();
    const uint8_t* d = nullptr;
    size_t mfrLen = 0;
    constexpr size_t kRtsMfgLen = 2 + 3 + protocol::NODE_ID_LEN + protocol::NODE_ID_LEN + 2 + 4;
    if (!bleAdvGetMfgData(pl.data(), pl.size(), 0, &d, &mfrLen) || mfrLen < kRtsMfgLen) return;
    if (d[0] != 0x4C || d[1] != 0x52) return;  // company ID 0x524C little-endian
    if (d[2] != 0x52 || d[3] != 0x54 || d[4] != 0x53) return;  // "RTS"
    uint8_t fromId[protocol::NODE_ID_LEN], toId[protocol::NODE_ID_LEN];
    memcpy(fromId, d + 5, protocol::NODE_ID_LEN);
    memcpy(toId, d + 5 + protocol::NODE_ID_LEN, protocol::NODE_ID_LEN);
    size_t lenOff = 5 + protocol::NODE_ID_LEN + protocol::NODE_ID_LEN;
    uint16_t len = (uint16_t)d[lenOff + 0] << 8 | d[lenOff + 1];
    uint32_t txAt = (uint32_t)d[lenOff + 2] << 24 | (uint32_t)d[lenOff + 3] << 16 |
        (uint32_t)d[lenOff + 4] << 8 | d[lenOff + 5];
    bls_n::addReceivedRts(fromId, toId, len, txAt);
  }
  void onScanEnd(const NimBLEScanResults& scanResults, int reason) override {
    (void)scanResults;
    (void)reason;
    s_blsScanActive = false;
    s_blsScanEnded = true;
  }
};

static BlsScanCallbacks s_blsScanCallbacks;
#endif /* !RIFTLINK_DISABLE_BLS_N */

/** Большой evt info — раньше String::reserve(1200); теперь BSS, без heap. */
static constexpr size_t NOTIFY_INFO_JSON_CAP = 1280;
static char s_notifyInfoPayload[NOTIFY_INFO_JSON_CAP];
static char s_lastInfoPayload[NOTIFY_INFO_JSON_CAP];
static size_t s_lastInfoPayloadLen = 0;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    s_connected = true;
    s_lastInfoPayloadLen = 0;  // send first info snapshot on every new BLE session
    displayWakeRequest();
    vTaskDelay(pdMS_TO_TICKS(5));   // краткая пауза для GATT
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    s_connected = false;
    if (s_bleDeinitInProgress || !s_bleInited) {
      return;
    }
#if !defined(RIFTLINK_DISABLE_BLS_N)
    s_blsScanEnded = true;
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
      pScan->stop();
      s_blsScanActive = false;
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(50));  // дать стеку освободить соединение
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (!pAdv->isAdvertising() && millis() >= s_advRetryNotBeforeMs) {
      if (pAdv->start()) {
        Serial.println("[BLE] Advertising restarted after disconnect");
      } else {
        s_advRetryNotBeforeMs = millis() + BLE_ADV_RESTART_BACKOFF_MS;
      }
    }
  }
};

static void bleHandleTxJson(const uint8_t* val, uint16_t len);

/**
 * TX GATT: NimBLE вызывает writeEvent с буфером записи — парсим JSON по указателю,
 * без getValue()/std::string и без копии NimBLEAttValue (deepCopy/realloc в библиотеке).
 */
class RiftTxCharacteristic : public NimBLECharacteristic {
 public:
  RiftTxCharacteristic(const NimBLEUUID& uuid, uint32_t properties, uint16_t maxLen, NimBLEService* pService)
      : NimBLECharacteristic(uuid, static_cast<uint16_t>(properties), maxLen, pService) {}

 protected:
  void writeEvent(const uint8_t* val, uint16_t len, NimBLEConnInfo& connInfo) override {
    (void)connInfo;
    if (!val || len == 0) return;
    if (len > BLE_ATT_MAX_JSON_BYTES) {
      s_pendingOversizeCmdLen = len;
      s_pendingOversizeCmdError = true;
      return;
    }
    if (!s_bleCmdQueue) return;
    BleCmdQueueItem item{};
    item.len = len;
    memcpy(item.data, val, len);
    if (xQueueSend(s_bleCmdQueue, &item, kBleCmdEnqueueTimeoutTicks) != pdTRUE) {
      s_bleDiag.cmdDroppedQueueFull++;
      static uint32_t s_lastCmdDropLogMs = 0;
      uint32_t now = millis();
      if (now - s_lastCmdDropLogMs >= 2000) {
        s_lastCmdDropLogMs = now;
        Serial.println("[BLE] cmd queue full, drop");
        bleDiagMaybeSnapshot("cmd_drop", 1500);
      }
    } else {
      s_bleDiag.cmdEnqueued++;
      const uint32_t waiting = (uint32_t)uxQueueMessagesWaiting(s_bleCmdQueue);
      if (waiting > s_bleDiag.cmdQueueHighWater) s_bleDiag.cmdQueueHighWater = waiting;
    }
  }
};

static void notifyGroupStatusV2(const char* groupUid) {
  if (!groupUid || !groupUid[0]) return;
  if (!hasActiveTransport()) return;
  uint32_t channelId32 = 0;
  char groupTag[groups::GROUP_TAG_MAX_LEN + 1] = {0};
  char canonicalName[groups::GROUP_CANONICAL_NAME_MAX_LEN + 1] = {0};
  uint16_t keyVersion = 0;
  groups::GroupRole role = groups::GroupRole::None;
  uint32_t revocationEpoch = 0;
  bool ackApplied = false;
  if (!groups::getGroupV2(groupUid, &channelId32, groupTag, sizeof(groupTag), canonicalName, sizeof(canonicalName), &keyVersion, &role, &revocationEpoch, &ackApplied)) {
    return;
  }
  JsonDocument ev(&s_bleJsonAllocator);
  ev["evt"] = "groupStatus";
  ev["groupUid"] = groupUid;
  ev["channelId32"] = channelId32;
  ev["groupTag"] = groupTag;
  ev["canonicalName"] = canonicalName;
  ev["myRole"] = groupRoleToStr(role);
  ev["keyVersion"] = keyVersion;
  ev["revocationEpoch"] = revocationEpoch;
  ev["ackApplied"] = ackApplied;
  ev["status"] = "ok";
  ev["rekeyRequired"] = !ackApplied;
  if (s_activeCmdId != 0) ev["cmdId"] = s_activeCmdId;
  char buf[360];
  size_t n = serializeJson(ev, buf);
  notifyJsonToApp(buf, n);
}

static void notifyGroupRekeyProgressV2(const char* groupUid, const char* rekeyOpId, uint16_t keyVersion) {
  if (!groupUid || !groupUid[0]) return;
  if (!hasActiveTransport()) return;
  JsonDocument ev(&s_bleJsonAllocator);
  ev["evt"] = "groupRekeyProgress";
  ev["groupUid"] = groupUid;
  ev["rekeyOpId"] = (rekeyOpId && rekeyOpId[0]) ? rekeyOpId : "local";
  ev["keyVersion"] = keyVersion;
  ev["pending"] = 0;
  ev["delivered"] = 0;
  ev["applied"] = 1;
  ev["failed"] = 0;
  if (s_activeCmdId != 0) ev["cmdId"] = s_activeCmdId;
  char buf[220];
  size_t n = serializeJson(ev, buf);
  notifyJsonToApp(buf, n);
}

static void notifyGroupMemberKeyStateV2(const char* groupUid, const char* memberId, const char* state, uint32_t ackAt) {
  if (!groupUid || !groupUid[0] || !memberId || !memberId[0] || !state || !state[0]) return;
  if (!hasActiveTransport()) return;
  JsonDocument ev(&s_bleJsonAllocator);
  ev["evt"] = "groupMemberKeyState";
  ev["groupUid"] = groupUid;
  ev["memberId"] = memberId;
  ev["status"] = state;
  if (ackAt > 0) ev["ackAt"] = ackAt;
  char buf[220];
  size_t n = serializeJson(ev, buf);
  notifyJsonToApp(buf, n);
}

static void notifyGroupSecurityErrorV2(const char* groupUid, const char* code, const char* msg) {
  if (!hasActiveTransport()) return;
  JsonDocument ev(&s_bleJsonAllocator);
  ev["evt"] = "groupSecurityError";
  if (groupUid && groupUid[0]) ev["groupUid"] = groupUid;
  ev["code"] = code ? code : "group_v2_unknown";
  ev["msg"] = msg ? msg : "";
  if (s_activeCmdId != 0) ev["cmdId"] = s_activeCmdId;
  char buf[280];
  size_t n = serializeJson(ev, buf);
  notifyJsonToApp(buf, n);
}

static void bleHandleTxJson(const uint8_t* val, uint16_t len) {
  if (len == 0) return;

  // BLE OTA: binary chunk mode — data goes directly to ble_ota, not JSON
  if (ble_ota::isActive()) {
    // Check if it's a JSON control message (starts with '{')
    if (val[0] == '{') {
      // Parse as JSON — could be bleOtaEnd or bleOtaAbort
      JsonDocument doc(&s_bleJsonAllocator);
      DeserializationError err = deserializeJson(doc, std::string_view((const char*)val, len));
      if (!err) {
        const char* cmd = doc["cmd"];
        if (cmd && strcmp(cmd, "bleOtaEnd") == 0) {
          bool ok = ble_ota::end();
          char resp[64];
          snprintf(resp, sizeof(resp), "{\"evt\":\"bleOtaResult\",\"ok\":%s}", ok ? "true" : "false");
          notifyJsonToApp(resp, strlen(resp));
          if (ok) {
            delay(500);
            esp_restart();
          }
          return;
        }
        if (cmd && strcmp(cmd, "bleOtaAbort") == 0) {
          ble_ota::abort();
          notifyJsonToApp("{\"evt\":\"bleOtaResult\",\"ok\":false,\"reason\":\"aborted\"}", 52);
          return;
        }
        if (cmd && strcmp(cmd, "bleOtaChunk") == 0) {
          const char* b64 = doc["data"];
          if (!b64 || !b64[0]) {
            ble_ota::abort();
            notifyJsonToApp("{\"evt\":\"bleOtaResult\",\"ok\":false,\"reason\":\"bad_chunk\"}", 53);
            return;
          }
          uint8_t chunk[768];
          size_t decLen = 0;
          if (mbedtls_base64_decode(chunk, sizeof(chunk), &decLen,
                                    (const unsigned char*)b64, strlen(b64)) != 0 || decLen == 0) {
            ble_ota::abort();
            notifyJsonToApp("{\"evt\":\"bleOtaResult\",\"ok\":false,\"reason\":\"bad_base64\"}", 54);
            return;
          }
          if (!ble_ota::writeChunk(chunk, decLen)) {
            ble_ota::abort();
            notifyJsonToApp("{\"evt\":\"bleOtaResult\",\"ok\":false,\"reason\":\"write_failed\"}", 56);
            return;
          }
          uint32_t w = ble_ota::bytesWritten();
          if (w > 0 && (w % 16384) < decLen) {
            char prog[80];
            int n = snprintf(prog, sizeof(prog), "{\"evt\":\"bleOtaProgress\",\"written\":%u}", (unsigned)w);
            notifyJsonToApp(prog, n);
          }
          return;
        }
      }
      // JSON в OTA-режиме принимаем только как управляющие/chunk команды.
      ble_ota::abort();
      notifyJsonToApp("{\"evt\":\"bleOtaResult\",\"ok\":false,\"reason\":\"bad_ota_cmd\"}", 55);
      return;
    }
    // Raw binary chunk
    if (!ble_ota::writeChunk(val, len)) {
      ble_ota::abort();
      notifyJsonToApp("{\"evt\":\"bleOtaResult\",\"ok\":false,\"reason\":\"write_failed\"}", 56);
      return;
    }
    // Progress notification every 16K
    uint32_t w = ble_ota::bytesWritten();
    if (w > 0 && (w % 16384) < len) {
      char prog[80];
      int n = snprintf(prog, sizeof(prog), "{\"evt\":\"bleOtaProgress\",\"written\":%u}", (unsigned)w);
      notifyJsonToApp(prog, n);
    }
    return;
  }

  if (len > BLE_ATT_MAX_JSON_BYTES) {
    ble::notifyError("payload_too_long", "JSON exceeds 512 bytes");
    return;
  }

  JsonDocument doc(&s_bleJsonAllocator);
  const std::string_view jsonSv(reinterpret_cast<const char*>(val), len);
  DeserializationError err = deserializeJson(doc, jsonSv);
  if (err) return;
  if (doc.overflowed()) {
    ble::notifyError("json_overflow", "ArduinoJson document overflow");
    return;
  }

    const char* cmd = doc["cmd"];
    const uint32_t cmdId = parseCmdIdFromDoc(doc);
    if (!cmd) return;
    ActiveCmdScope activeCmdScope(cmdId);

    if (strcmp(cmd, "bleOtaStart") == 0) {
      uint32_t size = doc["size"] | 0;
      const char* md5 = doc["md5"];
      if (size == 0) {
        notifyJsonToApp("{\"evt\":\"bleOtaResult\",\"ok\":false,\"reason\":\"no_size\"}", 51);
        return;
      }
      if (ble_ota::begin(size, md5)) {
        char resp[64];
        int n = snprintf(resp, sizeof(resp), "{\"evt\":\"bleOtaReady\",\"chunkSize\":%u}", (unsigned)(BLE_ATT_MAX_JSON_BYTES - 3));
        notifyJsonToApp(resp, n);
      } else {
        notifyJsonToApp("{\"evt\":\"bleOtaResult\",\"ok\":false,\"reason\":\"begin_failed\"}", 56);
      }
      return;
    }

    if (strcmp(cmd, "info") == 0) {
      scheduleInfoNotify(0, cmdId);
      return;
    }

    if (strcmp(cmd, "invite") == 0) {
      uint32_t ttlSec = doc["ttlSec"] | 600;
      if (ttlSec < 60) ttlSec = 60;
      if (ttlSec > 3600) ttlSec = 3600;
      for (size_t i = 0; i < sizeof(s_inviteToken); i++) {
        s_inviteToken[i] = (uint8_t)(esp_random() & 0xFF);
      }
      s_inviteExpiryMs = millis() + ttlSec * 1000UL;
      s_inviteTokenValid = true;
      s_pendingInvite = true;
      if (cmdId != 0) s_pendingInviteCmdId = cmdId;
      return;
    }

    if (strcmp(cmd, "channelKey") == 0) {
      const char* keyB64 = doc["key"];
      if (keyB64) {
        size_t decLen;
        uint8_t key[32];
        if (mbedtls_base64_decode(key, 32, &decLen, (const unsigned char*)keyB64, strlen(keyB64)) == 0 && decLen == 32) {
          if (crypto::setChannelKey(key)) {
            scheduleInfoNotify(0, cmdId);
          }
        }
      }
      return;
    }

    if (strcmp(cmd, "acceptInvite") == 0) {
      const char* idStr = doc["id"];
      const char* pubKeyB64 = doc["pubKey"];
      const char* channelKeyB64 = doc["channelKey"];
      const char* inviteTokenHex = doc["inviteToken"];
      if (inviteTokenHex && inviteTokenHex[0]) {
        if (strlen(inviteTokenHex) != 16) {
          ble::notifyError("invite_token_bad_length", "Bad inviteToken length");
          return;
        }
        for (int i = 0; i < 16; i++) {
          char c = inviteTokenHex[i];
          bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
          if (!ok) {
            ble::notifyError("invite_token_bad_format", "Bad inviteToken format");
            return;
          }
        }
      }
      if (idStr && pubKeyB64) {
        if (channelKeyB64) {
          size_t decLen;
          uint8_t chKey[32];
          if (mbedtls_base64_decode(chKey, 32, &decLen, (const unsigned char*)channelKeyB64, strlen(channelKeyB64)) == 0 && decLen == 32) {
            crypto::setChannelKey(chKey);
          }
        }
        uint8_t nodeId[protocol::NODE_ID_LEN];
        if (!parseFullNodeIdHex(idStr, nodeId)) {
          ble::notifyError("id_bad", "id must be full 16 hex node id");
          return;
        }
        size_t decLen;
        uint8_t pubKey[32];
        if (mbedtls_base64_decode(pubKey, 32, &decLen, (const unsigned char*)pubKeyB64, strlen(pubKeyB64)) == 0 && decLen == 32) {
          x25519_keys::onKeyExchange(nodeId, pubKey);
          x25519_keys::sendKeyExchange(nodeId, true, false, "ble");  // forceSend — отправить наш ключ пиру для завершения обмена
          // Local one-time consume: если приняли именно наш текущий inviteToken, сразу инвалидируем.
          if (inviteTokenHex && inviteTokenHex[0] && s_inviteTokenValid && (int32_t)(millis() - s_inviteExpiryMs) < 0) {
            char tokHex[17] = {0};
            for (int i = 0; i < 8; i++) snprintf(tokHex + i * 2, 3, "%02X", s_inviteToken[i]);
            if (strncmp(tokHex, inviteTokenHex, 16) == 0) {
              s_inviteTokenValid = false;
              memset(s_inviteToken, 0, sizeof(s_inviteToken));
              s_inviteExpiryMs = 0;
            }
          }
          scheduleInfoNotify(0, cmdId);
        }
      }
      return;
    }

    if (strcmp(cmd, "send") == 0) {
      const char* text = doc["text"];
      if (!text || strlen(text) == 0) return;  // пустые — не отправлять
      const char* lane = doc["lane"] | "normal";
      const char* trigger = doc["trigger"] | "";
      bool critical = (strcmp(lane, "critical") == 0);
      uint8_t triggerType = 0;
      uint32_t triggerValueMs = 0;
      if (strcmp(trigger, "target_online") == 0) {
        triggerType = 1;
      } else if (strcmp(trigger, "deliver_after_time") == 0) {
        triggerType = 2;
        triggerValueMs = (uint32_t)(doc["triggerAtMs"] | 0);
      }
      uint32_t groupId = doc["group"] | 0;
      if (groupId > 0) {
        if (strlen(text) == 0) return;  // пустые в группу — не отправлять
        strncpy(s_pendingGroupText, text, sizeof(s_pendingGroupText) - 1);
        s_pendingGroupText[sizeof(s_pendingGroupText) - 1] = '\0';
        s_pendingGroupId = groupId;
        __sync_synchronize();  // memory barrier — main loop должен видеть данные до флага
        s_pendingGroupSend = true;
      } else {
        uint8_t to[protocol::NODE_ID_LEN];
        memset(to, 0xFF, protocol::NODE_ID_LEN);
        const char* toStr = doc["to"];
        if (toStr && toStr[0]) {
          if (!parseFullNodeIdHex(toStr, to)) {
            ble::notifyError("send_to_bad", "to must be full 16 hex node id");
            return;
          }
        }
        uint8_t ttl = (doc["ttl"] | 0) & 0xFF;
        if (s_onSend) s_onSend(to, text, ttl, critical, triggerType, triggerValueMs, false);
        if (triggerType != 0) {
          JsonDocument qdoc(&s_bleJsonAllocator);
          qdoc["evt"] = "timeCapsuleQueued";
          if (toStr && strlen(toStr) > 0) qdoc["to"] = toStr;
          qdoc["trigger"] = trigger;
          if (triggerValueMs > 0) qdoc["triggerAtMs"] = triggerValueMs;
          char qbuf[180];
          size_t qlen = serializeJson(qdoc, qbuf);
          notifyJsonToApp(qbuf, qlen);
        }
      }
      return;
    }

    if (strcmp(cmd, "sos") == 0) {
      const char* text = doc["text"] | "SOS";
      if (s_onSend) s_onSend(protocol::BROADCAST_ID, text, 0, true, 0, 0, true);
      return;
    }

    if (strcmp(cmd, "location") == 0) {
      float lat = doc["lat"] | 0.0f;
      float lon = doc["lon"] | 0.0f;
      int16_t alt = doc["alt"] | 0;
      uint16_t radiusM = (uint16_t)(doc["radiusM"] | 0);
      uint32_t expiryEpochSec = (uint32_t)(doc["expiryEpochSec"] | 0);
      if (s_onLocation) s_onLocation(lat, lon, alt, radiusM, expiryEpochSec);
      return;
    }

    if (strcmp(cmd, "radioMode") == 0) {
      const char* mode = doc["mode"];
      if (!mode) return;
      if (strcmp(mode, "wifi") == 0) {
        const char* variant = doc["variant"] | "sta";
        const char* ssid = doc["ssid"];
        const char* pass = doc["pass"].as<const char*>();
        if (!pass) pass = doc["password"].as<const char*>();
        if (strcmp(variant, "sta") != 0) {
          ble::notifyError("radioMode", "Only STA variant is supported");
          return;
        }
        if (!ssid || !ssid[0]) {
          ble::notifyError("radioMode", "STA requires SSID");
          return;
        }
        radio_mode::switchTo(radio_mode::WIFI, radio_mode::STA, ssid, pass);
      } else if (strcmp(mode, "ble") == 0) {
        radio_mode::switchTo(radio_mode::BLE);
      }
      return;
    }

    if (strcmp(cmd, "lang") == 0) {
      const char* lang = doc["lang"];
      if (lang) {
        if (strcmp(lang, "ru") == 0) locale::setLang(LANG_RU);
        else if (strcmp(lang, "en") == 0) locale::setLang(LANG_EN);
      }
      return;
    }

    if (strcmp(cmd, "wifi") == 0) {
      const char* ssid = doc["ssid"];
      const char* pass = doc["pass"].as<const char*>();
      if (!pass) pass = doc["password"].as<const char*>();
      if (ssid && ssid[0]) {
        radio_mode::switchTo(radio_mode::WIFI, radio_mode::STA, ssid, pass);
      } else {
        ble::notifyError("wifi", "STA requires SSID");
      }
      return;
    }

    if (strcmp(cmd, "region") == 0) {
      const char* r = doc["region"];
      if (r && region::setRegion(r)) {
        ble::notifyRegion(region::getCode(), region::getFreq(), region::getPower(), region::getChannel(), cmdId);
      }
      return;
    }

    if (strcmp(cmd, "channel") == 0) {
      int ch = doc["channel"] | -1;
      if (ch >= 0 && ch <= 2 && region::setChannel(ch)) {
        ble::notifyRegion(region::getCode(), region::getFreq(), region::getPower(), region::getChannel(), cmdId);
      }
      return;
    }
    if (strcmp(cmd, "sf") == 0 || strcmp(cmd, "loraSf") == 0) {
      int sf = doc["sf"] | doc["value"] | -1;
      if (sf >= 7 && sf <= 12) {
        if (!radio::requestSpreadingFactor((uint8_t)sf)) {
          ble::notifyError("sf", "Queue busy, retry");
        } else {
          scheduleInfoNotify(450, cmdId);
        }
      }
      return;
    }
    if (strcmp(cmd, "modemPreset") == 0) {
      const JsonVariantConst presetVar = doc["preset"];
      const JsonVariantConst valueVar = doc["value"];
      int p = parseModemPresetValue(presetVar, valueVar);
      Serial.printf("[BLE_CHAIN] stage=fw_cmd action=modem_preset_req preset=%d current=%d\n",
          p, (int)radio::getModemPreset());
      if (p >= 0 && p < 4) {
        if (!radio::requestModemPreset((radio::ModemPreset)p)) {
          Serial.printf("[BLE_CHAIN] stage=fw_cmd action=modem_preset_queue_busy preset=%d\n", p);
          ble::notifyError("modemPreset", "Queue busy, retry");
        } else {
          Serial.printf("[BLE_CHAIN] stage=fw_cmd action=modem_preset_queued preset=%d\n", p);
          scheduleInfoNotify(450, cmdId);
        }
      } else {
        Serial.println("[BLE_CHAIN] stage=fw_cmd action=modem_preset_invalid");
        ble::notifyError("modemPreset", "Invalid preset (0..3 or speed/normal/range/maxrange)");
      }
      return;
    }
    if (strcmp(cmd, "modemCustom") == 0) {
      int sf = doc["sf"] | -1;
      float bw = doc["bw"] | -1.0f;
      int cr = doc["cr"] | -1;
      if (sf >= 7 && sf <= 12 && bw > 0 && cr >= 5 && cr <= 8) {
        if (!radio::requestCustomModem((uint8_t)sf, bw, (uint8_t)cr)) {
          ble::notifyError("modemCustom", "Queue busy, retry");
        } else {
          scheduleInfoNotify(450, cmdId);
        }
      }
      return;
    }
    if (strcmp(cmd, "espnowChannel") == 0) {
      if (radio_mode::current() != radio_mode::WIFI) {
        ble::notifyError("espnow", "ESP-NOW доступен только в WiFi-режиме");
        return;
      }
      int ch = doc["channel"] | doc["espnowChannel"] | -1;
      if (ch >= 1 && ch <= 13 && esp_now_slots::setChannel((uint8_t)ch)) {
        esp_now_slots::setAdaptive(false);
        scheduleInfoNotify(0, cmdId);
      }
      return;
    }
    if (strcmp(cmd, "espnowAdaptive") == 0) {
      if (radio_mode::current() != radio_mode::WIFI) {
        ble::notifyError("espnow", "ESP-NOW доступен только в WiFi-режиме");
        return;
      }
      bool on = doc["enabled"] | doc["adaptive"] | false;
      if (esp_now_slots::setAdaptive(on)) scheduleInfoNotify(0, cmdId);
      return;
    }

    if (strcmp(cmd, "nickname") == 0) {
      const char* nick = doc["nickname"];
      if (nick && strnlen(nick, 34) <= 32) {
        strncpy(s_pendingNicknameBuf, nick, 32);
        s_pendingNicknameBuf[32] = '\0';
        __sync_synchronize();
        s_pendingNickname = true;
        scheduleInfoNotify(0, cmdId);
        queueDisplayRequestInfoRedraw();
      }
      return;
    }

    if (strcmp(cmd, "groups") == 0) {
      s_pendingGroups = true;
      if (cmdId != 0) s_pendingGroupsCmdId = cmdId;
      return;
    }
    if (strcmp(cmd, "groupCreate") == 0) {
      const char* groupUid = doc["groupUid"];
      const char* groupTag = doc["groupTag"];
      const char* canonicalName = doc["displayName"];
      uint32_t channelId32 = doc["channelId32"] | 0;
      const char* keyB64 = doc["groupKey"];
      uint16_t keyVersion = (uint16_t)(doc["keyVersion"] | 1);
      const char* roleStr = doc["myRole"];
      uint32_t revEpoch = doc["revocationEpoch"] | 0;
      if (!groupUid || !groupUid[0] || !groupTag || !groupTag[0] || !canonicalName || !canonicalName[0] || channelId32 <= groups::GROUP_ALL) {
        notifyGroupSecurityErrorV2(groupUid, "group_v3_bad", "Missing groupUid/groupTag/canonicalName/channelId32");
        return;
      }
      if (strchr(canonicalName, '|') != nullptr) {
        notifyGroupSecurityErrorV2(groupUid, "group_v3_bad", "canonicalName contains invalid separator");
        return;
      }
      uint8_t key[32];
      if (keyB64 && keyB64[0]) {
        size_t decLen = 0;
        if (mbedtls_base64_decode(key, sizeof(key), &decLen, (const unsigned char*)keyB64, strlen(keyB64)) != 0 || decLen != 32) {
          notifyGroupSecurityErrorV2(groupUid, "group_v2_key_bad", "Bad groupKey");
          return;
        }
      } else {
        for (size_t i = 0; i < sizeof(key); i += 4) {
          uint32_t r = esp_random();
          memcpy(&key[i], &r, (sizeof(key) - i >= 4) ? 4 : (sizeof(key) - i));
        }
      }
      groups::GroupRole role = parseGroupRole(roleStr);
      if (role == groups::GroupRole::None) role = groups::GroupRole::Owner;
      if (!groups::upsertGroupV2(groupUid, channelId32, groupTag, canonicalName, key, keyVersion, role, revEpoch)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v3_store_failed", "Failed to store V3 group");
        return;
      }
      if (role == groups::GroupRole::Owner) {
        if (!groups::setOwnerSignPubKeyV2(groupUid, s_groupOwnerSignPk)) {
          notifyGroupSecurityErrorV2(groupUid, "group_v3_store_failed", "Failed to set owner signing key");
          return;
        }
      }
      notifyGroupStatusV2(groupUid);
      s_pendingGroups = true;
      if (cmdId != 0) s_pendingGroupsCmdId = cmdId;
      scheduleInfoNotify(0, cmdId);
      return;
    }
    if (strcmp(cmd, "groupStatus") == 0) {
      const char* groupUid = doc["groupUid"];
      if (!groupUid || !groupUid[0]) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_bad", "Missing groupUid");
        return;
      }
      notifyGroupStatusV2(groupUid);
      return;
    }
    if (strcmp(cmd, "groupCanonicalRename") == 0) {
      const char* groupUid = doc["groupUid"];
      const char* canonicalName = doc["canonicalName"];
      if (!groupUid || !groupUid[0] || !canonicalName || !canonicalName[0]) {
        notifyGroupSecurityErrorV2(groupUid, "group_v3_name_bad", "Missing groupUid/canonicalName");
        return;
      }
      groups::GroupRole role = groups::GroupRole::None;
      if (!groups::getGroupV2(groupUid, nullptr, nullptr, 0, nullptr, 0, nullptr, &role, nullptr, nullptr)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v3_name_bad", "Unknown group");
        return;
      }
      if (strchr(canonicalName, '|') != nullptr) {
        notifyGroupSecurityErrorV2(groupUid, "group_v3_name_bad", "canonicalName contains invalid separator");
        return;
      }
      if (role != groups::GroupRole::Owner) {
        notifyGroupSecurityErrorV2(groupUid, "group_v3_name_denied", "Only owner can rename canonicalName");
        return;
      }
      if (!groups::setCanonicalNameV2(groupUid, canonicalName)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v3_name_bad", "Cannot set canonicalName");
        return;
      }
      notifyGroupStatusV2(groupUid);
      s_pendingGroups = true;
      if (cmdId != 0) s_pendingGroupsCmdId = cmdId;
      scheduleInfoNotify(0, cmdId);
      return;
    }
    if (strcmp(cmd, "groupInviteCreate") == 0) {
      const char* groupUid = doc["groupUid"];
      const char* roleStr = doc["role"];
      uint32_t ttlSec = doc["ttlSec"] | 600;
      if (!groupUid || !groupUid[0]) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Missing groupUid");
        return;
      }
      uint32_t channelId32 = 0;
      char groupTag[groups::GROUP_TAG_MAX_LEN + 1] = {0};
      char canonicalName[groups::GROUP_CANONICAL_NAME_MAX_LEN + 1] = {0};
      uint16_t keyVersion = 0;
      groups::GroupRole myRole = groups::GroupRole::None;
      uint8_t key[32];
      if (!groups::getGroupV2(groupUid, &channelId32, groupTag, sizeof(groupTag), canonicalName, sizeof(canonicalName), &keyVersion, &myRole, nullptr, nullptr) ||
          !groups::getGroupKeyV2(groupUid, key, &keyVersion)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Unknown groupUid");
        return;
      }
      if (myRole != groups::GroupRole::Owner) {
        notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_denied", "Only owner can issue signed invite");
        return;
      }
      if (strchr(canonicalName, '|') != nullptr) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "canonicalName contains invalid separator");
        return;
      }
      uint8_t ownerSignPubKey[groups::GROUP_OWNER_SIGN_PUBKEY_LEN] = {0};
      if (!groups::getOwnerSignPubKeyV2(groupUid, ownerSignPubKey)) {
        if (!groups::setOwnerSignPubKeyV2(groupUid, s_groupOwnerSignPk) ||
            !groups::getOwnerSignPubKeyV2(groupUid, ownerSignPubKey)) {
          notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Owner signing key is not set");
          return;
        }
      }
      if (memcmp(ownerSignPubKey, s_groupOwnerSignPk, sizeof(ownerSignPubKey)) != 0) {
        notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_denied", "Owner signing key mismatch");
        return;
      }
      size_t keyB64Len = 0;
      mbedtls_base64_encode(nullptr, 0, &keyB64Len, key, sizeof(key));
      char keyB64[80] = {0};
      if (mbedtls_base64_encode((unsigned char*)keyB64, sizeof(keyB64), &keyB64Len, key, sizeof(key)) != 0) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Key encode failed");
        return;
      }
      keyB64[keyB64Len] = '\0';
      size_t ownerPubB64Len = 0;
      mbedtls_base64_encode(nullptr, 0, &ownerPubB64Len, ownerSignPubKey, sizeof(ownerSignPubKey));
      char ownerPubB64[96] = {0};
      if (mbedtls_base64_encode((unsigned char*)ownerPubB64, sizeof(ownerPubB64), &ownerPubB64Len, ownerSignPubKey, sizeof(ownerSignPubKey)) != 0) {
        notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Owner key encode failed");
        return;
      }
      ownerPubB64[ownerPubB64Len] = '\0';
      if (!roleStr || !roleStr[0]) roleStr = "member";
      const uint32_t expiresAt = (uint32_t)(millis() / 1000) + ttlSec;
      char raw[420] = {0};
      int rawLen = snprintf(
          raw, sizeof(raw),
          "v3.1|%s|%lu|%s|%s|%u|%s|%s|%lu|%s",
          groupUid,
          (unsigned long)channelId32,
          groupTag,
          canonicalName,
          (unsigned)keyVersion,
          keyB64,
          roleStr,
          (unsigned long)expiresAt,
          ownerPubB64);
      if (rawLen <= 0 || (size_t)rawLen >= sizeof(raw)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Invite payload too large");
        return;
      }
      unsigned char sig[crypto_sign_BYTES] = {0};
      unsigned long long sigLen = 0;
      if (crypto_sign_detached(sig, &sigLen, (const unsigned char*)raw, (unsigned long long)rawLen, s_groupOwnerSignSk) != 0 || sigLen != crypto_sign_BYTES) {
        notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Invite sign failed");
        return;
      }
      size_t sigB64Len = 0;
      mbedtls_base64_encode(nullptr, 0, &sigB64Len, sig, sizeof(sig));
      char sigB64[140] = {0};
      if (mbedtls_base64_encode((unsigned char*)sigB64, sizeof(sigB64), &sigB64Len, sig, sizeof(sig)) != 0) {
        notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Signature encode failed");
        return;
      }
      sigB64[sigB64Len] = '\0';
      char inviteRaw[600] = {0};
      int inviteRawLen = snprintf(inviteRaw, sizeof(inviteRaw), "%s|%s", raw, sigB64);
      if (inviteRawLen <= 0 || (size_t)inviteRawLen >= sizeof(inviteRaw)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Invite payload too large");
        return;
      }
      size_t inviteB64Len = 0;
      mbedtls_base64_encode(nullptr, 0, &inviteB64Len, (const unsigned char*)inviteRaw, (size_t)inviteRawLen);
      char inviteB64[640] = {0};
      if (mbedtls_base64_encode((unsigned char*)inviteB64, sizeof(inviteB64), &inviteB64Len, (const unsigned char*)inviteRaw, (size_t)inviteRawLen) != 0) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Invite encode failed");
        return;
      }
      inviteB64[inviteB64Len] = '\0';
      JsonDocument ev(&s_bleJsonAllocator);
      ev["evt"] = "groupInvite";
      ev["groupUid"] = groupUid;
      ev["role"] = roleStr;
      ev["invite"] = inviteB64;
      ev["expiresAt"] = expiresAt;
      ev["channelId32"] = channelId32;
      ev["canonicalName"] = canonicalName;
      if (s_activeCmdId != 0) ev["cmdId"] = s_activeCmdId;
      char out[720];
      size_t outLen = serializeJson(ev, out);
      notifyJsonToApp(out, outLen);
      return;
    }
    if (strcmp(cmd, "groupInviteAccept") == 0) {
      const char* inviteB64 = doc["invite"];
      if (!inviteB64 || !inviteB64[0]) {
        notifyGroupSecurityErrorV2(nullptr, "group_v2_invite_bad", "Missing invite");
        return;
      }
      uint8_t raw[700] = {0};
      size_t rawLen = 0;
      if (mbedtls_base64_decode(raw, sizeof(raw) - 1, &rawLen, (const unsigned char*)inviteB64, strlen(inviteB64)) != 0 || rawLen == 0) {
        notifyGroupSecurityErrorV2(nullptr, "group_v2_invite_bad", "Bad invite base64");
        return;
      }
      raw[rawLen] = 0;
      char* savePtr = nullptr;
      char* version = strtok_r((char*)raw, "|", &savePtr);
      char* groupUid = strtok_r(nullptr, "|", &savePtr);
      char* channelStr = strtok_r(nullptr, "|", &savePtr);
      char* groupTag = strtok_r(nullptr, "|", &savePtr);
      char* canonicalName = strtok_r(nullptr, "|", &savePtr);
      char* keyVersionStr = strtok_r(nullptr, "|", &savePtr);
      char* keyB64 = strtok_r(nullptr, "|", &savePtr);
      char* roleStr = strtok_r(nullptr, "|", &savePtr);
      char* expiresStr = strtok_r(nullptr, "|", &savePtr);
      char* ownerPubB64 = strtok_r(nullptr, "|", &savePtr);
      char* sigB64 = strtok_r(nullptr, "|", &savePtr);
      if (!version || strcmp(version, "v3.1") != 0 || !groupUid || !channelStr || !groupTag || !canonicalName || !canonicalName[0] ||
          !keyVersionStr || !keyB64 || !roleStr || !expiresStr || !ownerPubB64 || !ownerPubB64[0] || !sigB64 || !sigB64[0]) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Malformed invite");
        return;
      }
      char signedRaw[420] = {0};
      int signedRawLen = snprintf(
          signedRaw, sizeof(signedRaw),
          "v3.1|%s|%s|%s|%s|%s|%s|%s|%s|%s",
          groupUid,
          channelStr,
          groupTag,
          canonicalName,
          keyVersionStr,
          keyB64,
          roleStr,
          expiresStr,
          ownerPubB64);
      if (signedRawLen <= 0 || (size_t)signedRawLen >= sizeof(signedRaw)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Malformed signed payload");
        return;
      }
      uint8_t ownerSignPubKey[groups::GROUP_OWNER_SIGN_PUBKEY_LEN] = {0};
      size_t ownerSignPubKeyLen = 0;
      if (mbedtls_base64_decode(ownerSignPubKey, sizeof(ownerSignPubKey), &ownerSignPubKeyLen,
              (const unsigned char*)ownerPubB64, strlen(ownerPubB64)) != 0 || ownerSignPubKeyLen != sizeof(ownerSignPubKey)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Invalid owner signing key");
        return;
      }
      unsigned char sig[crypto_sign_BYTES] = {0};
      size_t sigLen = 0;
      if (mbedtls_base64_decode(sig, sizeof(sig), &sigLen, (const unsigned char*)sigB64, strlen(sigB64)) != 0 || sigLen != crypto_sign_BYTES) {
        notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Invalid signature encoding");
        return;
      }
      if (crypto_sign_verify_detached(sig, (const unsigned char*)signedRaw, (unsigned long long)signedRawLen, ownerSignPubKey) != 0) {
        notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Owner signature verification failed");
        return;
      }
      const uint32_t nowSec = (uint32_t)(millis() / 1000);
      const uint32_t expiresAt = (uint32_t)strtoul(expiresStr, nullptr, 10);
      if (expiresAt == 0 || nowSec > expiresAt) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_expired", "Invite expired");
        return;
      }
      const uint32_t channelId32 = (uint32_t)strtoul(channelStr, nullptr, 10);
      const uint16_t keyVersion = (uint16_t)strtoul(keyVersionStr, nullptr, 10);
      uint8_t key[32];
      size_t keyLen = 0;
      if (channelId32 <= groups::GROUP_ALL ||
          mbedtls_base64_decode(key, sizeof(key), &keyLen, (const unsigned char*)keyB64, strlen(keyB64)) != 0 ||
          keyLen != 32) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Invalid key/channel");
        return;
      }
      groups::GroupRole role = parseGroupRole(roleStr);
      if (role == groups::GroupRole::None) role = groups::GroupRole::Member;
      uint8_t pinnedOwnerSignPubKey[groups::GROUP_OWNER_SIGN_PUBKEY_LEN] = {0};
      if (groups::getOwnerSignPubKeyV2(groupUid, pinnedOwnerSignPubKey) &&
          memcmp(pinnedOwnerSignPubKey, ownerSignPubKey, sizeof(ownerSignPubKey)) != 0) {
        notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Owner signing key mismatch");
        return;
      }
      if (!groups::upsertGroupV2(groupUid, channelId32, groupTag, canonicalName, key, keyVersion > 0 ? keyVersion : 1, role, 0)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_store_failed", "Cannot store accepted invite");
        return;
      }
      if (!groups::setOwnerSignPubKeyV2(groupUid, ownerSignPubKey)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Cannot persist owner signing key");
        return;
      }
      s_pendingGroups = true;
      scheduleInfoNotify();
      notifyGroupStatusV2(groupUid);
      return;
    }
    if (strcmp(cmd, "groupGrantIssue") == 0) {
      const char* groupUid = doc["groupUid"];
      const char* subjectId = doc["subjectId"];
      const char* roleStr = doc["role"];
      if (!groupUid || !groupUid[0] || !subjectId || !subjectId[0] || !roleStr || !roleStr[0]) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_grant_bad", "Missing groupUid/subjectId/role");
        return;
      }
      if (isSelfNodeHex(subjectId)) {
        groups::GroupRole role = parseGroupRole(roleStr);
        if (role == groups::GroupRole::None || !groups::setGroupRoleV2(groupUid, role)) {
          notifyGroupSecurityErrorV2(groupUid, "group_v2_grant_bad", "Invalid role or unknown group");
          return;
        }
      }
      notifyGroupStatusV2(groupUid);
      return;
    }
    if (strcmp(cmd, "groupRevoke") == 0) {
      const char* groupUid = doc["groupUid"];
      const char* subjectId = doc["subjectId"];
      uint32_t revEpoch = doc["revocationEpoch"] | 0;
      if (!groupUid || !groupUid[0] || !subjectId || !subjectId[0]) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_revoke_bad", "Missing groupUid/subjectId");
        return;
      }
      if (isSelfNodeHex(subjectId)) {
        groups::setGroupRoleV2(groupUid, groups::GroupRole::None);
        uint32_t curEpoch = 0;
        groups::GroupRole curRole = groups::GroupRole::None;
        if (groups::getGroupV2(groupUid, nullptr, nullptr, 0, nullptr, 0, nullptr, &curRole, &curEpoch, nullptr)) {
          if (revEpoch <= curEpoch) revEpoch = curEpoch + 1;
        }
        groups::setRevocationEpochV2(groupUid, revEpoch);
      }
      notifyGroupStatusV2(groupUid);
      return;
    }
    if (strcmp(cmd, "groupLeave") == 0) {
      const char* groupUid = doc["groupUid"];
      if (!groupUid || !groupUid[0]) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_leave_bad", "Missing groupUid");
        return;
      }
      if (!groups::removeGroupV2(groupUid)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_leave_bad", "Unknown group");
        return;
      }
      s_pendingGroups = true;
      if (cmdId != 0) s_pendingGroupsCmdId = cmdId;
      scheduleInfoNotify(0, cmdId);
      return;
    }
    if (strcmp(cmd, "groupRekey") == 0) {
      const char* groupUid = doc["groupUid"];
      const char* keyB64 = doc["groupKey"];
      const char* rekeyOpId = doc["rekeyOpId"];
      uint16_t keyVersion = (uint16_t)(doc["keyVersion"] | 0);
      if (!groupUid || !groupUid[0]) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_rekey_bad", "Missing groupUid");
        return;
      }
      if (keyVersion == 0) {
        uint16_t curVer = 0;
        if (groups::getGroupV2(groupUid, nullptr, nullptr, 0, nullptr, 0, &curVer, nullptr, nullptr, nullptr)) {
          uint32_t next = (uint32_t)curVer + 1u;
          if (next == 0 || next > 0xFFFFu) {
            keyVersion = 0xFFFFu;
          } else {
            keyVersion = (uint16_t)next;
          }
        } else {
          keyVersion = 1;
        }
      }
      uint8_t key[32];
      if (keyB64 && keyB64[0]) {
        size_t decLen = 0;
        if (mbedtls_base64_decode(key, sizeof(key), &decLen, (const unsigned char*)keyB64, strlen(keyB64)) != 0 || decLen != 32) {
          notifyGroupSecurityErrorV2(groupUid, "group_v2_key_bad", "Bad groupKey");
          return;
        }
      } else {
        for (size_t i = 0; i < sizeof(key); i += 4) {
          uint32_t r = esp_random();
          memcpy(&key[i], &r, (sizeof(key) - i >= 4) ? 4 : (sizeof(key) - i));
        }
      }
      if (!groups::updateGroupKeyV2(groupUid, key, keyVersion)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_rekey_bad", "Unknown group");
        return;
      }
      groups::GroupRole role = groups::GroupRole::None;
      uint16_t appliedVersion = 0;
      if (groups::getGroupV2(groupUid, nullptr, nullptr, 0, nullptr, 0, &appliedVersion, &role, nullptr, nullptr) &&
          role != groups::GroupRole::None) {
        groups::ackKeyAppliedV2(groupUid, appliedVersion);
      }
      notifyGroupRekeyProgressV2(groupUid, rekeyOpId, appliedVersion);
      notifyGroupStatusV2(groupUid);
      return;
    }
    if (strcmp(cmd, "groupAckKeyApplied") == 0) {
      const char* groupUid = doc["groupUid"];
      uint16_t keyVersion = (uint16_t)(doc["keyVersion"] | 0);
      if (!groupUid || !groupUid[0] || keyVersion == 0) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_ack_bad", "Missing groupUid/keyVersion");
        return;
      }
      if (!groups::ackKeyAppliedV2(groupUid, keyVersion)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_ack_bad", "Ack failed");
        return;
      }
      char selfHex[17] = {0};
      nodeIdToHex(node::getId(), selfHex);
      notifyGroupMemberKeyStateV2(groupUid, selfHex, "applied", (uint32_t)(millis() / 1000));
      notifyGroupStatusV2(groupUid);
      return;
    }
    if (strcmp(cmd, "groupSyncSnapshot") == 0) {
      JsonVariant groupsVar = doc["groups"];
      if (!groupsVar.is<JsonArray>()) {
        notifyGroupSecurityErrorV2(nullptr, "group_v2_snapshot_bad", "groups must be array");
        return;
      }
      JsonArray arr = groupsVar.as<JsonArray>();
      for (JsonVariant v : arr) {
        if (!v.is<JsonObject>()) continue;
        JsonObject g = v.as<JsonObject>();
        const char* groupUid = g["groupUid"];
        const char* groupTag = g["groupTag"];
        const char* canonicalName = g["canonicalName"];
        const char* keyB64 = g["groupKey"];
        const char* roleStr = g["myRole"];
        uint32_t channelId32 = g["channelId32"] | 0;
        uint16_t keyVersion = (uint16_t)(g["keyVersion"] | 1);
        uint32_t revEpoch = g["revocationEpoch"] | 0;
        if (!groupUid || !groupUid[0] || !groupTag || !groupTag[0] || !canonicalName || !canonicalName[0] ||
            channelId32 <= groups::GROUP_ALL || !keyB64 || !keyB64[0]) continue;
        uint8_t key[32];
        size_t decLen = 0;
        if (mbedtls_base64_decode(key, sizeof(key), &decLen, (const unsigned char*)keyB64, strlen(keyB64)) != 0 || decLen != 32) {
          continue;
        }
        groups::GroupRole role = parseGroupRole(roleStr);
        if (role == groups::GroupRole::None) role = groups::GroupRole::Member;
        if (!groups::upsertGroupV2(groupUid, channelId32, groupTag, canonicalName, key, keyVersion, role, revEpoch)) continue;
        if (g["ackApplied"] == true) groups::ackKeyAppliedV2(groupUid, keyVersion);
      }
      s_pendingGroups = true;
      if (cmdId != 0) s_pendingGroupsCmdId = cmdId;
      scheduleInfoNotify(0, cmdId);
      return;
    }
    if (strcmp(cmd, "routes") == 0 || strcmp(cmd, "mesh") == 0) {
      s_pendingRoutes = true;
      if (cmdId != 0) s_pendingRoutesCmdId = cmdId;
      return;
    }
    if (strcmp(cmd, "powersave") == 0) {
      if (doc["enabled"].is<bool>()) {
        powersave::setEnabled(doc["enabled"].as<bool>());
      }
      return;
    }
    if (strcmp(cmd, "regeneratePin") == 0) {
      ble::regeneratePasskey();
      scheduleInfoNotify(0, cmdId);
      return;
    }
    if (strcmp(cmd, "addGroup") == 0 || strcmp(cmd, "removeGroup") == 0 ||
        strcmp(cmd, "setGroupKey") == 0 || strcmp(cmd, "clearGroupKey") == 0 ||
        strcmp(cmd, "setGroupAdminCap") == 0 || strcmp(cmd, "clearGroupAdminCap") == 0 ||
        strcmp(cmd, "getGroupKey") == 0) {
      // V2-only firmware: legacy V1 group commands are intentionally disabled.
      ble::notifyError("group_legacy_cmd_unsupported", "Legacy V1 group command is not supported");
      return;
    }

    if (strcmp(cmd, "gps_sync") == 0) {
      int64_t utcMs = doc["utc_ms"] | 0;
      float lat = doc["lat"] | 0.0f;
      float lon = doc["lon"] | 0.0f;
      int16_t alt = doc["alt"] | 0;
      if (utcMs != 0) gps::setPhoneSync(utcMs, lat, lon, alt);
      return;
    }

    if (strcmp(cmd, "gps") == 0) {
      s_pendingGpsHasEnabled = doc["enabled"].is<bool>();
      if (s_pendingGpsHasEnabled) s_pendingGpsEnabled = doc["enabled"].as<bool>();
      s_pendingGpsHasPins = doc["rx"].is<int>() || doc["rx"].is<int64_t>() ||
          doc["tx"].is<int>() || doc["tx"].is<int64_t>() ||
          doc["en"].is<int>() || doc["en"].is<int64_t>();
      if (s_pendingGpsHasPins) {
        s_pendingGpsRx = doc["rx"] | -1;
        s_pendingGpsTx = doc["tx"] | -1;
        s_pendingGpsEn = doc["en"] | -1;
      }
      __sync_synchronize();
      s_pendingGps = true;
      if (cmdId != 0) s_pendingGpsCmdId = cmdId;
      return;
    }

    if (strcmp(cmd, "selftest") == 0 || strcmp(cmd, "test") == 0) {
      s_pendingSelftest = true;
      if (cmdId != 0) s_pendingSelftestCmdId = cmdId;
      return;
    }

    if (strcmp(cmd, "shutdown") == 0 || strcmp(cmd, "poweroff") == 0) {
      powersave::requestShutdown();
      return;
    }

    if (strcmp(cmd, "signalTest") == 0) {
      int n = neighbors::getCount();
      for (int i = 0; i < n && i < 8; i++) {
        uint8_t peerId[protocol::NODE_ID_LEN];
        if (!neighbors::getId(i, peerId)) continue;
        uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID];
        uint16_t pktId = ++s_diagPktIdCounter;
        size_t len = protocol::buildPacket(pkt, sizeof(pkt),
            node::getId(), peerId, 31, protocol::OP_PING, nullptr, 0,
            false, false, false, protocol::CHANNEL_DEFAULT, pktId);
        if (len > 0) {
          if (cmdId != 0) rememberPingCmdId(peerId, cmdId);
          uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(peerId));
          // Разнести во времени: плотный залп давал коллизии в эфире и лавину notify на BLE (mutex drop).
          uint32_t delayMs = 140u + (uint32_t)(i * 220u) + (uint32_t)(esp_random() % 90u);
          queueDeferredSend(pkt, len, txSf, delayMs, true);
          RIFTLINK_DIAG("PING", "event=PING_PLAN mode=signal_test to=%02X%02X pktId=%u sf=%u delay_ms=%lu",
              peerId[0], peerId[1], (unsigned)pktId, (unsigned)txSf, (unsigned long)delayMs);
        }
      }
      return;
    }

    if (strcmp(cmd, "traceroute") == 0) {
      const char* toStr = doc["to"];
      if (toStr && toStr[0]) {
        uint8_t target[protocol::NODE_ID_LEN];
        if (!parseFullNodeIdHex(toStr, target)) {
          ble::notifyError("traceroute_to_bad", "to must be full 16 hex node id");
          return;
        }
        routing::requestRoute(target);
        s_pendingRoutes = true;
        if (cmdId != 0) s_pendingRoutesCmdId = cmdId;
      }
      return;
    }

    if (strcmp(cmd, "read") == 0) {
      const char* fromStr = doc["from"];
      uint32_t msgId = doc["msgId"] | 0u;
      if (fromStr && fromStr[0] && msgId != 0) {
        uint8_t to[protocol::NODE_ID_LEN];
        if (!parseFullNodeIdHex(fromStr, to)) {
          ble::notifyError("read_from_bad", "from must be full 16 hex node id");
          return;
        }
        uint8_t payload[4];
        memcpy(payload, &msgId, 4);
        uint8_t pkt[protocol::PAYLOAD_OFFSET + 4];
        size_t pktLen = protocol::buildPacket(pkt, sizeof(pkt),
            node::getId(), to, 31, protocol::OP_READ, payload, 4, false, false);
        if (pktLen > 0) {
          uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(to));
          char reasonBuf[40];
          if (!queueTxPacket(pkt, pktLen, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
            queueDeferredSend(pkt, pktLen, txSf, 60 + (esp_random() % 40), true);
            RIFTLINK_DIAG("READ", "event=READ_TX_DEFER to=%02X%02X cause=%s",
                to[0], to[1], reasonBuf[0] ? reasonBuf : "?");
          }
        }
      }
      return;
    }

    if (strcmp(cmd, "ping") == 0) {
      const char* toStr = doc["to"];
      uint8_t to[protocol::NODE_ID_LEN];
      memset(to, 0xFF, protocol::NODE_ID_LEN);
      if (toStr && toStr[0] && !parseFullNodeIdHex(toStr, to)) {
        ble::notifyError("ping_to_bad", "to must be full 16 hex node id");
        return;
      }
      uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID];
      uint16_t pktId = ++s_diagPktIdCounter;
      size_t len = protocol::buildPacket(pkt, sizeof(pkt),
          node::getId(), to, 31, protocol::OP_PING, nullptr, 0,
          false, false, false, protocol::CHANNEL_DEFAULT, pktId);
      if (len > 0) {
        if (cmdId != 0) rememberPingCmdId(to, cmdId);
        uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(to));
        char reasonBuf[40];
        if (!queueTxPacket(pkt, len, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
          queueDeferredSend(pkt, len, txSf, 60 + (esp_random() % 40), true);
          RIFTLINK_DIAG("PING", "event=PING_TX_DEFER to=%02X%02X pktId=%u cause=%s",
              to[0], to[1], (unsigned)pktId, reasonBuf[0] ? reasonBuf : "?");
        } else {
          RIFTLINK_DIAG("PING", "event=PING_TX_QUEUED to=%02X%02X pktId=%u sf=%u",
              to[0], to[1], (unsigned)pktId, (unsigned)txSf);
        }
      }
      return;
    }

    if (strcmp(cmd, "voice") == 0) {
      const char* toStr = doc["to"];
      const char* dataStr = doc["data"];
      int chunk = doc["chunk"] | -1;
      int total = doc["total"] | -1;
      if (!toStr || !toStr[0] || !dataStr || chunk < 0 || total <= 0) return;
      if (strlen(dataStr) > BLE_VOICE_CHUNK_B64_BUF) return;  // иначе не влезает в 512 B одной записи

      if (!parseFullNodeIdHex(toStr, s_voiceTo)) {
        ble::notifyError("voice_to_bad", "to must be full 16 hex node id");
        return;
      }

      if (chunk == 0) {
        s_voiceBufLen = 0;
        s_voiceChunkTotal = total;
      }
      if (s_voiceChunkTotal != total) return;

      size_t b64Len = strlen(dataStr);
      size_t maxDec = VOICE_BUF_MAX - s_voiceBufLen;
      size_t olen;
      int r = mbedtls_base64_decode(s_voiceBuf + s_voiceBufLen, maxDec, &olen,
          (const unsigned char*)dataStr, b64Len);
      if (r == 0) {
        s_voiceBufLen += olen;
      }

      if (chunk == total - 1 && s_voiceBufLen > 0 && s_voiceBufLen <= voice_frag::MAX_VOICE_PLAIN) {
        voice_frag::send(s_voiceTo, s_voiceBuf, s_voiceBufLen);
        s_voiceBufLen = 0;
        s_voiceChunkTotal = -1;
      }
      return;
    }
}

namespace ble {

void processCommand(const uint8_t* data, size_t len) {
  bleHandleTxJson(data, (uint16_t)(len > 0xFFFF ? 0xFFFF : len));
}

bool init() {
  Serial.println("[BLE] Init...");
  memset(&s_bleDiag, 0, sizeof(s_bleDiag));
  s_bleDiagLastSnapshotMs = 0;
  s_bleDeinitInProgress = false;
  if (!NimBLEDevice::init(DEVICE_NAME)) {
    Serial.println("[BLE] NimBLEDevice::init FAILED — устройство не будет видно в скане BLE");
    return false;
  }
  // Reduce NimBLE scan log spam (and stack pressure in nimble_host logging path).
  esp_log_level_set("NimBLE", ESP_LOG_WARN);
  esp_log_level_set("NimBLEScan", ESP_LOG_WARN);
  esp_log_level_set("NimBLEDevice", ESP_LOG_WARN);
  esp_log_level_set("NimBLEServer", ESP_LOG_WARN);
  esp_log_level_set("NimBLEAdvertising", ESP_LOG_WARN);
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);
  NimBLEDevice::setMTU(517);

  loadOrGeneratePasskey();
  if (!loadOrGenerateGroupOwnerSigningKey()) {
    Serial.println("[BLE] owner signing key init FAILED");
    NimBLEDevice::deinit(true);
    return false;
  }
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(s_passkey);
  Serial.printf("[BLE] Passkey: %06u\n", (unsigned)s_passkey);
  if (!s_bleCmdQueue) {
    s_bleCmdQueue = xQueueCreate(kBleCmdQueueDepth, sizeof(BleCmdQueueItem));
    if (!s_bleCmdQueue) {
      Serial.println("[BLE] cmd queue alloc FAILED");
      NimBLEDevice::deinit(true);
      return false;
    }
  }
  if (!s_notifyMutex) {
    s_notifyMutex = xSemaphoreCreateMutex();
    if (!s_notifyMutex) {
      Serial.println("[BLE] notify mutex alloc FAILED");
      NimBLEDevice::deinit(true);
      if (s_bleCmdQueue) {
        vQueueDelete(s_bleCmdQueue);
        s_bleCmdQueue = nullptr;
      }
      return false;
    }
  }

  pServer = NimBLEDevice::createServer();
  ServerCallbacks* serverCallbacks = new (std::nothrow) ServerCallbacks();
  if (!serverCallbacks) {
    Serial.println("[BLE] Server callbacks alloc FAILED");
    NimBLEDevice::deinit(true);
    return false;
  }
  pServer->setCallbacks(serverCallbacks);

  NimBLEService* pService = pServer->createService(SERVICE_UUID);

  pTxChar = new (std::nothrow) RiftTxCharacteristic(
      NimBLEUUID(CHAR_TX_UUID), NIMBLE_PROPERTY::WRITE_NR, BLE_ATT_ATTR_MAX_LEN, pService);
  if (!pTxChar) {
    Serial.println("[BLE] TX characteristic alloc FAILED");
    NimBLEDevice::deinit(true);
    return false;
  }
  pService->addCharacteristic(pTxChar);

  pRxChar = pService->createCharacteristic(CHAR_RX_UUID, NIMBLE_PROPERTY::NOTIFY);
  pService->start();

  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  // UUID в main packet, имя RL-<full-id> в scan response (лимит 31 байт на пакет).
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN);  // general discoverable — иначе Android не видит
  advData.addServiceUUID(SERVICE_UUID);
  NimBLEAdvertisementData scanData;
  char advName[20];
  const uint8_t* id = node::getId();
  snprintf(advName, sizeof(advName), "RL-%02X%02X%02X%02X%02X%02X%02X%02X",
      id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
  scanData.setName(advName);
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->setScanResponseData(scanData);
  pAdvertising->enableScanResponse(true);  // Android иначе видит "unknown"
  if (!pAdvertising->start()) {
    Serial.println("[BLE] Advertising start FAILED — перезагрузите устройство");
    return false;
  }
  Serial.printf("[BLE] Advertising as '%s'\n", advName);
  s_bleInited = true;
  return true;
}

void deinit() {
  if (!s_bleInited) return;
  Serial.println("[BLE] Deinit...");
  s_bleDeinitInProgress = true;
#if !defined(RIFTLINK_DISABLE_BLS_N)
  if (s_blsScanActive) {
    NimBLEDevice::getScan()->stop();
    s_blsScanActive = false;
  }
#endif
  s_connected = false;
  pTxChar = nullptr;
  pRxChar = nullptr;
  pServer = nullptr;
  NimBLEDevice::deinit(true);
  if (s_bleCmdQueue) {
    vQueueDelete(s_bleCmdQueue);
    s_bleCmdQueue = nullptr;
  }
  if (s_notifyMutex) {
    vSemaphoreDelete(s_notifyMutex);
    s_notifyMutex = nullptr;
  }
  s_bleInited = false;
  s_bleDeinitInProgress = false;
  s_pendingOversizeCmdError = false;
  s_pendingOversizeCmdLen = 0;
  Serial.printf("[BLE] Deinit done, heap free=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void setOnSend(void (*cb)(const uint8_t* to, const char* text, uint8_t ttlMinutes,
    bool critical, uint8_t triggerType, uint32_t triggerValueMs, bool isSos)) {
  s_onSend = cb;
}

void setOnLocation(void (*cb)(float lat, float lon, int16_t alt, uint16_t radiusM, uint32_t expiryEpochSec)) {
  s_onLocation = cb;
}

void requestMsgNotify(const uint8_t* from, const char* text, uint32_t msgId, int rssi, uint8_t ttlMinutes,
    const char* lane, const char* type, uint32_t groupId, const char* groupUid) {
  if (!from || !text) return;
  if (s_pendingMsg) {
    Serial.printf("[BLE_CHAIN] stage=fw_request_msg action=overwrite prevMsgId=%u prevFrom=%02X%02X\n",
        (unsigned)s_pendingMsgId, s_pendingMsgFrom[0], s_pendingMsgFrom[1]);
  }
  const size_t textLen = strnlen(text, BLE_PENDING_MSG_TEXT_MAX);
  Serial.printf("[BLE_CHAIN] stage=fw_request_msg action=enqueue from=%02X%02X msgId=%u len=%u rssi=%d ttl=%u lane=%s type=%s group=%lu uid=%s mode=%s\n",
      from[0], from[1], (unsigned)msgId, (unsigned)textLen, rssi, (unsigned)ttlMinutes,
      lane ? lane : "normal", type ? type : "text",
      (unsigned long)groupId,
      (groupUid && groupUid[0]) ? groupUid : "-",
      transportModeTag());
  memcpy(s_pendingMsgFrom, from, 8);
  strncpy(s_pendingMsgText, text, BLE_PENDING_MSG_TEXT_MAX);
  s_pendingMsgText[BLE_PENDING_MSG_TEXT_MAX] = '\0';
  s_pendingMsgId = msgId;
  s_pendingMsgRssi = rssi;
  s_pendingMsgTtl = ttlMinutes;
  strncpy(s_pendingMsgLane, lane ? lane : "normal", sizeof(s_pendingMsgLane) - 1);
  s_pendingMsgLane[sizeof(s_pendingMsgLane) - 1] = '\0';
  strncpy(s_pendingMsgType, type ? type : "text", sizeof(s_pendingMsgType) - 1);
  s_pendingMsgType[sizeof(s_pendingMsgType) - 1] = '\0';
  s_pendingMsgGroupId = groupId;
  if (groupUid && groupUid[0]) {
    strncpy(s_pendingMsgGroupUid, groupUid, sizeof(s_pendingMsgGroupUid) - 1);
    s_pendingMsgGroupUid[sizeof(s_pendingMsgGroupUid) - 1] = '\0';
  } else {
    s_pendingMsgGroupUid[0] = '\0';
  }
  s_pendingMsg = true;
}

void notifyMsg(const uint8_t* from, const char* text, uint32_t msgId, int rssi, uint8_t ttlMinutes,
    const char* lane, const char* type, uint32_t groupId, const char* groupUid) {
  if (!hasActiveTransport()) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "msg";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["text"] = text;
  if (msgId != 0) doc["msgId"] = msgId;
  if (rssi != 0) doc["rssi"] = rssi;
  if (ttlMinutes != 0) doc["ttl"] = ttlMinutes;
  if (lane && lane[0]) doc["lane"] = lane;
  if (type && type[0]) doc["type"] = type;
  if (groupId > 0) doc["group"] = groupId;
  if (groupUid && groupUid[0]) doc["groupUid"] = groupUid;

  const size_t jsonLen = measureJson(doc);
  char* buf = static_cast<char*>(s_bleJsonAllocator.allocate(jsonLen + 1));
  if (!buf) {
    ble::notifyError("notify_oom", "Failed to allocate msg notify buffer");
    return;
  }
  size_t len = serializeJson(doc, buf, jsonLen + 1);
  notifyJsonToApp(buf, len);
  s_bleJsonAllocator.deallocate(buf);
}

void notifyDelivered(const uint8_t* from, uint32_t msgId, int rssi) {
  if (!hasActiveTransport()) return;
  s_bleDiag.statusDelivered++;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "delivered";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["msgId"] = msgId;
  if (rssi != 0) doc["rssi"] = rssi;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  Serial.printf("[BLE_CHAIN] stage=fw_msg_state action=emit evt=delivered msgId=%u from=%s\n",
      (unsigned)msgId, fromHex);
  notifyJsonToApp(buf, len);
}

void notifyRead(const uint8_t* from, uint32_t msgId, int rssi) {
  if (!hasActiveTransport()) return;
  s_bleDiag.statusRead++;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "read";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["msgId"] = msgId;
  if (rssi != 0) doc["rssi"] = rssi;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  Serial.printf("[BLE_CHAIN] stage=fw_msg_state action=emit evt=read msgId=%u from=%s\n",
      (unsigned)msgId, fromHex);
  notifyJsonToApp(buf, len);
}

void notifySent(const uint8_t* to, uint32_t msgId) {
  if (!hasActiveTransport()) return;
  s_bleDiag.statusSent++;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "sent";
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(toHex + i*2, 3, "%02X", to[i]);
  doc["to"] = toHex;
  doc["msgId"] = msgId;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  Serial.printf("[BLE_CHAIN] stage=fw_msg_state action=emit evt=sent msgId=%u to=%s\n",
      (unsigned)msgId, toHex);
  notifyJsonToApp(buf, len);
}

void notifyWaitingKey(const uint8_t* to) {
  if (!pRxChar || !s_connected || !to) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "waiting_key";
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(toHex + i * 2, 3, "%02X", to[i]);
  doc["to"] = toHex;

  char buf[96];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyUndelivered(const uint8_t* to, uint32_t msgId) {
  if (!hasActiveTransport()) return;
  s_bleDiag.statusUndelivered++;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "undelivered";
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(toHex + i*2, 3, "%02X", to[i]);
  doc["to"] = toHex;
  doc["msgId"] = msgId;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  Serial.printf("[BLE_CHAIN] stage=fw_msg_state action=emit evt=undelivered msgId=%u to=%s\n",
      (unsigned)msgId, toHex);
  notifyJsonToApp(buf, len);
}

void notifyBroadcastDelivery(uint32_t msgId, int delivered, int total) {
  if (!hasActiveTransport()) return;
  s_bleDiag.statusBroadcastDelivery++;

  JsonDocument doc(&s_bleJsonAllocator);
  if (total > 0 && delivered == 0) {
    doc["evt"] = "undelivered";
  } else {
    doc["evt"] = "broadcast_delivery";
  }
  doc["msgId"] = msgId;
  doc["delivered"] = delivered;
  doc["total"] = total;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  Serial.printf("[BLE_CHAIN] stage=fw_msg_state action=emit evt=%s msgId=%u delivered=%d total=%d\n",
      (total > 0 && delivered == 0) ? "undelivered" : "broadcast_delivery",
      (unsigned)msgId, delivered, total);
  notifyJsonToApp(buf, len);
}

void notifyLocation(const uint8_t* from, float lat, float lon, int16_t alt, int rssi) {
  if (!hasActiveTransport()) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "location";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["lat"] = lat;
  doc["lon"] = lon;
  doc["alt"] = alt;
  if (rssi != 0) doc["rssi"] = rssi;

  char buf[200];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyTelemetry(const uint8_t* from, uint16_t batteryMv, uint16_t heapKb, int rssi) {
  if (!hasActiveTransport()) return;
  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "telemetry";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["battery"] = batteryMv;
  doc["heapKb"] = heapKb;
  if (rssi != 0) doc["rssi"] = rssi;
  char buf[320];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyRelayProof(const uint8_t* relayedBy, const uint8_t* from, const uint8_t* to, uint16_t pktId, uint8_t opcode) {
  if (!hasActiveTransport()) return;
  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "relayProof";
  char byHex[17] = {0};
  char fromHex[17] = {0};
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) {
    snprintf(byHex + i * 2, 3, "%02X", relayedBy[i]);
    snprintf(fromHex + i * 2, 3, "%02X", from[i]);
    snprintf(toHex + i * 2, 3, "%02X", to[i]);
  }
  doc["relayedBy"] = byHex;
  doc["from"] = fromHex;
  doc["to"] = toHex;
  doc["pktId"] = pktId;
  doc["opcode"] = opcode;
  char buf[220];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyTimeCapsuleReleased(const uint8_t* to, uint32_t msgId, uint8_t triggerType) {
  if (!hasActiveTransport()) return;
  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "timeCapsuleReleased";
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(toHex + i * 2, 3, "%02X", to[i]);
  doc["to"] = toHex;
  doc["msgId"] = msgId;
  if (triggerType == 1) doc["trigger"] = "target_online";
  else if (triggerType == 2) doc["trigger"] = "deliver_after_time";
  char buf[140];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyInfo() {
  const uint32_t cmdId = s_emitInfoCmdId;
  s_emitInfoCmdId = 0;
  if (!hasActiveTransport()) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "info";
  if (cmdId != 0) doc["cmdId"] = cmdId;
  const uint8_t* id = node::getId();
  char idHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(idHex + i*2, 3, "%02X", id[i]);
  doc["id"] = idHex;
  char nick[33];
  node::getNickname(nick, sizeof(nick));
  if (nick[0]) doc["nickname"] = nick;
  doc["region"] = region::getCode();
  doc["freq"] = region::getFreq();
  doc["power"] = region::getPower();
  if (region::getChannelCount() > 0) {
    doc["channel"] = region::getChannel();
  }
  doc["radioMode"] = (radio_mode::current() == radio_mode::BLE) ? "ble" : "wifi";
  doc["radioVariant"] = (radio_mode::currentWifiVariant() == radio_mode::STA) ? "sta" : "ap";
  doc["wifiConnected"] = wifi::isConnected();
  char wifiSsid[64] = {0};
  char wifiIp[24] = {0};
  wifi::getStatus(wifiSsid, sizeof(wifiSsid), wifiIp, sizeof(wifiIp));
  if (wifiSsid[0]) doc["wifiSsid"] = wifiSsid;
  if (wifiIp[0]) doc["wifiIp"] = wifiIp;
  doc["espnowChannel"] = esp_now_slots::getChannel();
  doc["espnowAdaptive"] = esp_now_slots::isAdaptive();
  doc["version"] = RIFTLINK_VERSION;
  doc["sf"] = radio::getSpreadingFactor();
  doc["bw"] = radio::getBandwidth();
  doc["cr"] = radio::getCodingRate();
  doc["modemPreset"] = (int)radio::getModemPreset();
  uint16_t batteryMv = telemetry::readBatteryMv();
  if (batteryMv > 0) doc["batteryMv"] = batteryMv;
  int batPct = telemetry::batteryPercent();
  if (batPct >= 0) doc["batteryPercent"] = batPct;
  doc["charging"] = telemetry::isCharging();
  if (gps::hasTime()) {
    doc["timeHour"] = gps::getHour();
    doc["timeMinute"] = gps::getMinute();
  }
  int offlinePending = offline_queue::getPendingCount();
  if (offlinePending > 0) doc["offlinePending"] = offlinePending;
  int offlineCourierPending = offline_queue::getCourierPendingCount();
  int offlineDirectPending = offline_queue::getDirectPendingCount();
  if (offlineCourierPending > 0) doc["offlineCourierPending"] = offlineCourierPending;
  if (offlineDirectPending > 0) doc["offlineDirectPending"] = offlineDirectPending;
  doc["gpsPresent"] = gps::isPresent();
  doc["gpsEnabled"] = gps::isEnabled();
  doc["gpsFix"] = gps::hasFix();
  doc["powersave"] = powersave::isEnabled();
  doc["blePin"] = s_passkey;

  JsonArray grpArr = doc["groups"].to<JsonArray>();
  const int ng = groups::getV2Count();
  for (int i = 0; i < ng; i++) {
    char uid[groups::GROUP_UID_MAX_LEN + 1] = {0};
    char tag[groups::GROUP_TAG_MAX_LEN + 1] = {0};
    char canonicalName[groups::GROUP_CANONICAL_NAME_MAX_LEN + 1] = {0};
    uint32_t channelId32 = 0;
    if (!groups::getV2At(i, uid, sizeof(uid), &channelId32, tag, sizeof(tag), canonicalName, sizeof(canonicalName), nullptr, nullptr, nullptr, nullptr)) continue;
    if (channelId32 == 0) continue;
    grpArr.add(channelId32);
  }
  JsonArray grpV2Arr = doc["groupsV2"].to<JsonArray>();
  const int nv2 = groups::getV2Count();
  // Слишком длинный info режется на несколько notify; обрезка по числу групп снижает «обрыв посередине строки» в первом чанке.
  constexpr int kMaxGroupsV2InBleInfo = 12;
  const int nv2Cap = (nv2 > kMaxGroupsV2InBleInfo) ? kMaxGroupsV2InBleInfo : nv2;
  for (int i = 0; i < nv2Cap; i++) {
    char uid[groups::GROUP_UID_MAX_LEN + 1] = {0};
    char tag[groups::GROUP_TAG_MAX_LEN + 1] = {0};
    char canonicalName[groups::GROUP_CANONICAL_NAME_MAX_LEN + 1] = {0};
    uint32_t channelId32 = 0;
    uint16_t keyVersion = 0;
    groups::GroupRole role = groups::GroupRole::None;
    uint32_t revEpoch = 0;
    bool ackApplied = false;
    if (!groups::getV2At(i, uid, sizeof(uid), &channelId32, tag, sizeof(tag), canonicalName, sizeof(canonicalName), &keyVersion, &role, &revEpoch, &ackApplied)) continue;
    JsonObject gv2 = grpV2Arr.add<JsonObject>();
    gv2["groupUid"] = uid;
    gv2["groupTag"] = tag;
    gv2["canonicalName"] = canonicalName;
    gv2["channelId32"] = channelId32;
    gv2["keyVersion"] = keyVersion;
    gv2["myRole"] = groupRoleToStr(role);
    gv2["revocationEpoch"] = revEpoch;
    gv2["ackApplied"] = ackApplied;
  }

  JsonArray arr = doc["neighbors"].to<JsonArray>();
  JsonArray rssiArr = doc["neighborsRssi"].to<JsonArray>();
  JsonArray hasKeyArr = doc["neighborsHasKey"].to<JsonArray>();
  int n = neighbors::getCount();
  char hex[17];
  uint8_t peerId[protocol::NODE_ID_LEN];
  for (int i = 0; i < n; i++) {
    neighbors::getIdHex(i, hex);
    arr.add(hex);
    rssiArr.add(neighbors::getRssi(i));
    if (neighbors::getId(i, peerId)) hasKeyArr.add(x25519_keys::hasKeyFor(peerId));
    else hasKeyArr.add(false);
  }

  JsonArray routesArr = doc["routes"].to<JsonArray>();
  int nr = routing::getRouteCount();
  uint8_t dest[8], nextHop[8];
  uint8_t hops;
  int8_t rssi;
  int trustScore;
  for (int i = 0; i < nr; i++) {
    if (!routing::getRouteAt(i, dest, nextHop, &hops, &rssi, &trustScore)) continue;
    JsonObject ro = routesArr.add<JsonObject>();
    char d[17], nh[17];
    for (int j = 0; j < 8; j++) { snprintf(d + j*2, 3, "%02X", dest[j]); snprintf(nh + j*2, 3, "%02X", nextHop[j]); }
    d[16] = nh[16] = '\0';
    ro["dest"] = d;
    ro["nextHop"] = nh;
    ro["hops"] = hops;
    ro["rssi"] = (int)rssi;
    ro["modemPreset"] = (int)radio::getModemPreset();
    ro["sf"] = radio::getSpreadingFactor();
    ro["bw"] = radio::getBandwidth();
    ro["cr"] = radio::getCodingRate();
    ro["trustScore"] = trustScore;
  }

  // Буфер 600 обрезал большой `info` (соседи/маршруты) → невалидный JSON в приложении.
  size_t plen = serializeJson(doc, s_notifyInfoPayload, sizeof(s_notifyInfoPayload));
  if (plen == 0) return;
  if (cmdId == 0 && s_lastInfoPayloadLen == plen && memcmp(s_lastInfoPayload, s_notifyInfoPayload, plen) == 0) {
    Serial.println("[BLE_CHAIN] stage=fw_notify_info action=skip reason=unchanged");
    return;
  }
  if (plen <= sizeof(s_lastInfoPayload)) {
    memcpy(s_lastInfoPayload, s_notifyInfoPayload, plen);
    s_lastInfoPayloadLen = plen;
  }
  notifyJsonToApp(s_notifyInfoPayload, plen);
}

void notifyInvite() {
  const uint32_t cmdId = s_emitInviteCmdId;
  s_emitInviteCmdId = 0;
  if (!hasActiveTransport()) return;

  uint8_t pubKey[32];
  if (!x25519_keys::getOurPublicKey(pubKey)) return;

  size_t b64Len;
  mbedtls_base64_encode(nullptr, 0, &b64Len, pubKey, 32);
  char pubKeyB64[64];
  if (mbedtls_base64_encode((unsigned char*)pubKeyB64, sizeof(pubKeyB64), &b64Len, pubKey, 32) != 0) return;
  pubKeyB64[b64Len] = '\0';

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "invite";
  if (cmdId != 0) doc["cmdId"] = cmdId;
  const uint8_t* id = node::getId();
  char idHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(idHex + i*2, 3, "%02X", id[i]);
  doc["id"] = idHex;
  doc["pubKey"] = pubKeyB64;
  if (s_inviteTokenValid && (int32_t)(millis() - s_inviteExpiryMs) < 0) {
    char tokHex[17] = {0};
    for (int i = 0; i < 8; i++) snprintf(tokHex + i * 2, 3, "%02X", s_inviteToken[i]);
    doc["inviteToken"] = tokHex;
    doc["inviteTtlMs"] = (uint32_t)(s_inviteExpiryMs - millis());
  }

  uint8_t chKey[32];
  if (crypto::getChannelKey(chKey)) {
    size_t chB64Len;
    mbedtls_base64_encode(nullptr, 0, &chB64Len, chKey, 32);
    char chKeyB64[64];
    if (mbedtls_base64_encode((unsigned char*)chKeyB64, sizeof(chKeyB64), &chB64Len, chKey, 32) == 0) {
      chKeyB64[chB64Len] = '\0';
      doc["channelKey"] = chKeyB64;
    }
  }

  char buf[200];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyRoutes() {
  const uint32_t cmdId = s_emitRoutesCmdId;
  s_emitRoutesCmdId = 0;
  if (!hasActiveTransport()) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "routes";
  if (cmdId != 0) doc["cmdId"] = cmdId;
  JsonArray arr = doc["routes"].to<JsonArray>();
  int n = routing::getRouteCount();
  uint8_t dest[8], nextHop[8];
  uint8_t hops;
  int8_t rssi;
  int trustScore;
  char d[17], nh[17];
  for (int i = 0; i < n; i++) {
    if (!routing::getRouteAt(i, dest, nextHop, &hops, &rssi, &trustScore)) continue;
    JsonObject ro = arr.add<JsonObject>();
    for (int j = 0; j < 8; j++) { snprintf(d + j*2, 3, "%02X", dest[j]); snprintf(nh + j*2, 3, "%02X", nextHop[j]); }
    d[16] = nh[16] = '\0';
    ro["dest"] = d;
    ro["nextHop"] = nh;
    ro["hops"] = hops;
    ro["rssi"] = (int)rssi;
    ro["modemPreset"] = (int)radio::getModemPreset();
    ro["sf"] = radio::getSpreadingFactor();
    ro["bw"] = radio::getBandwidth();
    ro["cr"] = radio::getCodingRate();
    ro["trustScore"] = trustScore;
  }
  char buf[400];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyGroups() {
  const uint32_t cmdId = s_emitGroupsCmdId;
  s_emitGroupsCmdId = 0;
  if (!hasActiveTransport()) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "groups";
  if (cmdId != 0) doc["cmdId"] = cmdId;
  JsonArray arr = doc["groups"].to<JsonArray>();
  const int n = groups::getV2Count();
  for (int i = 0; i < n; i++) {
    char uid[groups::GROUP_UID_MAX_LEN + 1] = {0};
    char tag[groups::GROUP_TAG_MAX_LEN + 1] = {0};
    char canonicalName[groups::GROUP_CANONICAL_NAME_MAX_LEN + 1] = {0};
    uint32_t channelId32 = 0;
    if (!groups::getV2At(i, uid, sizeof(uid), &channelId32, tag, sizeof(tag), canonicalName, sizeof(canonicalName), nullptr, nullptr, nullptr, nullptr)) continue;
    if (channelId32 == 0) continue;
    arr.add(channelId32);
  }
  JsonArray arrV2 = doc["groupsV2"].to<JsonArray>();
  const int nv2 = groups::getV2Count();
  for (int i = 0; i < nv2; i++) {
    char uid[groups::GROUP_UID_MAX_LEN + 1] = {0};
    char tag[groups::GROUP_TAG_MAX_LEN + 1] = {0};
    char canonicalName[groups::GROUP_CANONICAL_NAME_MAX_LEN + 1] = {0};
    uint32_t channelId32 = 0;
    uint16_t keyVersion = 0;
    groups::GroupRole role = groups::GroupRole::None;
    uint32_t revEpoch = 0;
    bool ackApplied = false;
    if (!groups::getV2At(i, uid, sizeof(uid), &channelId32, tag, sizeof(tag), canonicalName, sizeof(canonicalName), &keyVersion, &role, &revEpoch, &ackApplied)) continue;
    JsonObject gv2 = arrV2.add<JsonObject>();
    gv2["groupUid"] = uid;
    gv2["groupTag"] = tag;
    gv2["canonicalName"] = canonicalName;
    gv2["channelId32"] = channelId32;
    gv2["keyVersion"] = keyVersion;
    gv2["myRole"] = groupRoleToStr(role);
    gv2["revocationEpoch"] = revEpoch;
    gv2["ackApplied"] = ackApplied;
  }

  const size_t needed = measureJson(doc);
  if (needed >= 768) {
    notifyError("groups_payload_too_large", "groups payload exceeds BLE notify buffer");
    return;
  }
  char buf[768];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  notifyJsonToApp(buf, len);
}

void requestNeighborsNotify() {
  s_pendingNeighbors = true;
  s_pendingNeighborsCmdId = 0;
}

void notifyNeighbors() {
  const uint32_t cmdId = s_emitNeighborsCmdId;
  s_emitNeighborsCmdId = 0;
  if (!hasActiveTransport()) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "neighbors";
  if (cmdId != 0) doc["cmdId"] = cmdId;
  JsonArray arr = doc["neighbors"].to<JsonArray>();
  JsonArray rssiArr = doc["rssi"].to<JsonArray>();
  JsonArray hasKeyArr = doc["hasKey"].to<JsonArray>();
  int n = neighbors::getCount();
  char hex[17];
  uint8_t peerId[protocol::NODE_ID_LEN];
  for (int i = 0; i < n; i++) {
    neighbors::getIdHex(i, hex);
    arr.add(hex);
    rssiArr.add(neighbors::getRssi(i) != 0 ? neighbors::getRssi(i) : (int)0);
    if (neighbors::getId(i, peerId)) hasKeyArr.add(x25519_keys::hasKeyFor(peerId));
    else hasKeyArr.add(false);
  }

  char buf[320];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyWifi(bool connected, const char* ssid, const char* ip) {
  if (!hasActiveTransport()) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "wifi";
  doc["connected"] = connected;
  if (ssid) doc["ssid"] = ssid;
  if (ip) doc["ip"] = ip;

  char buf[150];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyRegion(const char* code, float freq, int power, int channel, uint32_t cmdId) {
  if (!hasActiveTransport()) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "region";
  if (cmdId != 0) doc["cmdId"] = cmdId;
  doc["region"] = code;
  doc["freq"] = freq;
  doc["power"] = power;
  if (channel >= 0) doc["channel"] = channel;

  char buf[120];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyGps(bool present, bool enabled, bool hasFix, int rx, int tx, int en) {
  const uint32_t cmdId = s_emitGpsCmdId;
  s_emitGpsCmdId = 0;
  if (!hasActiveTransport()) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "gps";
  if (cmdId != 0) doc["cmdId"] = cmdId;
  doc["present"] = present;
  doc["enabled"] = enabled;
  doc["hasFix"] = hasFix;
  if (rx >= 0) doc["rx"] = rx;
  if (tx >= 0) doc["tx"] = tx;
  if (en >= 0) doc["en"] = en;

  char buf[120];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyError(const char* code, const char* msg) {
  if (!hasActiveTransport() || !code || !msg) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "error";
  doc["code"] = code;
  doc["msg"] = msg;
  if (s_activeCmdId != 0) doc["cmdId"] = s_activeCmdId;

  char buf[200];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyPong(const uint8_t* from, int rssi) {
  if (!hasActiveTransport()) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "pong";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  if (rssi != 0) doc["rssi"] = rssi;
  const uint32_t cmdId = takePingCmdIdForFrom(from);
  if (cmdId != 0) doc["cmdId"] = cmdId;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifySelftest(bool radioOk, bool displayOk, uint16_t batteryMv, uint32_t heapFree, uint32_t cmdId) {
  if (!hasActiveTransport()) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "selftest";
  if (cmdId != 0) doc["cmdId"] = cmdId;
  doc["radioOk"] = radioOk;
  doc["displayOk"] = displayOk;
  doc["antennaOk"] = radioOk;
  doc["batteryMv"] = batteryMv;
  int batPct = telemetry::batteryPercent();
  if (batPct >= 0) doc["batteryPercent"] = batPct;
  doc["charging"] = telemetry::isCharging();
  doc["heapFree"] = heapFree;

  char buf[160];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyVoice(const uint8_t* from, const uint8_t* data, size_t dataLen) {
  if (!hasActiveTransport() || !data) return;

  const size_t CHUNK_RAW = BLE_VOICE_CHUNK_RAW_MAX;
  size_t totalChunks = (dataLen + CHUNK_RAW - 1) / CHUNK_RAW;

  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);

  for (size_t i = 0; i < totalChunks; i++) {
    size_t off = i * CHUNK_RAW;
    size_t chunkLen = dataLen - off;
    if (chunkLen > CHUNK_RAW) chunkLen = CHUNK_RAW;

    unsigned char b64[BLE_VOICE_CHUNK_B64_BUF + 1];
    size_t olen;
    if (mbedtls_base64_encode(b64, sizeof(b64) - 1, &olen, data + off, chunkLen) != 0) break;
    if (olen < sizeof(b64)) b64[olen] = '\0';

    JsonDocument doc(&s_bleJsonAllocator);
    doc["evt"] = "voice";
    doc["from"] = fromHex;
    doc["chunk"] = (int)i;
    doc["total"] = (int)totalChunks;
    doc["data"] = (const char*)b64;

    char buf[BLE_ATT_MAX_JSON_BYTES + 1];
    size_t len = serializeJson(doc, buf);
    if (len > 0 && len <= BLE_ATT_MAX_JSON_BYTES) {
      notifyJsonToApp(buf, len);
    }
  }
}

void update() {
  bleDiagMaybeSnapshot("tick", 15000);
  if (s_pendingOversizeCmdError && hasActiveTransport()) {
    const uint16_t droppedLen = s_pendingOversizeCmdLen;
    s_pendingOversizeCmdError = false;
    s_pendingOversizeCmdLen = 0;
    char buf[180];
    const int n = snprintf(
        buf, sizeof(buf),
        "{\"evt\":\"error\",\"code\":\"payload_too_long\",\"msg\":\"JSON exceeds 512 bytes\",\"len\":%u,\"limit\":%u}",
        (unsigned)droppedLen, (unsigned)BLE_ATT_MAX_JSON_BYTES);
    if (n > 0) notifyJsonToApp(buf, (size_t)n);
    return;
  }
  if (s_bleCmdQueue) {
    BleCmdQueueItem cmdItem{};
    // Parse commands outside nimble_host context to avoid stack canary under heavy app traffic.
    for (uint8_t i = 0; i < kBleCmdConsumePerTick; i++) {
      if (xQueueReceive(s_bleCmdQueue, &cmdItem, 0) != pdTRUE) break;
      s_bleDiag.cmdConsumed++;
      bleHandleTxJson(cmdItem.data, cmdItem.len);
    }
  }
  // GPS: применить в main loop (thread-safe: setPins удаляет s_serial, main читает в gps::update)
  if (s_pendingGps) {
    s_pendingGps = false;
    if (s_pendingGpsHasEnabled) gps::setEnabled(s_pendingGpsEnabled);
    if (s_pendingGpsHasPins) {
      gps::setPins(s_pendingGpsRx, s_pendingGpsTx, s_pendingGpsEn);
      gps::saveConfig();
    }
    if (hasActiveTransport()) {
      s_emitGpsCmdId = s_pendingGpsCmdId;
      s_pendingGpsCmdId = 0;
      int rx, tx, en;
      gps::getPins(&rx, &tx, &en);
      notifyGps(gps::isPresent(), gps::isEnabled(), gps::hasFix(), rx, tx, en);
    }
    return;
  }
  if (hasActiveTransport()) {
    if (s_pendingMsg || s_pendingInfo || s_pendingGroups || s_pendingRoutes || s_pendingNeighbors || s_pendingInvite) {
      Serial.printf("[BLE_CHAIN] stage=fw_update action=pending mode=%s msg=%u info=%u groups=%u routes=%u neighbors=%u invite=%u\n",
          transportModeTag(),
          (unsigned)s_pendingMsg, (unsigned)s_pendingInfo, (unsigned)s_pendingGroups,
          (unsigned)s_pendingRoutes, (unsigned)s_pendingNeighbors, (unsigned)s_pendingInvite);
    }
    // Сначала применить nickname (thread-safe: main loop пишет в node)
    if (s_pendingNickname) {
      s_pendingNickname = false;
      node::setNickname(s_pendingNicknameBuf);
    }
    // По одному notify за вызов — не перегружать BLE stack
    if (s_pendingInfo && (int32_t)(millis() - s_pendingInfoNotBeforeMs) >= 0) {
      s_pendingInfo = false;
      s_emitInfoCmdId = s_pendingInfoCmdId;
      s_pendingInfoCmdId = 0;
      notifyInfo();
      vTaskDelay(pdMS_TO_TICKS(2));  // дать BLE отправить большой payload
      return;
    }
    if (s_pendingMsg) {
      s_pendingMsg = false;
      notifyMsg(s_pendingMsgFrom, s_pendingMsgText, s_pendingMsgId, s_pendingMsgRssi, s_pendingMsgTtl,
          s_pendingMsgLane, s_pendingMsgType, s_pendingMsgGroupId, s_pendingMsgGroupUid);
      return;
    }
    if (s_pendingGroups) {
      s_pendingGroups = false;
      s_emitGroupsCmdId = s_pendingGroupsCmdId;
      s_pendingGroupsCmdId = 0;
      notifyGroups();
      return;
    }
    if (s_pendingRoutes) {
      s_pendingRoutes = false;
      s_emitRoutesCmdId = s_pendingRoutesCmdId;
      s_pendingRoutesCmdId = 0;
      notifyRoutes();
      return;
    }
    if (s_pendingNeighbors) {
      s_pendingNeighbors = false;
      s_emitNeighborsCmdId = s_pendingNeighborsCmdId;
      s_pendingNeighborsCmdId = 0;
      notifyNeighbors();
      return;
    }
    if (s_pendingInvite) {
      s_pendingInvite = false;
      s_emitInviteCmdId = s_pendingInviteCmdId;
      s_pendingInviteCmdId = 0;
      notifyInvite();
      return;
    }
    if (s_pendingSelftest) {
      s_pendingSelftest = false;
      const uint32_t stCmdId = s_pendingSelftestCmdId;
      s_pendingSelftestCmdId = 0;
      selftest::Result r;
      selftest::run(&r);
      notifySelftest(r.radioOk, r.displayOk, r.batteryMv, r.heapFree, stCmdId);
      return;
    }
    if (s_pendingGroupSend) {
      s_pendingGroupSend = false;
      bool ok = msg_queue::enqueueGroup(s_pendingGroupId, s_pendingGroupText);
      if (!ok) notifyError("group_send", "Сообщение слишком длинное или ошибка шифрования");
    }
  }
  if (s_bleInited && !s_bleDeinitInProgress &&
      radio_mode::current() == radio_mode::BLE &&
      pServer && !s_connected && pServer->getConnectedCount() == 0) {
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (!pAdv->isAdvertising() && millis() >= s_advRetryNotBeforeMs) {
      if (!pAdv->start()) {
        s_advRetryNotBeforeMs = millis() + BLE_ADV_RESTART_BACKOFF_MS;
      }
    }
  }

  // BLS-N: при подключённом телефоне — BLE scan для приёма RTS от соседей
  // Paper: отключено — BLE scan вызывает Malloc failed при heap ~10KB (много advertisers)
  // OLED + Wi‑Fi: после esp_wifi_init остаётся мало internal — не стартуем scan без порога (иначе NimBLE BLE_INIT).
#if !defined(USE_EINK) && !defined(RIFTLINK_DISABLE_BLS_N)
  if (s_connected && BLS_SCAN_DURING_GATT_SESSION) {
    static uint32_t s_blsHeapSkipLogMs = 0;
    if (!blsScanHeapOk()) {
      if (millis() - s_blsHeapSkipLogMs > 60000) {
        s_blsHeapSkipLogMs = millis();
        Serial.printf("[RiftLink] BLS: BLE scan пропущен (internal free=%u largest=%u; нужно >=%u / >=%u)\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned)BLS_SCAN_MIN_FREE_INTERNAL, (unsigned)BLS_SCAN_MIN_LARGEST_INTERNAL);
      }
    } else {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && !pScan->isScanning() && !s_blsScanActive) {
      if (s_blsScanEnded && (millis() - s_blsScanLastStart) < BLS_SCAN_RESTART_DELAY_MS)
        ;  // ждём перед повторной попыткой
      else {
        if (s_blsScanEnded) s_blsScanEnded = false;
        // Do not request duplicate scan callbacks; duplicates create callback storms and can
        // overflow nimble_host stack on dense RF environments.
        pScan->setScanCallbacks(&s_blsScanCallbacks, false);
        if (pScan->start(BLS_SCAN_DURATION_SEC, false, false)) {
          s_blsScanActive = true;
          s_blsScanLastStart = millis();
        } else {
          s_blsScanEnded = true;
          s_blsScanLastStart = millis();  // throttle при конфликте scan+GATT
        }
      }
    }
    }
  }
#endif  // !USE_EINK && !RIFTLINK_DISABLE_BLS_N
}

bool isConnected() { return s_connected; }

uint32_t getPasskey() { return s_passkey; }

void regeneratePasskey() {
  s_passkey = esp_random() % 900000 + 100000;
  nvs_handle_t h;
  if (nvs_open(NVS_BLE_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u32(h, NVS_KEY_BLE_PIN, s_passkey);
    nvs_commit(h);
    nvs_close(h);
  }
  NimBLEDevice::setSecurityPasskey(s_passkey);
  Serial.printf("[BLE] New passkey: %06u\n", (unsigned)s_passkey);
}

}  // namespace ble
