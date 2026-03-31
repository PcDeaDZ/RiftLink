/**
 * BLE — Nordic UART Service (те же UUID, что NimBLE на ESP) + JSON NDJSON для Flutter.
 */

#include "async_tasks.h"
#include "ble/ble.h"
#include "crypto/crypto.h"
#include "frag/frag.h"
#include "gps/gps.h"
#include "groups/groups.h"
#include "kv.h"
#include "locale/locale.h"
#include "menu_nrf.h"
#include "log.h"
#include "msg_queue/msg_queue.h"
#include "neighbors/neighbors.h"
#include "node/node.h"
#include "offline_queue/offline_queue.h"
#include "protocol/packet.h"
#include "radio/radio.h"
#include "region/region.h"
#include "routing/routing.h"
#include "selftest/selftest.h"
#include "telemetry/telemetry.h"
#include "version.h"
#include "voice_buffers/voice_buffers.h"
#include "voice_frag/voice_frag.h"
#include "x25519_keys/x25519_keys.h"
#include "nrf_wdt_feed.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <bluefruit.h>
#include <cstdlib>
#include <sodium.h>
#include <string.h>
#include <atomic>

extern "C" size_t xPortGetFreeHeapSize(void);

#ifndef RIFTLINK_BLE_JSON_LINE_MAX
#define RIFTLINK_BLE_JSON_LINE_MAX 512
#endif
#ifndef RIFTLINK_BLE_RX_LINE_MAX
#define RIFTLINK_BLE_RX_LINE_MAX 512
#endif
/**
 * Типичный Android после обмена MTU: ATT payload ≈ MTU−3 (часто 244 B). Одна NDJSON-строка без разрыва
 * по нескольким notify — JSON должен укладываться в один фрейм (+ '\n' отдельным байтом в том же write).
 * Иначе Flutter склеивает чанки, но отладочный jsonDecode на неполном буфере даёт «Unterminated string».
 */
#ifndef RIFTLINK_BLE_ATT_SAFE_JSON_BYTES
#define RIFTLINK_BLE_ATT_SAFE_JSON_BYTES 230
#endif

/** Nordic SoftDevice: нет валидного соединения (см. ble_gap.h). Не вызывать notify с «мёртвым» handle. */
#ifndef BLE_CONN_HANDLE_INVALID
#define BLE_CONN_HANDLE_INVALID ((uint16_t)0xFFFFu)
#endif

namespace ble {

static void emitJsonDoc(JsonDocument& doc);
static void emitJsonDocStackOrChunked(JsonDocument& doc);
static bool scheduleNdjsonLineOwned(char* heap, size_t len);
static void pumpBleTx(size_t maxChunkBytes, unsigned maxLineLines);

void notifyInfo();
void notifyInvite();
void getAdvertisingName(char* out, size_t outLen);

static void (*s_onSend)(const uint8_t* to, const char* text, uint8_t ttlMinutes, bool critical,
    uint8_t triggerType, uint32_t triggerValueMs, bool isSos) = nullptr;
static void (*s_onLocation)(float lat, float lon, int16_t alt, uint16_t radiusM, uint32_t expiryEpochSec) = nullptr;

static constexpr size_t kTxJsonMax = (size_t)RIFTLINK_BLE_JSON_LINE_MAX;
static constexpr size_t kRxLineMax = (size_t)RIFTLINK_BLE_RX_LINE_MAX;

/** Те же 128-bit UUID, что Nordic UART / Adafruit BLEUart (см. BLEUart.cpp в Bluefruit52Lib). */
static const uint8_t kNusUuidService[16] = {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5,
    0x01, 0x00, 0x40, 0x6E};
static const uint8_t kNusUuidChrRxd[16] = {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00,
    0x40, 0x6E}; /* central write → узел */
static const uint8_t kNusUuidChrTxd[16] = {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00,
    0x40, 0x6E}; /* notify → телефон */

static BLEService s_riftNus(kNusUuidService);
static BLECharacteristic s_riftNusTxd(kNusUuidChrTxd);
static BLECharacteristic s_riftNusRxd(kNusUuidChrRxd);

/** Паритет с ESP ble.cpp: одна GATT-запись от приложения = один элемент очереди, разбор в ble::update(). */
static constexpr uint8_t kInCmdQDepth = 12;
/** Меньше чем на ESP FreeRTOS: один поток + SoftDevice — длинная пачка JSON за один ble::update голод радио/BLE. */
static constexpr uint8_t kInCmdConsumePerUpdate = 2;
struct NrfBleInCmdItem {
  uint16_t len;
  uint8_t data[kRxLineMax];
};
static NrfBleInCmdItem s_inCmdQ[kInCmdQDepth];
static volatile uint8_t s_inCmdQHead = 0;
static volatile uint8_t s_inCmdQTail = 0;
static std::atomic<uint8_t> s_inCmdQCount{0};
static uint32_t s_inCmdDroppedQueueFull = 0;

static void resetIncomingCmdQueue() {
  s_inCmdQHead = s_inCmdQTail = 0;
  s_inCmdQCount.store(0, std::memory_order_relaxed);
}

/**
 * Добавление команды в RX-очередь из колбэка SoftDevice (IRQ контекст).
 * Защита от гонки с consumeIncomingCmdQueue() через отключение прерываний.
 */
static bool enqueueIncomingCmd(const uint8_t* data, uint16_t len) {
  if (!data || len == 0 || len > (uint16_t)kRxLineMax) return false;
  
  // Критическая секция: гонка между IRQ (enqueue) и main loop (consume)
  uint32_t irq_flags = __get_PRIMASK();
  __disable_irq();
  
  if (s_inCmdQCount.load(std::memory_order_relaxed) >= kInCmdQDepth) {
    s_inCmdDroppedQueueFull++;
    if (!irq_flags) __enable_irq();
    return false;
  }
  
  memcpy(s_inCmdQ[s_inCmdQHead].data, data, len);
  s_inCmdQ[s_inCmdQHead].len = len;
  s_inCmdQHead = (uint8_t)((s_inCmdQHead + 1) % kInCmdQDepth);
  s_inCmdQCount.fetch_add(1, std::memory_order_relaxed);
  
  if (!irq_flags) __enable_irq();
  return true;
}

/** Глубина BLE TX-очереди (строки NDJSON); при переполнении — drop старейших (как burst на ESP). */
static constexpr unsigned kOutQDepth = 8;

/** Один элемент FIFO: короткая строка ≤512 B или одна длинная NDJSON-строка (чанки + \\n в конце). Порядок строгий — «труба» без перемешивания. */
struct BleOutSlot {
  enum Kind : uint8_t { K_LINE = 0, K_CHUNKED = 1 } kind;
  union {
    char line[RIFTLINK_BLE_JSON_LINE_MAX];
    struct {
      char* heap;
      size_t len;
      size_t off;
    } ch;
  };
};
static BleOutSlot s_outQ[kOutQDepth];
static volatile uint8_t s_outHead = 0;
static volatile uint8_t s_outTail = 0;
static volatile uint8_t s_outCount = 0;

/** Куча evt:node из волны info (chunked): ждём drain перед neighbors/routes/groups. */
static char* s_infoWaveNodeChunkHeap = nullptr;
static bool s_emittingFromInfoWaveNode = false;
static uint32_t s_txQueueDroppedCount = 0;

static void bleOutQueueDropOldestSlot() {
  if (s_outCount == 0) return;
  BleOutSlot& s = s_outQ[s_outTail];
  const char* slotType = (s.kind == BleOutSlot::K_CHUNKED) ? "chunked_ndjson" : "short_line";
  if (s.kind == BleOutSlot::K_CHUNKED) {
    if (s.ch.heap == s_infoWaveNodeChunkHeap) s_infoWaveNodeChunkHeap = nullptr;
    free(s.ch.heap);
  }
  s_outTail = (uint8_t)((s_outTail + 1) % kOutQDepth);
  s_outCount--;
  s_txQueueDroppedCount++;
  RIFTLINK_DIAG("BLE", "event=TX_DROP reason=out_queue_full type=%s total_dropped=%u", 
      slotType, (unsigned)s_txQueueDroppedCount);
}

static void resetBleOutQueueOnDisconnect() {
  for (unsigned i = 0; i < s_outCount; i++) {
    unsigned idx = (s_outTail + i) % kOutQDepth;
    BleOutSlot& s = s_outQ[idx];
    if (s.kind == BleOutSlot::K_CHUNKED) {
      free(s.ch.heap);
    }
  }
  s_infoWaveNodeChunkHeap = nullptr;
  s_outHead = 0;
  s_outTail = 0;
  s_outCount = 0;
}

static bool s_inited = false;
static bool s_connected = false;

static inline bool bleLinkUpForNus() {
  if (!s_connected) return false;
  if (Bluefruit.connected() == 0) return false;
  const uint16_t hdl = Bluefruit.connHandle();
  if (hdl == BLE_CONN_HANDLE_INVALID) return false;
  return true;
}

static inline bool nusNotifyEnabled() {
  if (!bleLinkUpForNus()) return false;
  return s_riftNusTxd.notifyEnabled(Bluefruit.connHandle());
}

/**
 * Отправка notify с retry при временной ошибке SoftDevice.
 * Экспоненциальная задержка между попытками для восстановления ресурсов.
 */
static bool nusNotifyBytesWithRetry(const uint8_t* p, uint16_t len, uint8_t maxRetries = 3) {
  if (len == 0) return true;
  if (!bleLinkUpForNus()) return false;
  
  for (uint8_t attempt = 0; attempt < maxRetries; attempt++) {
    if (s_riftNusTxd.notify(Bluefruit.connHandle(), p, len)) {
      return true;
    }
    // Пауза для восстановления SoftDevice: 2ms, 4ms, 6ms
    yield();
    delay(2 + attempt * 2);
  }
  
  RIFTLINK_DIAG("BLE", "event=TX_FAIL reason=nus_notify_retry_exhausted len=%u", (unsigned)len);
  return false;
}

static inline bool nusNotifyBytes(const uint8_t* p, uint16_t len) {
  if (len == 0) return true;
  if (!bleLinkUpForNus()) return false;
  return s_riftNusTxd.notify(Bluefruit.connHandle(), p, len);
}

/** Конец предыдущего `ble::update()`: был ли активный линк (для сброса TX-очереди только на нисходящем фронте). */
static bool s_bleConnAtPrevUpdateEnd = false;
/** Лог CONNECTED/DISCONNECTED только из `syncBleConnectionAtUpdateEntry` — не из BLE-колбэка (Serial/SoftDevice). */
static bool s_bleConnPrevForDiag = false;
static uint8_t s_lastBleDiscReason = 0;
/** После CONNECT — ~20 с пошаговых POST_CONN_TRACE (где оборвалось до ребута/зависания). 0 = выкл. */
static uint32_t s_postConnTraceUntilMs = 0;
/** Сколько полных проходов ble::update() с цепочкой u01…u11 (чтобы не заспамить USB при каждом loop). */
static uint8_t s_postConnBleDetailPassesLeft = 0;
static uint8_t s_postConnMainTraceLeft = 0;

/** Трасса после connect: на T114 только Serial1 (GPIO6) — USB CDC Serial.printf может блокироваться навсегда при полном TX. */
static void bleEmitPostConnTraceLine(const char* tag) {
  char buf[112];
  snprintf(buf, sizeof(buf), "[RiftLink] POST_CONN_TRACE tag=%s ms=%lu", tag, (unsigned long)millis());
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  Serial1.println(buf);
#else
  RIFTLINK_DIAG("BLE", "event=POST_CONN_TRACE tag=%s ms=%lu", tag, (unsigned long)millis());
#endif
}

static void postConnTraceIf(const char* tag) {
  if (!tag || !tag[0]) return;
  if (s_postConnTraceUntilMs == 0) return;
  if ((int32_t)(millis() - s_postConnTraceUntilMs) >= 0) {
    s_postConnTraceUntilMs = 0;
    s_postConnBleDetailPassesLeft = 0;
    s_postConnMainTraceLeft = 0;
    return;
  }
  if (tag[0] == 'u' && s_postConnBleDetailPassesLeft == 0) return;
  if (strncmp(tag, "main_", 5) == 0 && s_postConnMainTraceLeft == 0) return;
  bleEmitPostConnTraceLine(tag);
  if (strncmp(tag, "main_", 5) == 0 && s_postConnMainTraceLeft > 0) s_postConnMainTraceLeft--;
}

/** Волна info разнесена по вызовам `update()` — иначе один кадр: глубокий стек (несколько JsonDocument) + долгие delay → HardFault/«мёртвый» loop. */
enum class InfoWavePhase : uint8_t { Idle, EmitNode, EmitNeighbors, EmitRoutes, EmitGroups };
static InfoWavePhase s_infoWavePhase = InfoWavePhase::Idle;
static bool s_infoWavePending = false;
static uint32_t s_infoWavePendingCmdId = 0;
static uint32_t s_infoWaveSeq = 0;
static uint32_t s_infoWaveCmdId = 0;
/** Волна info: по одному evt на фазу; при длинном NDJSON ждём drain pending перед следующей фазой. */
static bool s_infoWaveNodeEmitted = false;
static bool s_infoWaveNeighborsEmitted = false;
static bool s_infoWaveRoutesEmitted = false;
static bool s_infoWaveGroupsEmitted = false;
static uint32_t s_passkey = 0;

/** Полный сброс волны info при disconnect — иначе processInfoWaveStep() зависает на проверке chunked heap. */
static void resetInfoWaveOnDisconnect() {
  s_infoWavePending = false;
  s_infoWavePhase = InfoWavePhase::Idle;
  s_infoWaveNodeEmitted = false;
  s_infoWaveNeighborsEmitted = false;
  s_infoWaveRoutesEmitted = false;
  s_infoWaveGroupsEmitted = false;
  s_emittingFromInfoWaveNode = false;
  /* Критично: освободить chunked heap, иначе утечка памяти + блокировка волны */
  if (s_infoWaveNodeChunkHeap) {
    free(s_infoWaveNodeChunkHeap);
    s_infoWaveNodeChunkHeap = nullptr;
  }
}

static uint8_t s_inviteToken[8];
static uint32_t s_inviteExpiryMs = 0;
static bool s_inviteTokenValid = false;
static uint32_t s_emitInviteCmdId = 0;
/** Как ble.cpp: общий seq для evt:node → neighbors → routes → groups. */
static uint32_t s_bleSyncSeq = 0;
static uint32_t s_emitRoutesCmdId = 0;
static uint32_t s_emitGroupsCmdId = 0;
static uint32_t s_emitGpsCmdId = 0;
/** Как ble.cpp: текущий cmdId при разборе JSON в handleJsonCommand (evt:error, groupInvite, …). */
static uint32_t s_activeCmdId = 0;

/** Как ESP `requestMsgNotify` — буферизация, реальный `notifyMsg` из `ble::update()` (не из handlePacket). */
static constexpr size_t kBlePendingMsgTextMax = frag::MAX_MSG_PLAIN;
static uint8_t s_pendingMsgFrom[protocol::NODE_ID_LEN]{};
static char s_pendingMsgText[kBlePendingMsgTextMax + 1]{};
static uint32_t s_pendingMsgId = 0;
static int s_pendingMsgRssi = 0;
static uint8_t s_pendingMsgTtl = 0;
static char s_pendingMsgLane[10] = "normal";
static char s_pendingMsgType[10] = "text";
static uint32_t s_pendingMsgGroupId = 0;
static char s_pendingMsgGroupUid[groups::GROUP_UID_MAX_LEN + 1]{};
static bool s_pendingMsg = false;
/** Как ESP `requestNeighborsNotify` → PEND_NEIGHBORS. */
static bool s_pendingNeighbors = false;
/** Как ESP: pong не теряется при переполнении TX-очереди — повтор в `flushPendingBleOutbound`. */
static bool s_pendingPong = false;
static uint8_t s_pendingPongFrom[protocol::NODE_ID_LEN]{};
static int s_pendingPongRssi = 0;
static uint16_t s_pendingPongPktId = 0;

static void flushPendingBleOutbound();

class ActiveCmdScope {
 public:
  explicit ActiveCmdScope(uint32_t cmdId) { s_activeCmdId = cmdId; }
  ~ActiveCmdScope() { s_activeCmdId = 0; }
};

static constexpr const char* KV_PIN = "ble_pin";
static constexpr const char* KV_GPK = "gpk1";
static constexpr const char* KV_GSK = "gsk1";

static uint8_t s_groupOwnerSignPk[crypto_sign_PUBLICKEYBYTES] = {0};
static uint8_t s_groupOwnerSignSk[crypto_sign_SECRETKEYBYTES] = {0};
static bool s_groupOwnerSignReady = false;

static bool loadOrGenerateGroupOwnerSigningKey() {
  if (s_groupOwnerSignReady) return true;
  // После crypto::init() sodium уже поднят; иначе первый sodium_init здесь может зависать до SoftDevice.
  Serial.println("[RiftLink] BLE: signing key: sodium_init (fast if already done)");
  Serial.flush();
  if (sodium_init() < 0) return false;
  Serial.println("[RiftLink] BLE: signing key: KV gpk/gsk...");
  Serial.flush();
  size_t pkLen = sizeof(s_groupOwnerSignPk);
  size_t skLen = sizeof(s_groupOwnerSignSk);
  const bool hasPk =
      riftlink_kv::getBlob(KV_GPK, s_groupOwnerSignPk, &pkLen) && pkLen == sizeof(s_groupOwnerSignPk);
  const bool hasSk =
      riftlink_kv::getBlob(KV_GSK, s_groupOwnerSignSk, &skLen) && skLen == sizeof(s_groupOwnerSignSk);
  if (!hasPk || !hasSk) {
    Serial.println("[RiftLink] BLE: signing key: crypto_sign_keypair...");
    Serial.flush();
    if (crypto_sign_keypair(s_groupOwnerSignPk, s_groupOwnerSignSk) != 0) return false;
    (void)riftlink_kv::setBlob(KV_GPK, s_groupOwnerSignPk, sizeof(s_groupOwnerSignPk));
    (void)riftlink_kv::setBlob(KV_GSK, s_groupOwnerSignSk, sizeof(s_groupOwnerSignSk));
  }
  s_groupOwnerSignReady = true;
  return true;
}

static bool bin32ToBase64(const uint8_t* bin32, char* out, size_t outLen) {
  const size_t need = sodium_base64_encoded_len(32, sodium_base64_VARIANT_ORIGINAL);
  if (outLen < need) return false;
  sodium_bin2base64(out, outLen, bin32, 32, sodium_base64_VARIANT_ORIGINAL);
  return true;
}

static bool parseFullNodeIdHex(const char* hexId, uint8_t out[protocol::NODE_ID_LEN]) {
  if (!hexId || !out) return false;
  if (strlen(hexId) != protocol::NODE_ID_LEN * 2) return false;
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    char hi = hexId[i * 2];
    char lo = hexId[i * 2 + 1];
    bool hiOk = (hi >= '0' && hi <= '9') || (hi >= 'a' && hi <= 'f') || (hi >= 'A' && hi <= 'F');
    bool loOk = (lo >= '0' && lo <= '9') || (lo >= 'a' && lo <= 'f') || (lo >= 'A' && lo <= 'F');
    if (!hiOk || !loOk) return false;
    char hx[3] = {hi, lo, 0};
    out[i] = (uint8_t)strtoul(hx, nullptr, 16);
  }
  return true;
}

static bool b64Decode32(const char* b64, uint8_t out[32]) {
  if (!b64 || !b64[0]) return false;
  size_t binLen = 0;
  if (sodium_base642bin(out, 32, b64, strlen(b64), nullptr, &binLen, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0)
    return false;
  return binLen == 32;
}

static int parseModemPresetValue(JsonDocument& doc) {
  auto parseText = [](const char* s) -> int {
    if (!s || !s[0]) return -1;
    if (strcmp(s, "speed") == 0 || strcmp(s, "spaid") == 0) return 0;
    if (strcmp(s, "normal") == 0) return 1;
    if (strcmp(s, "range") == 0) return 2;
    if (strcmp(s, "maxrange") == 0 || strcmp(s, "max_range") == 0) return 3;
    return -1;
  };
  int p = -1;
  if (!doc["preset"].isNull()) {
    if (doc["preset"].is<int>()) p = doc["preset"].as<int>();
    else if (doc["preset"].is<const char*>()) p = parseText(doc["preset"].as<const char*>());
  }
  if (p < 0 && !doc["value"].isNull()) {
    if (doc["value"].is<int>()) p = doc["value"].as<int>();
    else if (doc["value"].is<const char*>()) p = parseText(doc["value"].as<const char*>());
  }
  if (p >= 0 && p < 4) return p;
  return -1;
}

static uint16_t s_diagPktIdCounter = 1;

/** Паритет с ble.cpp: голос BLE → mesh (OP_VOICE_MSG). */
static constexpr size_t BLE_VOICE_CHUNK_RAW_MAX = 300;
static constexpr size_t BLE_VOICE_CHUNK_B64_BUF = 400;
static size_t s_voiceBufLen = 0;
static int s_voiceChunkTotal = -1;
static uint8_t s_voiceTo[protocol::NODE_ID_LEN];
static bool s_voiceBleLocked = false;

/** Как ble.cpp: при отсутствии PONG — повторные OP_PING (диагностика). */
static constexpr uint8_t kPingRetryMaxExtra = 2;
struct PingRetryState {
  bool active = false;
  uint8_t to[protocol::NODE_ID_LEN]{};
  uint8_t txSf = 12;
  uint32_t deadlineMs = 0;
  uint8_t retriesLeft = 0;
};
static PingRetryState s_pingRetry;

static uint32_t pingAckWaitMs(uint8_t neighborSfHint) {
  uint8_t modemSf = radio::getSpreadingFactor();
  if (modemSf < 7 || modemSf > 12) modemSf = 12;
  uint8_t hint = neighborSfHint;
  if (hint < 7 || hint > 12) hint = modemSf;
  const uint8_t sf = modemSf > hint ? modemSf : hint;
  return 2000 + (uint32_t)(sf - 6) * 500;
}

static void pingRetryTick() {
  if (!s_pingRetry.active) return;
  const uint32_t now = millis();
  if ((int32_t)(now - s_pingRetry.deadlineMs) < 0) return;
  if (s_pingRetry.retriesLeft == 0) {
    s_pingRetry.active = false;
    return;
  }
  uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID];
  uint16_t pktId = ++s_diagPktIdCounter;
  size_t len = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), s_pingRetry.to, 31, protocol::OP_PING, nullptr, 0,
      false, false, false, protocol::CHANNEL_DEFAULT, pktId);
  if (len == 0) {
    s_pingRetry.deadlineMs = now + 400;
    return;
  }
  char reasonBuf[40];
  if (!queueTxPacket(pkt, len, s_pingRetry.txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
    queueDeferredSend(pkt, len, s_pingRetry.txSf, 60 + (uint32_t)(random(40)), true);
    RIFTLINK_DIAG("PING", "event=PING_RETRY_DEFER to=%02X%02X pktId=%u left=%u cause=%s",
        s_pingRetry.to[0], s_pingRetry.to[1], (unsigned)pktId, (unsigned)s_pingRetry.retriesLeft,
        reasonBuf[0] ? reasonBuf : "?");
  } else {
    RIFTLINK_DIAG("PING", "event=PING_RETRY_QUEUED to=%02X%02X pktId=%u left=%u sf=%u",
        s_pingRetry.to[0], s_pingRetry.to[1], (unsigned)pktId, (unsigned)s_pingRetry.retriesLeft,
        (unsigned)s_pingRetry.txSf);
  }
  s_pingRetry.retriesLeft--;
  if (s_pingRetry.retriesLeft == 0) {
    s_pingRetry.active = false;
  } else {
    s_pingRetry.deadlineMs = now + pingAckWaitMs(s_pingRetry.txSf);
  }
}

struct PendingPingCmdId {
  bool used = false;
  uint8_t to[protocol::NODE_ID_LEN] = {0};
  uint32_t cmdId = 0;
  uint32_t expiresAtMs = 0;
};
static PendingPingCmdId s_pendingPingCmdIds[8];

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

static uint32_t peekPingCmdIdForFrom(const uint8_t from[protocol::NODE_ID_LEN]) {
  if (!from) return 0;
  const uint32_t now = millis();
  for (int i = 0; i < (int)(sizeof(s_pendingPingCmdIds) / sizeof(s_pendingPingCmdIds[0])); i++) {
    auto& e = s_pendingPingCmdIds[i];
    if (!e.used) continue;
    if ((int32_t)(now - e.expiresAtMs) >= 0) continue;
    if (memcmp(e.to, from, protocol::NODE_ID_LEN) == 0) return e.cmdId;
  }
  return 0;
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
      const uint32_t id = e.cmdId;
      e.used = false;
      return id;
    }
  }
  return 0;
}

/** Сырые байты 0x00–0x1F в сериализованном JSON: строгий jsonDecode на Android (Control character in string). */
static bool bleSerializedJsonHasRawControlChars(const char* s, size_t len) {
  if (!s) return true;
  for (size_t i = 0; i < len; i++) {
    if ((unsigned char)s[i] < 0x20u) return true;
  }
  return false;
}

/** Любая C-строка из внешнего источника перед присвоением в JsonDocument (копия в буфер + in-place). */
static void sanitizeBleJsonCStringInPlace(char* s) {
  if (!s) return;
  for (size_t i = 0; s[i]; i++) {
    if ((unsigned char)s[i] < 0x20u) s[i] = ' ';
  }
}

/** Поля паспорта узла — паритет с ESP `appendNodePassportFieldsToDoc` (без Wi‑Fi/ESP‑NOW как «живых»). */
static void appendNrfPassportFields(JsonDocument& doc) {
  char nick[33];
  node::getNickname(nick, sizeof(nick));
  sanitizeBleJsonCStringInPlace(nick);
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  /* Полный ник в evt:node (до 32), BLE — chunked NDJSON при необходимости. */
  if (nick[0]) doc["nickname"] = nick;
#else
  if (nick[0] && strlen(nick) <= 24) doc["nickname"] = nick;
#endif
  char regionBuf[16];
  strncpy(regionBuf, region::getCode(), sizeof(regionBuf) - 1);
  regionBuf[sizeof(regionBuf) - 1] = '\0';
  sanitizeBleJsonCStringInPlace(regionBuf);
  doc["region"] = regionBuf;
  doc["freq"] = region::getFreq();
  doc["power"] = region::getPower();
  if (region::getChannelCount() > 0) doc["channel"] = region::getChannel();
  doc["radioMode"] = "ble";
  doc["wifiConnected"] = false;
  doc["espnowChannel"] = 0;
  doc["espnowAdaptive"] = false;
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
  doc["powersave"] = false;
}

/** Лимит одной строки BLE — 512 B; при переполнении убираем редкие поля. */
static void trimBlePassportDoc(JsonDocument& doc) {
  if (measureJson(doc) < kTxJsonMax) return;
  doc.remove("nickname");
  if (measureJson(doc) < kTxJsonMax) return;
  doc.remove("timeHour");
  doc.remove("timeMinute");
  if (measureJson(doc) < kTxJsonMax) return;
  doc.remove("offlineCourierPending");
  doc.remove("offlineDirectPending");
  if (measureJson(doc) < kTxJsonMax) return;
  doc.remove("offlinePending");
}

/** Ужать документ под один BLE notify (типичный Android ~244 B payload), не нарушая NDJSON одной строкой. */
static void shrinkJsonDocToAttSafe(JsonDocument& doc) {
  static const char* kTrimOrder[] = {
      "heapFree",
      "nickname",
      "timeHour",
      "timeMinute",
      "offlineDirectPending",
      "offlineCourierPending",
      "offlinePending",
      "gpsPresent",
      "gpsEnabled",
      "gpsFix",
      "batteryPercent",
      "charging",
      "batteryMv",
      "wifiConnected",
      "espnowChannel",
      "espnowAdaptive",
      "powersave",
      "channel",
      "radioMode",
      "freq",
      "power",
      "region",
      "modemPreset",
      "cr",
      "bw",
      "lang",
      "platform",
      "version",
  };
  for (size_t i = 0; i < sizeof(kTrimOrder) / sizeof(kTrimOrder[0]); i++) {
    if (measureJson(doc) <= (size_t)RIFTLINK_BLE_ATT_SAFE_JSON_BYTES) return;
    doc.remove(kTrimOrder[i]);
  }
  while (measureJson(doc) > (size_t)RIFTLINK_BLE_ATT_SAFE_JSON_BYTES) {
    if (!doc["sf"].isNull()) {
      doc.remove("sf");
      continue;
    }
    if (!doc["region"].isNull()) {
      doc.remove("region");
      continue;
    }
    break;
  }
}

/** Как ESP `appendNodeSysMetricsToDoc` — только то, что умещается в 512 B (nRF без flash/NVS в JSON). */
static void appendNrfSysMetricsToDoc(JsonDocument& doc) {
  const uint32_t heap = xPortGetFreeHeapSize();
  if (heap > 0) doc["heapFree"] = heap;
}

/**
 * evt:neighbors + паспорт не должны превышать kTxJsonMax; укорачиваем хвост массивов (как routesTruncated).
 */
static void shrinkNeighborEvtToFit(JsonDocument& doc, JsonArray& arr, JsonArray& rssiArr, JsonArray& keyArr,
    JsonArray* batMvOpt) {
  while (measureJson(doc) >= kTxJsonMax && arr.size() > 0) {
    const size_t last = arr.size() - 1;
    arr.remove(last);
    if (rssiArr.size() > last) rssiArr.remove(last);
    if (keyArr.size() > last) keyArr.remove(last);
    if (batMvOpt && batMvOpt->size() > last) batMvOpt->remove(last);
    doc["neighborsTruncated"] = true;
  }
}

static const char* groupRoleToStr(groups::GroupRole role);

/** Первый кадр волны info: как evt:node на ESP (паспорт + seq/cmdId). */
static void emitNodeSnapshotForWave(uint32_t seq, uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "node";
  doc["seq"] = seq;
  if (cmdId != 0) doc["cmdId"] = cmdId;
  doc["platform"] = "nrf52840";
  doc["version"] = RIFTLINK_VERSION;
  char idHex[17] = {0};
  const uint8_t* id = node::getId();
  for (int i = 0; i < 8; i++) snprintf(idHex + i * 2, 3, "%02X", id[i]);
  doc["nodeId"] = idHex;
  doc["id"] = idHex;
  doc["sf"] = radio::getSpreadingFactor();
  doc["bw"] = radio::getBandwidth();
  doc["cr"] = radio::getCodingRate();
  doc["modemPreset"] = (int)radio::getModemPreset();
  doc["blePin"] = s_passkey;
  doc["lang"] = (locale::getLang() == LANG_RU) ? "ru" : "en";
  /* Явно: соседи по эфиру (LoRa mesh), не «BLE-соседи». 0 — норма при одном узле / вне зоны / другом регионе. */
  doc["loraMeshPeers"] = neighbors::getCount();
  doc["radioReady"] = radio::isReady();
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  doc["board"] = "heltec_t114";
#endif
  appendNrfSysMetricsToDoc(doc);
  appendNrfPassportFields(doc);
  /* T114: раньше отключали trim «для полного паспорта» — это тянуло multi‑KiB chunked NDJSON, долгий pumpBleTx и
   * гонку со SoftDevice (узел «виснет», телефон рвёт GATT). Тот же trim, что на остальных nRF — паритет с ESP по полям. */
  trimBlePassportDoc(doc);
  s_emittingFromInfoWaveNode = true;
  emitJsonDocStackOrChunked(doc);
  s_emittingFromInfoWaveNode = false;
}

static void notifyNeighborsWithSeq(uint32_t seq, uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "neighbors";
  doc["seq"] = seq;
  if (cmdId != 0) doc["cmdId"] = cmdId;
  JsonArray arr = doc["neighbors"].to<JsonArray>();
  JsonArray rssiArr = doc["rssi"].to<JsonArray>();
  JsonArray keyArr = doc["hasKey"].to<JsonArray>();
  JsonArray batMvArr = doc["batMv"].to<JsonArray>();
  int n = neighbors::getCount();
  char hex[17];
  uint8_t peerId[protocol::NODE_ID_LEN];
  for (int i = 0; i < n; i++) {
    neighbors::getIdHex(i, hex);
    arr.add(hex);
    const int r = neighbors::getRssi(i);
    rssiArr.add(r != 0 ? r : 0);
    if (neighbors::getId(i, peerId)) {
      keyArr.add(x25519_keys::hasKeyFor(peerId));
      int mv = neighbors::getBatteryMv(peerId);
      batMvArr.add(mv > 0 ? mv : 0);
    } else {
      keyArr.add(false);
      batMvArr.add(0);
    }
  }
  /* Паритет с ESP notifyNeighborsWithSeq: тот же JSON содержит паспорт локального узла + heap (см. API §3.1). */
  char idHex[17] = {0};
  const uint8_t* myId = node::getId();
  for (int i = 0; i < 8; i++) snprintf(idHex + i * 2, 3, "%02X", myId[i]);
  doc["id"] = idHex;
  doc["version"] = RIFTLINK_VERSION;
  doc["sf"] = radio::getSpreadingFactor();
  doc["bw"] = radio::getBandwidth();
  doc["cr"] = radio::getCodingRate();
  doc["modemPreset"] = (int)radio::getModemPreset();
  doc["blePin"] = s_passkey;
  appendNrfSysMetricsToDoc(doc);
  appendNrfPassportFields(doc);
  trimBlePassportDoc(doc);
  shrinkNeighborEvtToFit(doc, arr, rssiArr, keyArr, &batMvArr);
  /* Полная строка через emitJsonDocStackOrChunked (как evt:node); без shrinkJsonDocToAttSafe — иначе соседи «пропадают». */
  emitJsonDocStackOrChunked(doc);
}

static void notifyRoutesWithSeq(uint32_t seq, uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "routes";
  doc["seq"] = seq;
  if (cmdId != 0) doc["cmdId"] = cmdId;
  JsonArray arr = doc["routes"].to<JsonArray>();
  const int nAll = routing::getRouteCount();
  uint8_t dest[8], nextHop[8];
  uint8_t hops;
  int8_t rssi;
  int trustScore;
  char d[17], nh[17];
  for (int i = 0; i < nAll; i++) {
    if (!routing::getRouteAt(i, dest, nextHop, &hops, &rssi, &trustScore)) continue;
    JsonObject ro = arr.add<JsonObject>();
    for (int j = 0; j < 8; j++) {
      snprintf(d + j * 2, 3, "%02X", dest[j]);
      snprintf(nh + j * 2, 3, "%02X", nextHop[j]);
    }
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
    if (measureJson(doc) >= kTxJsonMax - 4) {
      arr.remove(arr.size() - 1);
      doc["routesTruncated"] = true;
      break;
    }
  }
  emitJsonDocStackOrChunked(doc);
}

static void notifyGroupsWithSeq(uint32_t seq, uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "groups";
  doc["seq"] = seq;
  if (cmdId != 0) doc["cmdId"] = cmdId;
  JsonArray arr = doc["groups"].to<JsonArray>();
  const int nv2 = groups::getV2Count();
  char uid[groups::GROUP_UID_MAX_LEN + 1];
  char tag[groups::GROUP_TAG_MAX_LEN + 1];
  char canonicalName[groups::GROUP_CANONICAL_NAME_MAX_LEN + 1];
  for (int i = 0; i < nv2; i++) {
    uint32_t channelId32 = 0;
    uint16_t keyVersion = 0;
    groups::GroupRole role = groups::GroupRole::None;
    uint32_t revEpoch = 0;
    bool ackApplied = false;
    if (!groups::getV2At(i, uid, sizeof(uid), &channelId32, tag, sizeof(tag), canonicalName, sizeof(canonicalName),
            &keyVersion, &role, &revEpoch, &ackApplied))
      continue;
    sanitizeBleJsonCStringInPlace(uid);
    sanitizeBleJsonCStringInPlace(tag);
    sanitizeBleJsonCStringInPlace(canonicalName);
    JsonObject gv2 = arr.add<JsonObject>();
    gv2["groupUid"] = uid;
    gv2["groupTag"] = tag;
    gv2["canonicalName"] = canonicalName;
    gv2["channelId32"] = channelId32;
    gv2["keyVersion"] = keyVersion;
    gv2["myRole"] = groupRoleToStr(role);
    gv2["revocationEpoch"] = revEpoch;
    gv2["ackApplied"] = ackApplied;
    if (measureJson(doc) >= kTxJsonMax) {
      arr.remove(arr.size() - 1);
      if (arr.size() == 0) {
        ble::notifyError("groups_payload_too_large", "groups payload exceeds BLE notify buffer");
        return;
      }
      doc["groupsTruncated"] = true;
      break;
    }
  }
  emitJsonDocStackOrChunked(doc);
}

/** Запрос волны info (node→neighbors→routes→groups); выполнение по одному кадру на вызов `ble::update()`. */
static void emitInfoWaveRequest(uint32_t cmdId) {
  if (s_infoWavePhase != InfoWavePhase::Idle || s_infoWavePending) {
    RIFTLINK_DIAG("BLE", "event=INFO_WAVE_SKIP reason=busy");
    return;
  }
  s_infoWavePending = true;
  s_infoWavePendingCmdId = cmdId;
}

static void processInfoWaveStep() {
  /* Линк уже оборван (в т.ч. в середине этого update) — не крутить фазу и не трогать notify до следующего sync. */
  if (!bleLinkUpForNus()) {
    /* T114: если линк мёртв, но chunked heap остался — это баг, сбрасываем принудительно */
    if (s_infoWaveNodeChunkHeap) {
      RIFTLINK_DIAG("BLE", "event=INFO_WAVE_FORCE_RESET reason=link_down_heap_stuck");
      s_infoWaveNodeChunkHeap = nullptr;
      s_infoWavePhase = InfoWavePhase::Idle;
      s_infoWavePending = false;
    }
    return;
  }

  if (s_infoWavePending && s_infoWavePhase == InfoWavePhase::Idle) {
    s_infoWavePending = false;
    s_infoWaveSeq = ++s_bleSyncSeq;
    s_infoWaveCmdId = s_infoWavePendingCmdId;
    s_infoWavePhase = InfoWavePhase::EmitNode;
    s_infoWaveNodeEmitted = false;
    s_infoWaveNeighborsEmitted = false;
    s_infoWaveRoutesEmitted = false;
    s_infoWaveGroupsEmitted = false;
    return;
  }
  if (s_infoWavePhase == InfoWavePhase::Idle) return;

  switch (s_infoWavePhase) {
    case InfoWavePhase::EmitNode:
      if (!s_infoWaveNodeEmitted) {
        emitNodeSnapshotForWave(s_infoWaveSeq, s_infoWaveCmdId);
        s_infoWaveNodeEmitted = true;
      }
      /* Длинный evt:node — ждём, пока chunked NDJSON не уйдёт из FIFO целиком. */
      if (s_infoWaveNodeChunkHeap) {
        break;
      }
      s_infoWaveNodeEmitted = false;
      s_infoWavePhase = InfoWavePhase::EmitNeighbors;
      break;
    case InfoWavePhase::EmitNeighbors:
      if (s_infoWaveNodeChunkHeap) break;
      if (!s_infoWaveNeighborsEmitted) {
        notifyNeighborsWithSeq(s_infoWaveSeq, s_infoWaveCmdId);
        s_infoWaveNeighborsEmitted = true;
      }
      if (s_infoWaveNodeChunkHeap) break;
      s_infoWaveNeighborsEmitted = false;
      s_infoWavePhase = InfoWavePhase::EmitRoutes;
      break;
    case InfoWavePhase::EmitRoutes:
      if (s_infoWaveNodeChunkHeap) break;
      if (!s_infoWaveRoutesEmitted) {
        notifyRoutesWithSeq(s_infoWaveSeq, s_infoWaveCmdId);
        s_infoWaveRoutesEmitted = true;
      }
      if (s_infoWaveNodeChunkHeap) break;
      s_infoWaveRoutesEmitted = false;
      s_infoWavePhase = InfoWavePhase::EmitGroups;
      break;
    case InfoWavePhase::EmitGroups:
      if (s_infoWaveNodeChunkHeap) break;
      if (!s_infoWaveGroupsEmitted) {
        notifyGroupsWithSeq(s_infoWaveSeq, s_infoWaveCmdId);
        s_infoWaveGroupsEmitted = true;
      }
      if (s_infoWaveNodeChunkHeap) break;
      s_infoWaveGroupsEmitted = false;
      s_infoWavePhase = InfoWavePhase::Idle;
      break;
    default:
      s_infoWavePhase = InfoWavePhase::Idle;
      break;
  }
}

/** Волна cmd:info (node→…→groups) или chunked evt:node в FIFO — нельзя вставлять отдельный evt:neighbors из mesh (flushPendingBleOutbound). */
static bool infoWavePipelineBusy() {
  return s_infoWavePending || s_infoWavePhase != InfoWavePhase::Idle || s_infoWaveNodeChunkHeap != nullptr;
}

static const char* groupRoleToStr(groups::GroupRole role) {
  switch (role) {
    case groups::GroupRole::Owner: return "owner";
    case groups::GroupRole::Admin: return "admin";
    case groups::GroupRole::Member: return "member";
    default: return "none";
  }
}

static void loadOrCreatePasskey() {
  uint32_t pin = 0;
  if (riftlink_kv::getU32(KV_PIN, &pin) && pin >= 100000U && pin <= 999999U) {
    s_passkey = pin;
    return;
  }
  s_passkey = (uint32_t)(random(900000) + 100000);
  (void)riftlink_kv::setU32(KV_PIN, s_passkey);
}

static bool enqueueJsonLine(const char* json) {
  if (!json) return false;
  size_t L = strlen(json);
  if (L == 0 || L >= kTxJsonMax) return false;
  if (bleSerializedJsonHasRawControlChars(json, L)) {
    RIFTLINK_DIAG("BLE", "event=TX_FAIL reason=raw_ctrl_in_serialized line len=%u", (unsigned)L);
    return false;
  }
  if (s_outCount >= kOutQDepth) {
    bleOutQueueDropOldestSlot();
  }
  BleOutSlot& slot = s_outQ[s_outHead];
  slot.kind = BleOutSlot::K_LINE;
  memcpy(slot.line, json, L + 1);
  s_outHead = (uint8_t)((s_outHead + 1) % kOutQDepth);
  s_outCount++;
  return true;
}

/** За один вызов `ble::update()` не слать слишком много notify подряд — иначе централь рвёт линк / SoftDevice захлёбывается. */
static constexpr unsigned kBleOutLinesPerUpdateFlush = 4;

/** Сколько байт payload длинной NDJSON-строки отдать за один вызов pump (несколько вызовов за один ble::update()). */
static constexpr size_t kBleNdjsonTxBudgetBytes = 1024;

/** Минимальный размер кучи для chunked NDJSON: measureJson(n) на сложных doc иногда заметно меньше фактического serialize. */
static constexpr size_t kChunkedJsonHeapMinBytes = 4096;

/**
 * Сериализация JsonDocument в кучу для одной NDJSON-строки (chunked по эфиру). Несколько попыток с ростом cap.
 * Иначе на телефон уходит обрезанный JSON + \\n → FormatException.
 */
static bool serializeDocToHeapForChunkedLine(JsonDocument& doc, char** heapOut, size_t* lenOut) {
  const size_t n = measureJson(doc);
  if (n == 0) return false;
  size_t cap = n + 1 + 1024;
  if (cap < kChunkedJsonHeapMinBytes) cap = kChunkedJsonHeapMinBytes;
  for (unsigned attempt = 0; attempt < 5u; attempt++) {
    char* heap = (char*)malloc(cap);
    if (!heap) return false;
    const size_t w = serializeJson(doc, heap, cap);
    const bool truncated = (w + 1 >= cap);
    const bool looksClosed = (w >= 2 && heap[w - 1] == '}');
    if (w > 0 && !truncated && looksClosed) {
      heap[w] = '\0';
      *heapOut = heap;
      *lenOut = w;
      return true;
    }
    free(heap);
    if (w == 0) return false;
    {
      const size_t doubled = cap * 2u;
      cap = (doubled > cap) ? doubled : (cap + 8192u);
    }
    if (cap < n + 2048u) cap = n + 2048u;
    if (cap > 65536u) return false;
  }
  return false;
}

/**
 * Единый drain FIFO: хвост очереди — короткая строка или chunked NDJSON; порядок байт в одной «трубе» не нарушается.
 * maxLineLines — лимит коротких строк за вызов; maxChunkBytes — бюджет payload chunked (чанки по 32 B).
 */
static void pumpBleTx(size_t maxChunkBytes, unsigned maxLineLines) {
  if (!s_connected || Bluefruit.connected() == 0) {
    resetBleOutQueueOnDisconnect();
    return;
  }
  if (!nusNotifyEnabled()) return;

  size_t chunkBudget = maxChunkBytes;
  unsigned lineBurst = 0;
  static const uint8_t kNlByte = '\n';

  while (s_outCount > 0) {
    /* Обрыв во время pump: иначе notify может зависнуть в SoftDevice при мёртвом линке. */
    if (!s_connected || Bluefruit.connected() == 0) {
      resetBleOutQueueOnDisconnect();
      return;
    }
    BleOutSlot& slot = s_outQ[s_outTail];
    if (slot.kind == BleOutSlot::K_LINE) {
      if (lineBurst >= maxLineLines) return;
      const char* line = slot.line;
      size_t L = strlen(line);
      if (L > 0) {
        if (L > 65535u) L = 65535u;
        // Retry для надёжной отправки коротких JSON-строк
        if (!nusNotifyBytesWithRetry((const uint8_t*)line, (uint16_t)L)) {
          if (Bluefruit.connected() == 0) resetBleOutQueueOnDisconnect();
          return;
        }
        if (!nusNotifyBytesWithRetry(&kNlByte, 1)) {
          if (Bluefruit.connected() == 0) resetBleOutQueueOnDisconnect();
          return;
        }
      }
      s_outTail = (uint8_t)((s_outTail + 1) % kOutQDepth);
      s_outCount--;
      lineBurst++;
      if (s_outCount > 0 && lineBurst < maxLineLines) {
        BleOutSlot& next = s_outQ[s_outTail];
        if (next.kind == BleOutSlot::K_LINE) {
          yield();
          delay(1);
        }
      }
      continue;
    }

    constexpr size_t kChunk = 32;
    unsigned chunkInPump = 0;
    while (slot.ch.off < slot.ch.len && chunkBudget > 0) {
      if (!s_connected || Bluefruit.connected() == 0) {
        resetBleOutQueueOnDisconnect();
        return;
      }
      size_t remain = slot.ch.len - slot.ch.off;
      size_t n = remain > kChunk ? kChunk : remain;
      if (n > chunkBudget) n = chunkBudget;
      // Retry для каждого чанка длинного NDJSON
      if (!nusNotifyBytesWithRetry((const uint8_t*)(slot.ch.heap + slot.ch.off), (uint16_t)n)) {
        if (Bluefruit.connected() == 0) resetBleOutQueueOnDisconnect();
        return;
      }
      slot.ch.off += n;
      chunkBudget -= n;
      yield();
      if ((++chunkInPump & 7u) == 0) riftlink_wdt_feed();
    }
    if (slot.ch.off >= slot.ch.len) {
      if (!nusNotifyBytesWithRetry(&kNlByte, 1)) {
        if (Bluefruit.connected() == 0) resetBleOutQueueOnDisconnect();
        return;
      }
      if (slot.ch.heap == s_infoWaveNodeChunkHeap) s_infoWaveNodeChunkHeap = nullptr;
      free(slot.ch.heap);
      s_outTail = (uint8_t)((s_outTail + 1) % kOutQDepth);
      s_outCount--;
      continue;
    }
    return;
  }
}

static bool scheduleNdjsonLineOwned(char* heap, size_t len) {
  if (!heap || len == 0) {
    if (heap) free(heap);
    return false;
  }
  if (bleSerializedJsonHasRawControlChars(heap, len)) {
    RIFTLINK_DIAG("BLE", "event=TX_FAIL reason=raw_ctrl_in_serialized owned len=%u", (unsigned)len);
    free(heap);
    return false;
  }
  if (!s_connected || Bluefruit.connected() == 0) {
    free(heap);
    return false;
  }
  if (!nusNotifyEnabled()) {
    free(heap);
    return false;
  }
  if (s_outCount >= kOutQDepth) {
    bleOutQueueDropOldestSlot();
  }
  BleOutSlot& slot = s_outQ[s_outHead];
  slot.kind = BleOutSlot::K_CHUNKED;
  slot.ch.heap = heap;
  slot.ch.len = len;
  slot.ch.off = 0;
  if (s_emittingFromInfoWaveNode) {
    s_infoWaveNodeChunkHeap = heap;
  }
  s_outHead = (uint8_t)((s_outHead + 1) % kOutQDepth);
  s_outCount++;
  return true;
}

static void emitJsonDoc(JsonDocument& doc) {
  char buf[kTxJsonMax];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) {
    RIFTLINK_DIAG("BLE", "event=TX_FAIL reason=serialize");
    return;
  }
  buf[n] = '\0';
  if (n < 2 || buf[n - 1] != '}') {
    RIFTLINK_DIAG("BLE", "event=TX_FAIL reason=serialize_not_closed n=%u", (unsigned)n);
    return;
  }
  (void)enqueueJsonLine(buf);
}

/**
 * Одна строка NDJSON: если сериализация ≥ kTxJsonMax — в кучу и в общую FIFO-очередь TX; pumpBleTx по тикам.
 * (несколько ATT notify до завершающего \\n; клиент склеивает — см. docs/API.md §476).
 */
static void emitJsonDocStackOrChunked(JsonDocument& doc) {
  const size_t n = measureJson(doc);
  if (n == 0) return;
  if (n < kTxJsonMax) {
    emitJsonDoc(doc);
    return;
  }
  /* Без CCCD notify не уйдёт; не держим pending — ужимаем и шлём ≤511 B, иначе волна info зависает навсегда. */
  if (!nusNotifyEnabled()) {
    trimBlePassportDoc(doc);
    shrinkJsonDocToAttSafe(doc);
    emitJsonDoc(doc);
    return;
  }
  char* heap = nullptr;
  size_t w = 0;
  if (!serializeDocToHeapForChunkedLine(doc, &heap, &w)) {
    RIFTLINK_DIAG("BLE", "event=TX_FAIL reason=chunked_serialize n=%u", (unsigned)n);
    trimBlePassportDoc(doc);
    shrinkJsonDocToAttSafe(doc);
    emitJsonDoc(doc);
    return;
  }
  if (!scheduleNdjsonLineOwned(heap, w)) {
    trimBlePassportDoc(doc);
    shrinkJsonDocToAttSafe(doc);
    emitJsonDoc(doc);
  }
}

/** Паритет с ESP `notifyJsonToApp` для oversize: поля `len` и `limit`. */
static void notifyPayloadTooLong(uint16_t droppedLen) {
  if (!s_connected || Bluefruit.connected() == 0) return;
  JsonDocument doc;
  doc["evt"] = "error";
  doc["code"] = "payload_too_long";
  doc["msg"] = "JSON exceeds 512 bytes";
  doc["len"] = droppedLen;
  doc["limit"] = (uint32_t)kRxLineMax;
  emitJsonDoc(doc);
}

/** Одна ATT WRITE на NUS RXD — паритет с ESP: writeEvent → очередь, разбор в ble::update(). */
static void onRiftNusRxdWrite(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  (void)conn_hdl;
  (void)chr;
  if (Bluefruit.connected() == 0) return;
  if (len == 0 || data == nullptr) return;
  
  // Диагностика переполнения
  if (len > (uint16_t)kRxLineMax) {
    notifyPayloadTooLong(len);
    RIFTLINK_DIAG("BLE", "event=RX_DROP reason=payload_too_long len=%u max=%u", 
        (unsigned)len, (unsigned)kRxLineMax);
    return;
  }
  
  // Обрезка CR/LF с конца
  uint16_t trim = len;
  while (trim > 0 && (data[trim - 1] == '\n' || data[trim - 1] == '\r')) trim--;
  if (trim == 0) return;
  
  // Попытка постановки в очередь
  if (!enqueueIncomingCmd(data, trim)) {
    s_inCmdDroppedQueueFull++;
    RIFTLINK_DIAG("BLE", "event=RX_CMD_DROP reason=in_queue_full count=%u", 
        (unsigned)s_inCmdDroppedQueueFull);
    
    // Backpressure: уведомление телефона о перегрузке
    JsonDocument doc;
    doc["evt"] = "error";
    doc["code"] = "ble_rx_queue_full";
    doc["msg"] = "Node RX queue overflow, reduce send rate";
    doc["dropped"] = (unsigned)s_inCmdDroppedQueueFull;
    char buf[256];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
      buf[n] = '\0';
      nusNotifyBytesWithRetry((const uint8_t*)buf, (uint16_t)n);
    }
  }
}

static void emitLoraScanDoc(uint32_t cmdId, int n, const selftest::ScanResult* res) {
  if (!res) n = 0;
  if (n > 8) n = 8;
  JsonDocument doc;
  doc["evt"] = "loraScan";
  doc["count"] = n;
  doc["quick"] = true;
  JsonArray arr = doc["results"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["sf"] = res[i].sf;
    o["bw"] = res[i].bw;
    o["rssi"] = res[i].rssi;
  }
  if (cmdId != 0) doc["cmdId"] = cmdId;
  if (measureJson(doc) >= kTxJsonMax) {
    ble::notifyError("lora_scan_serialize", "loraScan JSON exceeds BLE limit");
    return;
  }
  emitJsonDoc(doc);
}

static groups::GroupRole parseGroupRoleStr(const char* role) {
  if (!role || !role[0]) return groups::GroupRole::None;
  if (strcmp(role, "owner") == 0) return groups::GroupRole::Owner;
  if (strcmp(role, "admin") == 0) return groups::GroupRole::Admin;
  if (strcmp(role, "member") == 0) return groups::GroupRole::Member;
  return groups::GroupRole::None;
}

static void nodeIdToHexBuf(const uint8_t in[protocol::NODE_ID_LEN], char out[17]) {
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) snprintf(out + i * 2, 3, "%02X", in[i]);
  out[16] = '\0';
}

static bool isSelfNodeHex(const char* hexId) {
  uint8_t id[protocol::NODE_ID_LEN];
  if (!parseFullNodeIdHex(hexId, id)) return false;
  return memcmp(id, node::getId(), protocol::NODE_ID_LEN) == 0;
}

static uint32_t parseJsonChannelId32(JsonVariant v) {
  if (v.isNull()) return 0;
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!s || !s[0]) return 0;
    char* end = nullptr;
    unsigned long ul = strtoul(s, &end, 10);
    if (end == s) return 0;
    if (ul > 4294967295UL) return 0;
    return static_cast<uint32_t>(ul);
  }
  if (v.is<int>()) {
    int i = v.as<int>();
    if (i < 2) return 0;
    return static_cast<uint32_t>(i);
  }
  if (v.is<long>()) {
    long i = v.as<long>();
    if (i < 2L || i > 4294967295L) return 0;
    return static_cast<uint32_t>(i);
  }
  if (v.is<long long>()) {
    long long i = v.as<long long>();
    if (i < 2LL || i > 4294967295LL) return 0;
    return static_cast<uint32_t>(i);
  }
  const double d = v.as<double>();
  if (d < 2.0 || d > 4294967295.0) return 0;
  return static_cast<uint32_t>(static_cast<uint64_t>(d + 0.5));
}

/// cmdId из JSON: как ble.cpp — большие целые могут лежать в double; `doc["cmdId"]|0` даёт 0.
static uint32_t parseCmdIdFromDoc(JsonDocument& doc) {
  JsonVariant v = doc["cmdId"];
  if (v.isNull()) return 0;
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!s || !s[0]) return 0;
    char* end = nullptr;
    unsigned long ul = strtoul(s, &end, 10);
    if (end == s) return 0;
    if (ul > 4294967295UL) return 0;
    return static_cast<uint32_t>(ul);
  }
  if (v.is<int>()) {
    int i = v.as<int>();
    if (i < 1) return 0;
    return static_cast<uint32_t>(i);
  }
  if (v.is<long>()) {
    long i = v.as<long>();
    if (i < 1L || i > 4294967295L) return 0;
    return static_cast<uint32_t>(i);
  }
  if (v.is<long long>()) {
    long long i = v.as<long long>();
    if (i < 1LL || i > 4294967295LL) return 0;
    return static_cast<uint32_t>(i);
  }
  const double d = v.as<double>();
  if (d < 1.0 || d > 4294967295.0) return 0;
  return static_cast<uint32_t>(static_cast<uint64_t>(d + 0.5));
}

/** Команды без tracked-ответа в приложении — допускают отсутствие cmdId (как ble.cpp). */
static bool bleJsonCmdAllowsMissingCmdId(const char* cmd) {
  if (!cmd) return false;
  if (strcmp(cmd, "send") == 0) return true;
  if (strcmp(cmd, "sos") == 0) return true;
  if (strcmp(cmd, "location") == 0) return true;
  if (strcmp(cmd, "radioMode") == 0) return true;
  if (strcmp(cmd, "lang") == 0) return true;
  if (strcmp(cmd, "wifi") == 0) return true;
  if (strcmp(cmd, "read") == 0) return true;
  if (strcmp(cmd, "voice") == 0) return true;
  if (strcmp(cmd, "signalTest") == 0) return true;
  if (strcmp(cmd, "loraScan") == 0) return true;
  if (strcmp(cmd, "shutdown") == 0 || strcmp(cmd, "poweroff") == 0) return true;
  if (strncmp(cmd, "bleOta", 6) == 0) return true;
  if (strcmp(cmd, "nickname") == 0) return true;
  return false;
}

static bool b64EncodeBytes(const uint8_t* data, size_t len, char* out, size_t outCap) {
  const size_t need = sodium_base64_encoded_len(len, sodium_base64_VARIANT_ORIGINAL);
  if (outCap < need + 1) return false;
  sodium_bin2base64(out, outCap, data, len, sodium_base64_VARIANT_ORIGINAL);
  return true;
}

static bool b64DecodeTo(const char* b64, uint8_t* out, size_t outMax, size_t* outLen) {
  if (!b64 || !b64[0] || !outLen) return false;
  return sodium_base642bin(out, outMax, b64, strlen(b64), nullptr, outLen, nullptr, sodium_base64_VARIANT_ORIGINAL) == 0;
}

static void notifyGroupSecurityErrorV2(const char* groupUid, const char* code, const char* msg, uint32_t cmdId) {
  JsonDocument ev;
  ev["evt"] = "groupSecurityError";
  if (groupUid && groupUid[0]) {
    char uidBuf[groups::GROUP_UID_MAX_LEN + 1];
    strncpy(uidBuf, groupUid, sizeof(uidBuf) - 1);
    uidBuf[sizeof(uidBuf) - 1] = '\0';
    sanitizeBleJsonCStringInPlace(uidBuf);
    ev["groupUid"] = uidBuf;
  }
  char codeBuf[96];
  char msgBuf[256];
  strncpy(codeBuf, code ? code : "group_v2_unknown", sizeof(codeBuf) - 1);
  codeBuf[sizeof(codeBuf) - 1] = '\0';
  strncpy(msgBuf, msg ? msg : "", sizeof(msgBuf) - 1);
  msgBuf[sizeof(msgBuf) - 1] = '\0';
  sanitizeBleJsonCStringInPlace(codeBuf);
  sanitizeBleJsonCStringInPlace(msgBuf);
  ev["code"] = codeBuf;
  ev["msg"] = msgBuf;
  if (cmdId != 0) ev["cmdId"] = cmdId;
  emitJsonDoc(ev);
}

static void notifyGroupStatusV2(const char* groupUid, bool inviteAcceptNoop, uint32_t cmdId) {
  if (!groupUid || !groupUid[0]) return;
  uint32_t channelId32 = 0;
  char groupTag[groups::GROUP_TAG_MAX_LEN + 1] = {0};
  char canonicalName[groups::GROUP_CANONICAL_NAME_MAX_LEN + 1] = {0};
  uint16_t keyVersion = 0;
  groups::GroupRole role = groups::GroupRole::None;
  uint32_t revocationEpoch = 0;
  bool ackApplied = false;
  if (!groups::getGroupV2(groupUid, &channelId32, groupTag, sizeof(groupTag), canonicalName, sizeof(canonicalName),
          &keyVersion, &role, &revocationEpoch, &ackApplied))
    return;
  sanitizeBleJsonCStringInPlace(groupTag);
  sanitizeBleJsonCStringInPlace(canonicalName);
  char uidBuf[groups::GROUP_UID_MAX_LEN + 1];
  strncpy(uidBuf, groupUid, sizeof(uidBuf) - 1);
  uidBuf[sizeof(uidBuf) - 1] = '\0';
  sanitizeBleJsonCStringInPlace(uidBuf);
  JsonDocument ev;
  ev["evt"] = "groupStatus";
  ev["groupUid"] = uidBuf;
  ev["channelId32"] = channelId32;
  ev["groupTag"] = groupTag;
  ev["canonicalName"] = canonicalName;
  ev["myRole"] = groupRoleToStr(role);
  ev["keyVersion"] = keyVersion;
  ev["revocationEpoch"] = revocationEpoch;
  ev["ackApplied"] = ackApplied;
  ev["status"] = "ok";
  ev["rekeyRequired"] = !ackApplied;
  if (inviteAcceptNoop) ev["inviteNoop"] = true;
  if (cmdId != 0) ev["cmdId"] = cmdId;
  emitJsonDoc(ev);
}

static void notifyGroupRekeyProgressV2(const char* groupUid, const char* rekeyOpId, uint16_t keyVersion, uint32_t cmdId) {
  if (!groupUid || !groupUid[0]) return;
  JsonDocument ev;
  ev["evt"] = "groupRekeyProgress";
  ev["groupUid"] = groupUid;
  ev["rekeyOpId"] = (rekeyOpId && rekeyOpId[0]) ? rekeyOpId : "local";
  ev["keyVersion"] = keyVersion;
  ev["pending"] = 0;
  ev["delivered"] = 0;
  ev["applied"] = 1;
  ev["failed"] = 0;
  if (cmdId != 0) ev["cmdId"] = cmdId;
  emitJsonDoc(ev);
}

static void notifyGroupMemberKeyStateV2(
    const char* groupUid, const char* memberId, const char* state, uint32_t ackAt, uint32_t cmdId) {
  if (!groupUid || !groupUid[0] || !memberId || !memberId[0] || !state || !state[0]) return;
  JsonDocument ev;
  ev["evt"] = "groupMemberKeyState";
  ev["groupUid"] = groupUid;
  ev["memberId"] = memberId;
  ev["status"] = state;
  if (ackAt > 0) ev["ackAt"] = ackAt;
  if (cmdId != 0) ev["cmdId"] = cmdId;
  emitJsonDoc(ev);
}

static bool handleGroupV2Commands(JsonDocument& doc, const char* cmd, uint32_t cmdId) {
  if (strcmp(cmd, "groupCreate") == 0) {
    if (!loadOrGenerateGroupOwnerSigningKey()) {
      notifyGroupSecurityErrorV2(nullptr, "group_v3_bad", "sodium/sign init failed", cmdId);
      return true;
    }
    const char* groupUid = doc["groupUid"];
    const char* groupTag = doc["groupTag"];
    const char* canonicalName = doc["displayName"];
    uint32_t channelId32 = parseJsonChannelId32(doc["channelId32"]);
    const char* keyB64 = doc["groupKey"];
    uint16_t keyVersion = (uint16_t)(doc["keyVersion"] | 1);
    const char* roleStr = doc["myRole"];
    uint32_t revEpoch = doc["revocationEpoch"] | 0;
    if (!groupUid || !groupUid[0] || !groupTag || !groupTag[0] || !canonicalName || !canonicalName[0] ||
        channelId32 <= groups::GROUP_ALL) {
      notifyGroupSecurityErrorV2(groupUid, "group_v3_bad", "Missing groupUid/groupTag/canonicalName/channelId32", cmdId);
      return true;
    }
    if (strchr(canonicalName, '|') != nullptr) {
      notifyGroupSecurityErrorV2(groupUid, "group_v3_bad", "canonicalName contains invalid separator", cmdId);
      return true;
    }
    uint8_t key[32];
    if (keyB64 && keyB64[0]) {
      size_t decLen = 0;
      if (!b64DecodeTo(keyB64, key, sizeof(key), &decLen) || decLen != 32) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_key_bad", "Bad groupKey", cmdId);
        return true;
      }
    } else {
      randombytes_buf(key, sizeof(key));
    }
    groups::GroupRole role = parseGroupRoleStr(roleStr);
    if (role == groups::GroupRole::None) role = groups::GroupRole::Owner;
    if (!groups::upsertGroupV2(groupUid, channelId32, groupTag, canonicalName, key, keyVersion, role, revEpoch)) {
      notifyGroupSecurityErrorV2(groupUid, "group_v3_store_failed", "Failed to store V3 group", cmdId);
      return true;
    }
    if (role == groups::GroupRole::Owner) {
      if (!groups::setOwnerSignPubKeyV2(groupUid, s_groupOwnerSignPk)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v3_store_failed", "Failed to set owner signing key", cmdId);
        return true;
      }
    }
    {
      const uint16_t appliedKv = keyVersion > 0 ? keyVersion : 1;
      (void)groups::ackKeyAppliedV2(groupUid, appliedKv);
    }
    notifyGroupStatusV2(groupUid, false, cmdId);
    emitInfoWaveRequest(cmdId);
    return true;
  }
  if (strcmp(cmd, "groupStatus") == 0) {
    const char* groupUid = doc["groupUid"];
    if (!groupUid || !groupUid[0]) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_bad", "Missing groupUid", cmdId);
      return true;
    }
    notifyGroupStatusV2(groupUid, false, cmdId);
    return true;
  }
  if (strcmp(cmd, "groupCanonicalRename") == 0) {
    const char* groupUid = doc["groupUid"];
    const char* newName = doc["canonicalName"];
    if (!groupUid || !groupUid[0] || !newName || !newName[0]) {
      notifyGroupSecurityErrorV2(groupUid, "group_v3_name_bad", "Missing groupUid/canonicalName", cmdId);
      return true;
    }
    groups::GroupRole role = groups::GroupRole::None;
    if (!groups::getGroupV2(groupUid, nullptr, nullptr, 0, nullptr, 0, nullptr, &role, nullptr, nullptr)) {
      notifyGroupSecurityErrorV2(groupUid, "group_v3_name_bad", "Unknown group", cmdId);
      return true;
    }
    if (strchr(newName, '|') != nullptr) {
      notifyGroupSecurityErrorV2(groupUid, "group_v3_name_bad", "canonicalName contains invalid separator", cmdId);
      return true;
    }
    if (role != groups::GroupRole::Owner) {
      notifyGroupSecurityErrorV2(groupUid, "group_v3_name_denied", "Only owner can rename canonicalName", cmdId);
      return true;
    }
    if (!groups::setCanonicalNameV2(groupUid, newName)) {
      notifyGroupSecurityErrorV2(groupUid, "group_v3_name_bad", "Cannot set canonicalName", cmdId);
      return true;
    }
    notifyGroupStatusV2(groupUid, false, cmdId);
    emitInfoWaveRequest(cmdId);
    return true;
  }
  if (strcmp(cmd, "groupInviteCreate") == 0) {
    if (!loadOrGenerateGroupOwnerSigningKey()) {
      notifyGroupSecurityErrorV2(nullptr, "group_v31_invite_bad", "sodium/sign init failed", cmdId);
      return true;
    }
    const char* groupUid = doc["groupUid"];
    const char* roleStr = doc["role"];
    uint32_t ttlSec = doc["ttlSec"] | 600;
    if (!groupUid || !groupUid[0]) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Missing groupUid", cmdId);
      return true;
    }
    uint32_t channelId32 = 0;
    char gtag[groups::GROUP_TAG_MAX_LEN + 1] = {0};
    char cname[groups::GROUP_CANONICAL_NAME_MAX_LEN + 1] = {0};
    uint16_t keyVersion = 0;
    groups::GroupRole myRole = groups::GroupRole::None;
    uint8_t key[32];
    if (!groups::getGroupV2(groupUid, &channelId32, gtag, sizeof(gtag), cname, sizeof(cname), &keyVersion, &myRole,
            nullptr, nullptr) ||
        !groups::getGroupKeyV2(groupUid, key, &keyVersion)) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Unknown groupUid", cmdId);
      return true;
    }
    if (myRole != groups::GroupRole::Owner) {
      notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_denied", "Only owner can issue signed invite", cmdId);
      return true;
    }
    if (strchr(cname, '|') != nullptr) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "canonicalName contains invalid separator", cmdId);
      return true;
    }
    uint8_t ownerSignPubKey[groups::GROUP_OWNER_SIGN_PUBKEY_LEN] = {0};
    if (!groups::getOwnerSignPubKeyV2(groupUid, ownerSignPubKey)) {
      if (!groups::setOwnerSignPubKeyV2(groupUid, s_groupOwnerSignPk) ||
          !groups::getOwnerSignPubKeyV2(groupUid, ownerSignPubKey)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Owner signing key is not set", cmdId);
        return true;
      }
    }
    if (memcmp(ownerSignPubKey, s_groupOwnerSignPk, sizeof(ownerSignPubKey)) != 0) {
      notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_denied", "Owner signing key mismatch", cmdId);
      return true;
    }
    char keyB64[80] = {0};
    char ownerPubB64[96] = {0};
    if (!b64EncodeBytes(key, sizeof(key), keyB64, sizeof(keyB64)) ||
        !b64EncodeBytes(ownerSignPubKey, sizeof(ownerSignPubKey), ownerPubB64, sizeof(ownerPubB64))) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Key encode failed", cmdId);
      return true;
    }
    if (!roleStr || !roleStr[0]) roleStr = "member";
    const uint32_t expiresAt = (uint32_t)(millis() / 1000) + ttlSec;
    char raw[420] = {0};
    int rawLen = snprintf(raw, sizeof(raw), "v3.1|%s|%lu|%s|%s|%u|%s|%s|%lu|%s", groupUid,
        (unsigned long)channelId32, gtag, cname, (unsigned)keyVersion, keyB64, roleStr, (unsigned long)expiresAt,
        ownerPubB64);
    if (rawLen <= 0 || (size_t)rawLen >= sizeof(raw)) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Invite payload too large", cmdId);
      return true;
    }
    unsigned char sig[crypto_sign_BYTES] = {0};
    unsigned long long sigLen = 0;
    if (crypto_sign_detached(sig, &sigLen, (const unsigned char*)raw, (unsigned long long)rawLen, s_groupOwnerSignSk) !=
            0 ||
        sigLen != crypto_sign_BYTES) {
      notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Invite sign failed", cmdId);
      return true;
    }
    char sigB64[140] = {0};
    if (!b64EncodeBytes(sig, sizeof(sig), sigB64, sizeof(sigB64))) {
      notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Signature encode failed", cmdId);
      return true;
    }
    char inviteRaw[600] = {0};
    int inviteRawLen = snprintf(inviteRaw, sizeof(inviteRaw), "%s|%s", raw, sigB64);
    if (inviteRawLen <= 0 || (size_t)inviteRawLen >= sizeof(inviteRaw)) {
      notifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Invite payload too large", cmdId);
      return true;
    }
    char inviteB64[880] = {0};
    if (!b64EncodeBytes((const uint8_t*)inviteRaw, (size_t)inviteRawLen, inviteB64, sizeof(inviteB64))) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Invite encode failed", cmdId);
      return true;
    }
    char uidSan[groups::GROUP_UID_MAX_LEN + 1];
    strncpy(uidSan, groupUid, sizeof(uidSan) - 1);
    uidSan[sizeof(uidSan) - 1] = '\0';
    sanitizeBleJsonCStringInPlace(uidSan);
    char roleSan[32];
    strncpy(roleSan, roleStr, sizeof(roleSan) - 1);
    roleSan[sizeof(roleSan) - 1] = '\0';
    sanitizeBleJsonCStringInPlace(roleSan);
    sanitizeBleJsonCStringInPlace(gtag);
    sanitizeBleJsonCStringInPlace(cname);
    JsonDocument ev;
    ev["evt"] = "groupInvite";
    ev["groupUid"] = uidSan;
    ev["role"] = roleSan;
    ev["invite"] = inviteB64;
    ev["expiresAt"] = expiresAt;
    ev["channelId32"] = channelId32;
    ev["canonicalName"] = cname;
    if (cmdId != 0) ev["cmdId"] = cmdId;
    char out[2048];
    size_t outLen = serializeJson(ev, out, sizeof(out));
    if (outLen == 0 || outLen >= sizeof(out)) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Invite JSON serialize failed", cmdId);
      return true;
    }
    if (outLen < kTxJsonMax) {
      out[outLen] = '\0';
      (void)enqueueJsonLine(out);
    } else {
      char* heap = (char*)malloc(outLen + 1);
      if (!heap) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Invite TX failed", cmdId);
        return true;
      }
      memcpy(heap, out, outLen);
      heap[outLen] = '\0';
      if (!scheduleNdjsonLineOwned(heap, outLen)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Invite TX failed", cmdId);
        return true;
      }
    }
    return true;
  }
  if (strcmp(cmd, "groupInviteAccept") == 0) {
    const char* inviteB64 = doc["invite"];
    if (!inviteB64 || !inviteB64[0]) {
      notifyGroupSecurityErrorV2(nullptr, "group_v2_invite_bad", "Missing invite", cmdId);
      return true;
    }
    uint8_t rawBuf[700] = {0};
    size_t rawLen = 0;
    if (!b64DecodeTo(inviteB64, rawBuf, sizeof(rawBuf) - 1, &rawLen) || rawLen == 0) {
      notifyGroupSecurityErrorV2(nullptr, "group_v2_invite_bad", "Bad invite base64", cmdId);
      return true;
    }
    rawBuf[rawLen] = 0;
    char* savePtr = nullptr;
    char* version = strtok_r((char*)rawBuf, "|", &savePtr);
    char* gu = strtok_r(nullptr, "|", &savePtr);
    char* channelStr = strtok_r(nullptr, "|", &savePtr);
    char* groupTag = strtok_r(nullptr, "|", &savePtr);
    char* canonicalName = strtok_r(nullptr, "|", &savePtr);
    char* keyVersionStr = strtok_r(nullptr, "|", &savePtr);
    char* keyB64 = strtok_r(nullptr, "|", &savePtr);
    char* roleStr = strtok_r(nullptr, "|", &savePtr);
    char* expiresStr = strtok_r(nullptr, "|", &savePtr);
    char* ownerPubB64 = strtok_r(nullptr, "|", &savePtr);
    char* sigB64 = strtok_r(nullptr, "|", &savePtr);
    if (!version || strcmp(version, "v3.1") != 0 || !gu || !channelStr || !groupTag || !canonicalName ||
        !canonicalName[0] || !keyVersionStr || !keyB64 || !roleStr || !expiresStr || !ownerPubB64 ||
        !ownerPubB64[0] || !sigB64 || !sigB64[0]) {
      notifyGroupSecurityErrorV2(gu, "group_v2_invite_bad", "Malformed invite", cmdId);
      return true;
    }
    char signedRaw[420] = {0};
    int signedRawLen = snprintf(signedRaw, sizeof(signedRaw), "v3.1|%s|%s|%s|%s|%s|%s|%s|%s|%s", gu, channelStr,
        groupTag, canonicalName, keyVersionStr, keyB64, roleStr, expiresStr, ownerPubB64);
    if (signedRawLen <= 0 || (size_t)signedRawLen >= sizeof(signedRaw)) {
      notifyGroupSecurityErrorV2(gu, "group_v31_invite_bad", "Malformed signed payload", cmdId);
      return true;
    }
    uint8_t ownerSignPubKey[groups::GROUP_OWNER_SIGN_PUBKEY_LEN] = {0};
    size_t ownerSignPubKeyLen = 0;
    if (!b64DecodeTo(ownerPubB64, ownerSignPubKey, sizeof(ownerSignPubKey), &ownerSignPubKeyLen) ||
        ownerSignPubKeyLen != sizeof(ownerSignPubKey)) {
      notifyGroupSecurityErrorV2(gu, "group_v31_invite_bad", "Invalid owner signing key", cmdId);
      return true;
    }
    unsigned char sig[crypto_sign_BYTES] = {0};
    size_t sigLen = 0;
    if (!b64DecodeTo(sigB64, sig, sizeof(sig), &sigLen) || sigLen != crypto_sign_BYTES) {
      notifyGroupSecurityErrorV2(gu, "group_v31_invite_bad", "Invalid signature encoding", cmdId);
      return true;
    }
    if (crypto_sign_verify_detached(sig, (const unsigned char*)signedRaw, (unsigned long long)signedRawLen,
            ownerSignPubKey) != 0) {
      notifyGroupSecurityErrorV2(gu, "group_v31_invite_bad", "Owner signature verification failed", cmdId);
      return true;
    }
    const uint32_t nowSec = (uint32_t)(millis() / 1000);
    const uint32_t expiresAt = (uint32_t)strtoul(expiresStr, nullptr, 10);
    if (expiresAt == 0 || nowSec > expiresAt) {
      notifyGroupSecurityErrorV2(gu, "group_v2_invite_expired", "Invite expired", cmdId);
      return true;
    }
    const uint32_t channelId32 = (uint32_t)strtoul(channelStr, nullptr, 10);
    const uint16_t keyVersion = (uint16_t)strtoul(keyVersionStr, nullptr, 10);
    uint8_t gkey[32];
    size_t keyLen = 0;
    if (channelId32 <= groups::GROUP_ALL ||
        !b64DecodeTo(keyB64, gkey, sizeof(gkey), &keyLen) || keyLen != 32) {
      notifyGroupSecurityErrorV2(gu, "group_v2_invite_bad", "Invalid key/channel", cmdId);
      return true;
    }
    groups::GroupRole role = parseGroupRoleStr(roleStr);
    if (role == groups::GroupRole::None) role = groups::GroupRole::Member;
    uint8_t pinnedOwnerSignPubKey[groups::GROUP_OWNER_SIGN_PUBKEY_LEN] = {0};
    if (groups::getOwnerSignPubKeyV2(gu, pinnedOwnerSignPubKey) &&
        memcmp(pinnedOwnerSignPubKey, ownerSignPubKey, sizeof(ownerSignPubKey)) != 0) {
      notifyGroupSecurityErrorV2(gu, "group_v31_invite_bad", "Owner signing key mismatch", cmdId);
      return true;
    }
    if (groups::getGroupV2(gu, nullptr, nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, nullptr)) {
      notifyGroupStatusV2(gu, true, cmdId);
      emitInfoWaveRequest(0);
      return true;
    }
    if (!groups::upsertGroupV2(gu, channelId32, groupTag, canonicalName, gkey, keyVersion > 0 ? keyVersion : 1, role,
            0)) {
      notifyGroupSecurityErrorV2(gu, "group_v2_store_failed", "Cannot store accepted invite", cmdId);
      return true;
    }
    if (!groups::setOwnerSignPubKeyV2(gu, ownerSignPubKey)) {
      notifyGroupSecurityErrorV2(gu, "group_v31_invite_bad", "Cannot persist owner signing key", cmdId);
      return true;
    }
    {
      const uint16_t appliedKv = keyVersion > 0 ? keyVersion : 1;
      (void)groups::ackKeyAppliedV2(gu, appliedKv);
    }
    emitInfoWaveRequest(0);
    notifyGroupStatusV2(gu, false, cmdId);
    return true;
  }
  if (strcmp(cmd, "groupGrantIssue") == 0) {
    const char* groupUid = doc["groupUid"];
    const char* subjectId = doc["subjectId"];
    const char* roleStr = doc["role"];
    if (!groupUid || !groupUid[0] || !subjectId || !subjectId[0] || !roleStr || !roleStr[0]) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_grant_bad", "Missing groupUid/subjectId/role", cmdId);
      return true;
    }
    if (isSelfNodeHex(subjectId)) {
      groups::GroupRole role = parseGroupRoleStr(roleStr);
      if (role == groups::GroupRole::None || !groups::setGroupRoleV2(groupUid, role)) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_grant_bad", "Invalid role or unknown group", cmdId);
        return true;
      }
    }
    notifyGroupStatusV2(groupUid, false, cmdId);
    return true;
  }
  if (strcmp(cmd, "groupRevoke") == 0) {
    const char* groupUid = doc["groupUid"];
    const char* subjectId = doc["subjectId"];
    uint32_t revEpoch = doc["revocationEpoch"] | 0;
    if (!groupUid || !groupUid[0] || !subjectId || !subjectId[0]) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_revoke_bad", "Missing groupUid/subjectId", cmdId);
      return true;
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
    notifyGroupStatusV2(groupUid, false, cmdId);
    return true;
  }
  if (strcmp(cmd, "groupLeave") == 0) {
    const char* groupUid = doc["groupUid"];
    if (!groupUid || !groupUid[0]) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_leave_bad", "Missing groupUid", cmdId);
      return true;
    }
    if (!groups::removeGroupV2(groupUid)) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_leave_bad", "Unknown group", cmdId);
      return true;
    }
    emitInfoWaveRequest(cmdId);
    return true;
  }
  if (strcmp(cmd, "groupRekey") == 0) {
    const char* groupUid = doc["groupUid"];
    const char* keyB64 = doc["groupKey"];
    const char* rekeyOpId = doc["rekeyOpId"];
    uint16_t keyVersion = (uint16_t)(doc["keyVersion"] | 0);
    if (!groupUid || !groupUid[0]) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_rekey_bad", "Missing groupUid", cmdId);
      return true;
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
      if (!b64DecodeTo(keyB64, key, sizeof(key), &decLen) || decLen != 32) {
        notifyGroupSecurityErrorV2(groupUid, "group_v2_key_bad", "Bad groupKey", cmdId);
        return true;
      }
    } else {
      randombytes_buf(key, sizeof(key));
    }
    if (!groups::updateGroupKeyV2(groupUid, key, keyVersion)) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_rekey_bad", "Unknown group", cmdId);
      return true;
    }
    groups::GroupRole role = groups::GroupRole::None;
    uint16_t appliedVersion = 0;
    if (groups::getGroupV2(groupUid, nullptr, nullptr, 0, nullptr, 0, &appliedVersion, &role, nullptr, nullptr) &&
        role != groups::GroupRole::None) {
      groups::ackKeyAppliedV2(groupUid, appliedVersion);
    }
    notifyGroupRekeyProgressV2(groupUid, rekeyOpId, appliedVersion, cmdId);
    notifyGroupStatusV2(groupUid, false, cmdId);
    return true;
  }
  if (strcmp(cmd, "groupAckKeyApplied") == 0) {
    const char* groupUid = doc["groupUid"];
    uint16_t keyVersion = (uint16_t)(doc["keyVersion"] | 0);
    if (!groupUid || !groupUid[0] || keyVersion == 0) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_ack_bad", "Missing groupUid/keyVersion", cmdId);
      return true;
    }
    if (!groups::ackKeyAppliedV2(groupUid, keyVersion)) {
      notifyGroupSecurityErrorV2(groupUid, "group_v2_ack_bad", "Ack failed", cmdId);
      return true;
    }
    char selfHex[17] = {0};
    nodeIdToHexBuf(node::getId(), selfHex);
    notifyGroupMemberKeyStateV2(groupUid, selfHex, "applied", (uint32_t)(millis() / 1000), cmdId);
    notifyGroupStatusV2(groupUid, false, cmdId);
    return true;
  }
  if (strcmp(cmd, "groupSyncSnapshot") == 0) {
    JsonVariant groupsVar = doc["groups"];
    if (!groupsVar.is<JsonArray>()) {
      notifyGroupSecurityErrorV2(nullptr, "group_v2_snapshot_bad", "groups must be array", cmdId);
      return true;
    }
    JsonArray arr = groupsVar.as<JsonArray>();
    for (JsonVariant v : arr) {
      if (!v.is<JsonObject>()) continue;
      JsonObject g = v.as<JsonObject>();
      const char* groupUid = g["groupUid"];
      const char* gtag = g["groupTag"];
      const char* canonicalName = g["canonicalName"];
      const char* gkeyB64 = g["groupKey"];
      const char* roleStr = g["myRole"];
      uint32_t channelId32 = parseJsonChannelId32(g["channelId32"]);
      uint16_t keyVersion = (uint16_t)(g["keyVersion"] | 1);
      uint32_t revEpoch = g["revocationEpoch"] | 0;
      if (!groupUid || !groupUid[0] || !gtag || !gtag[0] || !canonicalName || !canonicalName[0] ||
          channelId32 <= groups::GROUP_ALL || !gkeyB64 || !gkeyB64[0])
        continue;
      uint8_t gkey[32];
      size_t decLen = 0;
      if (!b64DecodeTo(gkeyB64, gkey, sizeof(gkey), &decLen) || decLen != 32) continue;
      groups::GroupRole role = parseGroupRoleStr(roleStr);
      if (role == groups::GroupRole::None) role = groups::GroupRole::Member;
      if (!groups::upsertGroupV2(groupUid, channelId32, gtag, canonicalName, gkey, keyVersion, role, revEpoch)) continue;
      if (g["ackApplied"] == true) groups::ackKeyAppliedV2(groupUid, keyVersion);
    }
    emitInfoWaveRequest(cmdId);
    return true;
  }
  return false;
}

static void handleJsonCommand(const char* json, size_t len) {
  if (!json || len == 0 || len > kRxLineMax) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, len);
  if (err) {
    RIFTLINK_DIAG("BLE", "event=RX_PARSE_FAIL err=%s", err.c_str());
    return;
  }
  if (doc.overflowed()) {
    ble::notifyError("json_overflow", "ArduinoJson document overflow");
    return;
  }

  const char* cmd = doc["cmd"] | "";
  if (!cmd[0]) return;
  const uint32_t cmdId = parseCmdIdFromDoc(doc);
  if (!bleJsonCmdAllowsMissingCmdId(cmd) && cmdId == 0) {
    ble::notifyError("missing_cmdId", "cmdId required");
    return;
  }

  const ActiveCmdScope activeCmdScope(cmdId);

  if (handleGroupV2Commands(doc, cmd, cmdId)) return;

  if (strcmp(cmd, "info") == 0) {
    emitInfoWaveRequest(cmdId);
    return;
  }
  if (strcmp(cmd, "neighbors") == 0) {
    /* Не вызывать notifyNeighbors() сразу — та же очередь, что и волна info; иначе interleave с chunked node. */
    requestNeighborsNotify();
    return;
  }
  if (strcmp(cmd, "send") == 0) {
    const char* text = doc["text"] | "";
    if (!text[0]) return;
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
      if (triggerValueMs == 0) triggerValueMs = (uint32_t)(doc["triggerValueMs"] | 0);
    }
    uint32_t groupId = doc["group"] | 0U;
    if (groupId > 0) {
      if (!msg_queue::enqueueGroup(groupId, text)) {
        ble::notifyError("group_send", "Сообщение слишком длинное или ошибка шифрования");
      }
      return;
    }
    if (!s_onSend) return;
    const char* toHex = doc["to"] | "";
    uint8_t toId[protocol::NODE_ID_LEN];
    if (toHex[0]) {
      if (strlen(toHex) != protocol::NODE_ID_LEN * 2) {
        ble::notifyError("send_to_bad", "to must be full 16 hex node id");
        return;
      }
      for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
        char hx[3] = {toHex[i * 2], toHex[i * 2 + 1], 0};
        char* end = nullptr;
        unsigned long v = strtoul(hx, &end, 16);
        if (end != hx + 2) {
          ble::notifyError("send_to_bad", "to must be full 16 hex node id");
          return;
        }
        toId[i] = (uint8_t)v;
      }
    } else {
      memcpy(toId, protocol::BROADCAST_ID, protocol::NODE_ID_LEN);
    }
    uint8_t ttl = (uint8_t)(doc["ttl"] | 0);
    s_onSend(toId, text, ttl, critical, triggerType, triggerValueMs, false);
    if (triggerType != 0) {
      JsonDocument qdoc;
      qdoc["evt"] = "timeCapsuleQueued";
      const char* toStr = doc["to"];
      if (toStr && toStr[0]) qdoc["to"] = toStr;
      qdoc["trigger"] = trigger;
      if (triggerValueMs > 0) qdoc["triggerAtMs"] = triggerValueMs;
      emitJsonDoc(qdoc);
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

  if (strcmp(cmd, "invite") == 0) {
    uint32_t ttlSec = doc["ttlSec"] | 600;
    if (ttlSec < 60) ttlSec = 60;
    if (ttlSec > 3600) ttlSec = 3600;
    for (size_t i = 0; i < sizeof(s_inviteToken); i++) s_inviteToken[i] = (uint8_t)(random(256));
    s_inviteExpiryMs = millis() + ttlSec * 1000UL;
    s_inviteTokenValid = true;
    s_emitInviteCmdId = cmdId;
    notifyInvite();
    return;
  }

  if (strcmp(cmd, "channelKey") == 0) {
    const char* keyB64 = doc["key"];
    if (keyB64) {
      uint8_t key[32];
      if (b64Decode32(keyB64, key) && crypto::setChannelKey(key)) {
        emitInfoWaveRequest(cmdId);
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
        uint8_t chKey[32];
        if (b64Decode32(channelKeyB64, chKey)) crypto::setChannelKey(chKey);
      }
      uint8_t nodeId[protocol::NODE_ID_LEN];
      if (!parseFullNodeIdHex(idStr, nodeId)) {
        ble::notifyError("id_bad", "id must be full 16 hex node id");
        return;
      }
      uint8_t pubKey[32];
      if (b64Decode32(pubKeyB64, pubKey)) {
        x25519_keys::onKeyExchange(nodeId, pubKey);
        x25519_keys::sendKeyExchange(nodeId, true, false, "ble");
        if (inviteTokenHex && inviteTokenHex[0] && s_inviteTokenValid && (int32_t)(millis() - s_inviteExpiryMs) < 0) {
          char tokHex[17] = {0};
          for (int i = 0; i < 8; i++) snprintf(tokHex + i * 2, 3, "%02X", s_inviteToken[i]);
          if (strncmp(tokHex, inviteTokenHex, 16) == 0) {
            s_inviteTokenValid = false;
            memset(s_inviteToken, 0, sizeof(s_inviteToken));
            s_inviteExpiryMs = 0;
          }
        }
        emitInfoWaveRequest(cmdId);
      }
    }
    return;
  }

  if (strcmp(cmd, "routes") == 0 || strcmp(cmd, "mesh") == 0) {
    s_emitRoutesCmdId = cmdId;
    ble::notifyRoutes();
    return;
  }
  if (strcmp(cmd, "traceroute") == 0) {
    const char* toStr = doc["to"];
    if (!toStr || !toStr[0]) {
      ble::notifyError("traceroute_to_missing", "to is required (full 16 hex node id)");
      return;
    }
    uint8_t target[protocol::NODE_ID_LEN];
    if (!parseFullNodeIdHex(toStr, target)) {
      ble::notifyError("traceroute_to_bad", "to must be full 16 hex node id");
      return;
    }
    routing::requestRoute(target);
    s_emitRoutesCmdId = cmdId;
    ble::notifyRoutes();
    return;
  }
  if (strcmp(cmd, "groups") == 0) {
    s_emitGroupsCmdId = cmdId;
    ble::notifyGroups();
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
        emitInfoWaveRequest(cmdId);
      }
    }
    return;
  }
  if (strcmp(cmd, "modemPreset") == 0) {
    int p = parseModemPresetValue(doc);
    if (p >= 0 && p < 4) {
      if (!radio::requestModemPreset((radio::ModemPreset)p)) {
        ble::notifyError("modemPreset", "Queue busy, retry");
      } else {
        emitInfoWaveRequest(cmdId);
      }
    } else {
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
        emitInfoWaveRequest(cmdId);
      }
    }
    return;
  }

  if (strcmp(cmd, "nickname") == 0) {
    const char* nick = doc["nickname"];
    if (nick && strnlen(nick, 34) <= 32) {
      (void)node::setNickname(nick);
      emitInfoWaveRequest(cmdId);
    }
    return;
  }

  if (strcmp(cmd, "regeneratePin") == 0) {
    ble::regeneratePasskey();
    emitInfoWaveRequest(cmdId);
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
    if (doc["enabled"].is<bool>()) gps::setEnabled(doc["enabled"].as<bool>());
    if (doc["rx"].is<int>() || doc["rx"].is<int64_t>() || doc["tx"].is<int>() || doc["tx"].is<int64_t>() ||
        doc["en"].is<int>() || doc["en"].is<int64_t>()) {
      int rx = doc["rx"] | -1;
      int tx = doc["tx"] | -1;
      int en = doc["en"] | -1;
      gps::setPins(rx, tx, en);
    }
    gps::saveConfig();
    int rx = -1, tx = -1, en = -1;
    gps::getPins(&rx, &tx, &en);
    s_emitGpsCmdId = cmdId;
    ble::notifyGps(gps::isPresent(), gps::isEnabled(), gps::hasFix(), rx, tx, en);
    return;
  }

  if (strcmp(cmd, "selftest") == 0 || strcmp(cmd, "test") == 0) {
    selftest::Result st;
    selftest::run(&st);
    ble::notifySelftest(st.radioOk, st.displayOk, st.batteryMv, st.heapFree, cmdId);
    return;
  }

  if (strcmp(cmd, "loraScan") == 0) {
    static selftest::ScanResult s_loraScanRes[8];
    int n = selftest::modemScanQuick(s_loraScanRes, 8);
    emitLoraScanDoc(cmdId, n, s_loraScanRes);
    return;
  }

  if (strcmp(cmd, "signalTest") == 0) {
    int n = neighbors::getCount();
    if (n == 0) {
      ble::notifyError("signal_test_no_neighbors", "no neighbors on device");
      return;
    }
    for (int i = 0; i < n && i < 8; i++) {
      uint8_t peerId[protocol::NODE_ID_LEN];
      if (!neighbors::getId(i, peerId)) continue;
      uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID + 64];
      uint16_t pktId = ++s_diagPktIdCounter;
      size_t plen = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), peerId, 31, protocol::OP_PING, nullptr, 0,
          false, false, false, protocol::CHANNEL_DEFAULT, pktId);
      if (plen > 0) {
        if (cmdId != 0) rememberPingCmdId(peerId, cmdId);
        uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(peerId));
        char reasonBuf[40];
        uint32_t delayMs = 140u + (uint32_t)(i * 220u) + (uint32_t)(random(90));
        if (!queueTxPacket(pkt, plen, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
          queueDeferredSend(pkt, plen, txSf, delayMs, true);
        }
      }
    }
    (void)cmdId;
    return;
  }

  if (strcmp(cmd, "ping") == 0) {
    const char* toStr = doc["to"];
    uint8_t to[protocol::NODE_ID_LEN];
    memset(to, 0xFF, protocol::NODE_ID_LEN);
    if (toStr && toStr[0]) {
      if (!parseFullNodeIdHex(toStr, to)) {
        ble::notifyError("ping_to_bad", "to must be full 16 hex node id");
        return;
      }
    }
    uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID + 64];
    uint16_t pktId = ++s_diagPktIdCounter;
    size_t plen = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), to, 31, protocol::OP_PING, nullptr, 0, false,
        false, false, protocol::CHANNEL_DEFAULT, pktId);
    if (plen > 0) {
      if (cmdId != 0) rememberPingCmdId(to, cmdId);
      uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(to));
      char reasonBuf[40];
      if (!queueTxPacket(pkt, plen, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
        queueDeferredSend(pkt, plen, txSf, 60 + (uint32_t)(random(40)), true);
        RIFTLINK_DIAG("PING", "event=PING_TX_DEFER to=%02X%02X pktId=%u cause=%s",
            to[0], to[1], (unsigned)pktId, reasonBuf[0] ? reasonBuf : "?");
      } else {
        RIFTLINK_DIAG("PING", "event=PING_TX_QUEUED to=%02X%02X pktId=%u sf=%u",
            to[0], to[1], (unsigned)pktId, (unsigned)txSf);
      }
      s_pingRetry.active = true;
      memcpy(s_pingRetry.to, to, protocol::NODE_ID_LEN);
      s_pingRetry.txSf = txSf;
      s_pingRetry.retriesLeft = kPingRetryMaxExtra;
      s_pingRetry.deadlineMs = millis() + pingAckWaitMs(txSf);
    }
    (void)cmdId;
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
      size_t pktLen = protocol::buildPacket(pkt, sizeof(pkt), node::getId(), to, 31, protocol::OP_READ, payload, 4, false, false);
      if (pktLen > 0) {
        uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(to));
        char reasonBuf[40];
        if (!queueTxPacket(pkt, pktLen, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
          queueDeferredSend(pkt, pktLen, txSf, 60 + (uint32_t)(random(40)), true);
        }
      }
    }
    return;
  }

  if (strcmp(cmd, "voice") == 0) {
    const char* toStr = doc["to"];
    const char* dataStr = doc["data"];
    int chunk = doc["chunk"] | -1;
    int total = doc["total"] | -1;
    if (!toStr || !toStr[0] || !dataStr || chunk < 0 || total <= 0) {
      RIFTLINK_DIAG("BLE", "event=VOICE_RX_DROP reason=missing_fields");
      return;
    }
    if (strlen(dataStr) > BLE_VOICE_CHUNK_B64_BUF) {
      RIFTLINK_DIAG("BLE", "event=VOICE_RX_DROP reason=b64_too_long");
      return;
    }
    if (!parseFullNodeIdHex(toStr, s_voiceTo)) {
      ble::notifyError("voice_to_bad", "to must be full 16 hex node id");
      return;
    }
    if (chunk == 0) {
      if (!voice_buffers_init()) {
        ble::notifyError("voice_init", "voice_buffers_init failed");
        return;
      }
      if (!voice_buffers_acquire()) {
        ble::notifyError("voice_oom", "voice buffer alloc failed");
        return;
      }
      s_voiceBleLocked = true;
      s_voiceBufLen = 0;
      s_voiceChunkTotal = total;
    }
    if (s_voiceChunkTotal != total) {
      if (s_voiceBleLocked) {
        voice_buffers_release();
        s_voiceBleLocked = false;
      }
      s_voiceBufLen = 0;
      s_voiceChunkTotal = -1;
      return;
    }
    uint8_t* plain = voice_buffers_plain();
    const size_t plainCap = voice_buffers_plain_cap();
    size_t maxDec = plainCap - s_voiceBufLen;
    size_t binLen = 0;
    if (sodium_base642bin(plain + s_voiceBufLen, maxDec, dataStr, strlen(dataStr), nullptr, &binLen, nullptr,
            sodium_base64_VARIANT_ORIGINAL) != 0) {
      RIFTLINK_DIAG("BLE", "event=VOICE_RX_B64_ERR");
    } else {
      s_voiceBufLen += binLen;
    }
    if (chunk == total - 1) {
      if (s_voiceBufLen > 0 && s_voiceBufLen <= voice_frag::MAX_VOICE_PLAIN) {
        uint32_t voiceMsgId = 0;
        bool hasKey = x25519_keys::hasKeyFor(s_voiceTo);
        bool ok = voice_frag::send(s_voiceTo, plain, s_voiceBufLen, &voiceMsgId);
        if (ok && voiceMsgId != 0) ble::notifySent(s_voiceTo, voiceMsgId);
        if (!ok) {
          ble::notifyError("voice_send_fail", hasKey ? "voice_frag::send failed (queue full or too large?)"
                                                     : "no pairwise key for peer — key exchange not completed");
        }
      }
      s_voiceBufLen = 0;
      s_voiceChunkTotal = -1;
      if (s_voiceBleLocked) {
        voice_buffers_release();
        s_voiceBleLocked = false;
      }
    }
    return;
  }
  if (strcmp(cmd, "shutdown") == 0 || strcmp(cmd, "poweroff") == 0) {
    ble::notifyError("poweroff_unsupported", "Управление питанием по BLE на nRF не реализовано");
    return;
  }
  if (strcmp(cmd, "powersave") == 0) {
    ble::notifyError("powersave_unsupported", "powersave по BLE на nRF не реализован");
    return;
  }
  if (cmd[0] && strncmp(cmd, "bleOta", 6) == 0) {
    ble::notifyError("ble_ota_unsupported", "BLE OTA прошивки на nRF в этой сборке не поддерживается (DFU по USB)");
    return;
  }
  if (strcmp(cmd, "espnowChannel") == 0 || strcmp(cmd, "espnowAdaptive") == 0) {
    ble::notifyError("espnow_unsupported", "ESP-NOW на nRF отключён");
    return;
  }

  if (strcmp(cmd, "radioMode") == 0) {
    const char* mode = doc["mode"];
    if (mode && strcmp(mode, "ble") == 0) {
      /* Как ble.cpp: switchTo(BLE) без лишнего evt:info. */
      return;
    }
    ble::notifyError("radioMode", "nRF: только BLE, Wi‑Fi недоступен");
    return;
  }
  if (strcmp(cmd, "wifi") == 0) {
    ble::notifyError("wifi", "nRF: Wi‑Fi недоступен");
    return;
  }
  if (strcmp(cmd, "ota") == 0) {
    ble::notifyError("ota_unsupported", "Wi‑Fi OTA на nRF недоступна; обновление: DFU/USB (docs/flasher/NRF52.md)");
    return;
  }
  if (strcmp(cmd, "lang") == 0) {
    const char* lang = doc["lang"];
    if (lang) {
      if (strcmp(lang, "ru") == 0) {
        if (locale::getLang() != LANG_RU) {
          if (locale::setLang(LANG_RU)) menu_nrf_request_redraw_after_locale();
        }
      } else if (strcmp(lang, "en") == 0) {
        if (locale::getLang() != LANG_EN) {
          if (locale::setLang(LANG_EN)) menu_nrf_request_redraw_after_locale();
        }
      }
    }
    /* Как ble.cpp: только смена локали, без emit info. */
    return;
  }

  if (strcmp(cmd, "addGroup") == 0 || strcmp(cmd, "removeGroup") == 0 || strcmp(cmd, "setGroupKey") == 0 ||
      strcmp(cmd, "clearGroupKey") == 0 || strcmp(cmd, "setGroupAdminCap") == 0 ||
      strcmp(cmd, "clearGroupAdminCap") == 0 || strcmp(cmd, "getGroupKey") == 0) {
    ble::notifyError("group_legacy_cmd_unsupported", "Legacy V1 group command is not supported");
    return;
  }

  char unknownBuf[80];
  snprintf(unknownBuf, sizeof(unknownBuf), "%.79s", cmd ? cmd : "");
  ble::notifyError("unknown_cmd", unknownBuf);
}

static void connect_callback(uint16_t conn_handle) {
  (void)conn_handle;
  /* Не Serial/DIAG здесь — колбэк SoftDevice; s_connected и лог — в syncBleConnectionAtUpdateEntry(). */
  /* Но для отладки зависаний — минимальный лог в Serial1 (T114) */
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  {
    char b[96];
    snprintf(b, sizeof(b), "[RiftLink] BLE CB event=CONNECT cb_t=%lu", (unsigned long)millis());
    Serial1.println(b);
  }
#endif
}

static void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  (void)conn_handle;
  s_lastBleDiscReason = reason;
  /* Лог DISCONNECTED — в syncBleConnectionAtUpdateEntry() при нисходящем фронте (не из колбэка). */
  /* Но для отладки зависаний — минимальный лог в Serial1 (T114) */
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  {
    char b[96];
    snprintf(b, sizeof(b), "[RiftLink] BLE CB event=DISCONNECT reason=0x%02X cb_t=%lu", (unsigned)reason, (unsigned long)millis());
    Serial1.println(b);
  }
#endif
}

static void startAdvertising() {
  char name[24];
  getAdvertisingName(name, sizeof(name));

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(s_riftNus);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.setName(name);
  Bluefruit.Advertising.start(0);
}

bool init() {
  if (s_inited) return true;

  loadOrCreatePasskey();

  // T114: зелёный GPIO35 — conn LED Bluefruit (мигание при рекламе BLE). Отключаем: не цель паритета, раздражает рядом с NeoPixel.
#if defined(RIFTLINK_BOARD_HELTEC_T114)
  Bluefruit.autoConnLed(false);
#else
  Bluefruit.autoConnLed(true);
#endif
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  // Сначала SoftDevice (Bluefruit.begin): иначе первый sodium_init() в loadOrGenerateGroupOwnerSigningKey()
  // до поднятия SD на части nRF52840 блокируется навсегда.
  if (!Bluefruit.begin(1)) {
    RIFTLINK_LOG_ERR("[RiftLink] Bluefruit.begin failed\n");
    return false;
  }
  Serial.println("[RiftLink] BLE: SoftDevice/Bluefruit.begin ok");
  Serial.flush();

  // Ключи mesh (ChaCha) + sodium: до loadOrGenerateGroupOwnerSigningKey(), иначе libsodium в signing зависает.
  if (!crypto::init()) {
    Serial.println("[RiftLink] BLE: crypto::init failed");
    Serial.flush();
    return false;
  }
  Serial.println("[RiftLink] BLE: crypto::init ok");
  Serial.flush();

  (void)loadOrGenerateGroupOwnerSigningKey();
  Serial.println("[RiftLink] BLE: group signing key ok");
  Serial.flush();

  Bluefruit.setTxPower(4);
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);
  /* Как Meshtastic NRF52Bluetooth::setup(): slave latency 0 (SoftDevice + iOS), интервал 12–80 ×1.25 ms ≈ 15–100 ms. */
  Bluefruit.Periph.setConnSlaveLatency(0);
  Bluefruit.Periph.setConnInterval(12, 80);

  {
    uint16_t max_mtu = Bluefruit.getMaxMtu(BLE_GAP_ROLE_PERIPH);
    err_t err = s_riftNus.begin();
    if (err != ERROR_NONE) {
      Serial.printf("[RiftLink] BLE: NUS service begin failed err=%u\n", (unsigned)err);
      return false;
    }
    s_riftNusTxd.setProperties(CHR_PROPS_NOTIFY);
    s_riftNusTxd.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
    s_riftNusTxd.setMaxLen(max_mtu);
    s_riftNusTxd.setUserDescriptor("TXD");
    err = s_riftNusTxd.begin();
    if (err != ERROR_NONE) {
      Serial.printf("[RiftLink] BLE: NUS TXD begin failed err=%u\n", (unsigned)err);
      return false;
    }
    s_riftNusRxd.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
    s_riftNusRxd.setPermission(SECMODE_NO_ACCESS, SECMODE_OPEN);
    s_riftNusRxd.setMaxLen(max_mtu);
    s_riftNusRxd.setUserDescriptor("RXD");
    s_riftNusRxd.setWriteCallback(onRiftNusRxdWrite, true);
    err = s_riftNusRxd.begin();
    if (err != ERROR_NONE) {
      Serial.printf("[RiftLink] BLE: NUS RXD begin failed err=%u\n", (unsigned)err);
      return false;
    }
  }

  startAdvertising();
  s_inited = true;
  RIFTLINK_DIAG("BLE", "event=INIT_OK name=%s", "RL-…");
  return true;
}

void deinit() {
  resetBleOutQueueOnDisconnect();
  resetIncomingCmdQueue();
  Bluefruit.Advertising.stop();
  s_inited = false;
}

/**
 * В начале каждого `update()`: зеркалировать SoftDevice и при обрыве сразу освободить TX (chunked heap) и волну info.
 * Иначе после disconnect колбэк мог обнулить флаги, а очередь ещё ждать первого `pumpBleTx` — гонка/«залипание» узла
 * (processInfoWaveStep / pending notify при отсутствии линка).
 */
static void syncBleConnectionAtUpdateEntry() {
  const bool conn = (Bluefruit.connected() > 0);
  if (conn && !s_bleConnPrevForDiag) {
    /* Сначала окно трассы, потом строка CONNECTED — иначе postConnTraceIf отсекает до arm. */
    s_postConnTraceUntilMs = millis() + 20000;
    s_postConnBleDetailPassesLeft = 40;
    s_postConnMainTraceLeft = 48;
#if defined(RIFTLINK_BOARD_HELTEC_T114)
    {
      char b[96];
      snprintf(b, sizeof(b), "[RiftLink] BLE event=CONNECTED t=%lu", (unsigned long)millis());
      Serial1.println(b);
    }
#else
    RIFTLINK_DIAG("BLE", "event=CONNECTED");
#endif
    postConnTraceIf("sync_arm_trace");
  }
  if (!conn && s_bleConnPrevForDiag) {
#if defined(RIFTLINK_BOARD_HELTEC_T114)
    {
      char b[96];
      snprintf(b, sizeof(b), "[RiftLink] BLE event=DISCONNECTED reason=0x%02X t=%lu", (unsigned)s_lastBleDiscReason,
          (unsigned long)millis());
      Serial1.println(b);
    }
#else
    RIFTLINK_DIAG("BLE", "event=DISCONNECTED reason=0x%02X", (unsigned)s_lastBleDiscReason);
#endif
  }
  s_bleConnPrevForDiag = conn;
  s_connected = conn;
  if (!conn) {
    s_postConnTraceUntilMs = 0;
    s_postConnBleDetailPassesLeft = 0;
    s_postConnMainTraceLeft = 0;
    /* 1. Сброс info wave ДО очистки очередей — иначе chunked heap остаётся висеть */
    resetInfoWaveOnDisconnect();
    /* 2. Очистка TX-очереди (освобождает всю кучу chunked NDJSON) */
    resetBleOutQueueOnDisconnect();
    /* 3. Очистка RX-очереди */
    resetIncomingCmdQueue();
    /* 4. Отдача времени SoftDevice — только один короткий delay, иначе блокировка loop на 300мс!
     * SoftDevice требует ~10-20мс на завершение, не 100мс. */
    yield();
    delay(10);  // Один короткий delay вместо 5×20мс
  }
}

static void consumeIncomingCmdQueue() {
  if (!s_connected) return;
  for (uint8_t i = 0; i < kInCmdConsumePerUpdate && s_inCmdQCount.load(std::memory_order_relaxed) > 0; i++) {
    NrfBleInCmdItem& it = s_inCmdQ[s_inCmdQTail];
    s_inCmdQTail = (uint8_t)((s_inCmdQTail + 1) % kInCmdQDepth);
    s_inCmdQCount.fetch_sub(1, std::memory_order_relaxed);
    postConnTraceIf("u03b_before_handle_json");
    riftlink_wdt_feed();
    yield();
    handleJsonCommand((const char*)it.data, it.len);
    riftlink_wdt_feed();
    yield();
    postConnTraceIf("u03c_after_handle_json");
  }
}

void update() {
  if (!s_inited) return;

  riftlink_wdt_feed();
  
  /* T114: отладка — лог каждого вызова update() */
#if defined(RIFTLINK_BOARD_HELTEC_T114) && defined(RIFTLINK_BLE_DEBUG_UPDATE)
  {
    static uint32_t s_lastUpdateLogMs = 0;
    if ((uint32_t)(millis() - s_lastUpdateLogMs) >= 1000) {
      char b[128];
      snprintf(b, sizeof(b), "[RiftLink] BLE_UPDATE t=%lu conn=%d inited=%d outCount=%u",
          (unsigned long)millis(), (int)Bluefruit.connected(), (int)s_inited, (unsigned)s_outCount);
      Serial1.println(b);
      s_lastUpdateLogMs = millis();
    }
  }
#endif
  
  syncBleConnectionAtUpdateEntry();
  postConnTraceIf("u01_after_sync");
  /* Как ESP ble.cpp: очередь команд из GATT до pump TX (порядок обработки входящих). */
  postConnTraceIf("u03_rx_queue_enter");
  consumeIncomingCmdQueue();
  postConnTraceIf("u04_rx_queue_exit");
  pumpBleTx(kBleNdjsonTxBudgetBytes, kBleOutLinesPerUpdateFlush);
  postConnTraceIf("u02_after_pump_tx_a");

  pumpBleTx(kBleNdjsonTxBudgetBytes, kBleOutLinesPerUpdateFlush);
  postConnTraceIf("u05_after_pump_tx_b");

  postConnTraceIf("u06_before_info_wave");
  processInfoWaveStep();
  postConnTraceIf("u07_after_info_wave");

  pingRetryTick();
  postConnTraceIf("u08_after_ping_retry");

  flushPendingBleOutbound();
  postConnTraceIf("u09_after_flush_pending");

  pumpBleTx(kBleNdjsonTxBudgetBytes, kBleOutLinesPerUpdateFlush);
  postConnTraceIf("u10_after_pump_tx_c");
  pumpBleTx(kBleNdjsonTxBudgetBytes, kBleOutLinesPerUpdateFlush);
  postConnTraceIf("u11_update_end");
  if (s_postConnBleDetailPassesLeft > 0) s_postConnBleDetailPassesLeft--;

  /* Сброс TX-очереди только на нисходящем фронте линка (после flush), не при любом connected()==0:
   * иначе кратковременный «ещё не connected» при установлении связи или между двумя ble::update() в loop
   * мог снести очередь между enqueue волны info и отправкой — обрыв/рестарт сразу после connect. */
  {
    const bool connEnd = (Bluefruit.connected() > 0);
    if (s_bleConnAtPrevUpdateEnd && !connEnd && s_outCount > 0) resetBleOutQueueOnDisconnect();
    s_bleConnAtPrevUpdateEnd = connEnd;
  }
}

void postConnectTrace(const char* tag) {
  if (tag && tag[0]) postConnTraceIf(tag);
}

void setOnSend(void (*cb)(const uint8_t* to, const char* text, uint8_t ttlMinutes, bool critical,
        uint8_t triggerType, uint32_t triggerValueMs, bool isSos)) {
  s_onSend = cb;
}

void setOnLocation(void (*cb)(float lat, float lon, int16_t alt, uint16_t radiusM, uint32_t expiryEpochSec)) {
  s_onLocation = cb;
}

/**
 * Отправка evt:msg. Длинный JSON (≥512 B сериализации) — NDJSON-чанки, как ESP `notifyJsonToApp` / docs/API.md §476.
 * Возвращает false, если нет транспорта или сериализация не удалась (запись в FIFO TX не удалась).
 */
static bool emitMsgEvt(const uint8_t* from, const char* text, uint32_t msgId, int rssi, uint8_t ttlMinutes,
    const char* lane, const char* type, uint32_t groupId, const char* groupUid) {
  if (!from || !text) return false;
  if (!s_connected || Bluefruit.connected() == 0) return false;
  JsonDocument doc;
  doc["evt"] = "msg";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i * 2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  char textBuf[kBlePendingMsgTextMax + 1];
  strncpy(textBuf, text, kBlePendingMsgTextMax);
  textBuf[kBlePendingMsgTextMax] = '\0';
  sanitizeBleJsonCStringInPlace(textBuf);
  doc["text"] = textBuf;
  if (msgId != 0) doc["msgId"] = msgId;
  if (rssi != 0) doc["rssi"] = rssi;
  if (ttlMinutes != 0) doc["ttl"] = ttlMinutes;
  if (lane && lane[0]) {
    char laneBuf[sizeof(s_pendingMsgLane)];
    strncpy(laneBuf, lane, sizeof(laneBuf) - 1);
    laneBuf[sizeof(laneBuf) - 1] = '\0';
    sanitizeBleJsonCStringInPlace(laneBuf);
    doc["lane"] = laneBuf;
  }
  if (type && type[0]) {
    char typeBuf[sizeof(s_pendingMsgType)];
    strncpy(typeBuf, type, sizeof(typeBuf) - 1);
    typeBuf[sizeof(typeBuf) - 1] = '\0';
    sanitizeBleJsonCStringInPlace(typeBuf);
    doc["type"] = typeBuf;
  }
  if (groupId > 0) doc["group"] = groupId;
  if (groupUid && groupUid[0]) {
    char gidBuf[groups::GROUP_UID_MAX_LEN + 1];
    strncpy(gidBuf, groupUid, sizeof(gidBuf) - 1);
    gidBuf[sizeof(gidBuf) - 1] = '\0';
    sanitizeBleJsonCStringInPlace(gidBuf);
    doc["groupUid"] = gidBuf;
  }
  const size_t n = measureJson(doc);
  if (n == 0) return false;
  if (n >= kTxJsonMax) {
    char* heap = nullptr;
    size_t w = 0;
    if (!serializeDocToHeapForChunkedLine(doc, &heap, &w)) {
      RIFTLINK_DIAG("BLE", "event=MSG_TX_FAIL reason=chunked_serialize n=%u", (unsigned)n);
      return false;
    }
    if (!scheduleNdjsonLineOwned(heap, w)) {
      return false;
    }
    return true;
  }
  char buf[kTxJsonMax];
  const size_t w = serializeJson(doc, buf, sizeof(buf));
  if (w == 0 || w >= sizeof(buf)) return false;
  buf[w] = '\0';
  if (w < 2 || buf[w - 1] != '}') return false;
  return enqueueJsonLine(buf);
}

void notifyMsg(const uint8_t* from, const char* text, uint32_t msgId, int rssi, uint8_t ttlMinutes,
    const char* lane, const char* type, uint32_t groupId, const char* groupUid) {
  (void)emitMsgEvt(from, text, msgId, rssi, ttlMinutes, lane, type, groupId, groupUid);
}

void requestMsgNotify(const uint8_t* from, const char* text, uint32_t msgId, int rssi, uint8_t ttlMinutes,
    const char* lane, const char* type, uint32_t groupId, const char* groupUid) {
  if (!from || !text) return;
  if (s_pendingMsg) {
    RIFTLINK_DIAG("BLE", "event=REQUEST_MSG action=overwrite prevMsgId=%u", (unsigned)s_pendingMsgId);
  }
  memcpy(s_pendingMsgFrom, from, protocol::NODE_ID_LEN);
  strncpy(s_pendingMsgText, text, kBlePendingMsgTextMax);
  s_pendingMsgText[kBlePendingMsgTextMax] = '\0';
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

void notifyDelivered(const uint8_t* from, uint32_t msgId, int rssi) {
  if (!from) return;
  JsonDocument doc;
  doc["evt"] = "delivered";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i * 2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["msgId"] = msgId;
  if (rssi != 0) doc["rssi"] = rssi;
  emitJsonDoc(doc);
}

void notifyRead(const uint8_t* from, uint32_t msgId, int rssi) {
  if (!from) return;
  JsonDocument doc;
  doc["evt"] = "read";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i * 2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["msgId"] = msgId;
  if (rssi != 0) doc["rssi"] = rssi;
  emitJsonDoc(doc);
}

void notifySent(const uint8_t* to, uint32_t msgId) {
  if (!to) return;
  JsonDocument doc;
  doc["evt"] = "sent";
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(toHex + i * 2, 3, "%02X", to[i]);
  doc["to"] = toHex;
  doc["msgId"] = msgId;
  emitJsonDoc(doc);
}

void notifyWaitingKey(const uint8_t* to) {
  if (!to) return;
  if (!s_connected || Bluefruit.connected() == 0) return;
  JsonDocument doc;
  doc["evt"] = "waiting_key";
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(toHex + i * 2, 3, "%02X", to[i]);
  doc["to"] = toHex;
  emitJsonDoc(doc);
}

void notifyUndelivered(const uint8_t* to, uint32_t msgId) {
  if (!to) return;
  JsonDocument doc;
  doc["evt"] = "undelivered";
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(toHex + i * 2, 3, "%02X", to[i]);
  doc["to"] = toHex;
  doc["msgId"] = msgId;
  emitJsonDoc(doc);
}

void notifyBroadcastDelivery(uint32_t msgId, int delivered, int total) {
  JsonDocument doc;
  // Как ble.cpp tryNotifyBroadcastDelivery: при нуле доставок — evt:undelivered (тот же JSON + delivered/total).
  if (total > 0 && delivered == 0) {
    doc["evt"] = "undelivered";
  } else {
    doc["evt"] = "broadcast_delivery";
  }
  doc["msgId"] = msgId;
  doc["delivered"] = delivered;
  doc["total"] = total;
  emitJsonDoc(doc);
}

void notifyLocation(const uint8_t* from, float lat, float lon, int16_t alt, int rssi) {
  if (!from) return;
  JsonDocument doc;
  doc["evt"] = "location";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i * 2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["lat"] = lat;
  doc["lon"] = lon;
  doc["alt"] = alt;
  if (rssi != 0) doc["rssi"] = rssi;
  emitJsonDoc(doc);
}

void notifyTelemetry(const uint8_t* from, uint16_t batteryMv, uint16_t heapKb, int rssi) {
  if (!from) return;
  JsonDocument doc;
  doc["evt"] = "telemetry";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i * 2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["battery"] = batteryMv;
  doc["heapKb"] = heapKb;
  if (rssi != 0) doc["rssi"] = rssi;
  emitJsonDoc(doc);
}

void notifyRelayProof(const uint8_t* relayedBy, const uint8_t* from, const uint8_t* to, uint16_t pktId,
    uint8_t opcode) {
  if (!relayedBy || !from || !to) return;
  JsonDocument doc;
  doc["evt"] = "relayProof";
  char byHex[17];
  char fromHex[17];
  char toHex[17];
  nodeIdToHexBuf(relayedBy, byHex);
  nodeIdToHexBuf(from, fromHex);
  nodeIdToHexBuf(to, toHex);
  doc["relayedBy"] = byHex;
  doc["from"] = fromHex;
  doc["to"] = toHex;
  doc["pktId"] = pktId;
  doc["opcode"] = opcode;
  emitJsonDoc(doc);
}

void notifyTimeCapsuleReleased(const uint8_t* to, uint32_t msgId, uint8_t triggerType) {
  if (!to) return;
  JsonDocument doc;
  doc["evt"] = "timeCapsuleReleased";
  char toHex[17];
  nodeIdToHexBuf(to, toHex);
  doc["to"] = toHex;
  doc["msgId"] = msgId;
  if (triggerType == 1) doc["trigger"] = "target_online";
  else if (triggerType == 2) doc["trigger"] = "deliver_after_time";
  emitJsonDoc(doc);
}

void notifyInfo() {
  emitInfoWaveRequest(0);
}

void notifyInvite() {
  const uint32_t cmdId = s_emitInviteCmdId;
  s_emitInviteCmdId = 0;

  uint8_t pubKey[32];
  if (!x25519_keys::getOurPublicKey(pubKey)) return;
  char pubKeyB64[64];
  if (!bin32ToBase64(pubKey, pubKeyB64, sizeof(pubKeyB64))) return;

  JsonDocument doc;
  doc["evt"] = "invite";
  if (cmdId != 0) doc["cmdId"] = cmdId;
  const uint8_t* id = node::getId();
  char idHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(idHex + i * 2, 3, "%02X", id[i]);
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
    char chKeyB64[64];
    if (bin32ToBase64(chKey, chKeyB64, sizeof(chKeyB64))) doc["channelKey"] = chKeyB64;
  }
  emitJsonDoc(doc);
}

void notifyNeighbors() {
  const uint32_t seq = ++s_bleSyncSeq;
  notifyNeighborsWithSeq(seq, 0);
}

static void tryFlushPendingPong() {
  if (!s_pendingPong || !s_inited) return;
  if (!s_connected || Bluefruit.connected() == 0) return;
  const uint8_t* from = s_pendingPongFrom;
  JsonDocument doc;
  doc["evt"] = "pong";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i * 2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  if (s_pendingPongRssi != 0) doc["rssi"] = s_pendingPongRssi;
  if (s_pendingPongPktId != 0) doc["pingPktId"] = s_pendingPongPktId;
  const uint32_t appCmdId = peekPingCmdIdForFrom(from);
  if (appCmdId != 0) doc["cmdId"] = appCmdId;
  char buf[kTxJsonMax];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) return;
  buf[n] = '\0';
  if (!enqueueJsonLine(buf)) return;
  s_pendingPong = false;
  (void)takePingCmdIdForFrom(from);
}

static void flushPendingBleOutbound() {
  if (!s_inited) return;
  if (!s_connected || Bluefruit.connected() == 0) return;
  tryFlushPendingPong();
  if (s_pendingMsg) {
    const bool sent = emitMsgEvt(s_pendingMsgFrom, s_pendingMsgText, s_pendingMsgId, s_pendingMsgRssi, s_pendingMsgTtl,
        s_pendingMsgLane, s_pendingMsgType, s_pendingMsgGroupId, s_pendingMsgGroupUid[0] ? s_pendingMsgGroupUid : nullptr);
    if (sent) s_pendingMsg = false;
  }
  if (s_pendingNeighbors) {
    if (infoWavePipelineBusy()) {
      /* Флаг остаётся true — снимем после волны info в следующем update(). */
    } else {
      s_pendingNeighbors = false;
      notifyNeighbors();
    }
  }
}

void requestNeighborsNotify() {
  s_pendingNeighbors = true;
}

void notifyRoutes() {
  const uint32_t cmdId = s_emitRoutesCmdId;
  s_emitRoutesCmdId = 0;
  const uint32_t seq = ++s_bleSyncSeq;
  notifyRoutesWithSeq(seq, cmdId);
}

void notifyGroups() {
  const uint32_t cmdId = s_emitGroupsCmdId;
  s_emitGroupsCmdId = 0;
  const uint32_t seq = ++s_bleSyncSeq;
  notifyGroupsWithSeq(seq, cmdId);
}

void notifyWifi(bool connected, const char* ssid, const char* ip) {
  if (!s_connected || Bluefruit.connected() == 0) return;
  JsonDocument doc;
  doc["evt"] = "wifi";
  doc["connected"] = connected;
  if (ssid) {
    char ssidBuf[33];
    strncpy(ssidBuf, ssid, sizeof(ssidBuf) - 1);
    ssidBuf[sizeof(ssidBuf) - 1] = '\0';
    sanitizeBleJsonCStringInPlace(ssidBuf);
    doc["ssid"] = ssidBuf;
  }
  if (ip) {
    char ipBuf[24];
    strncpy(ipBuf, ip, sizeof(ipBuf) - 1);
    ipBuf[sizeof(ipBuf) - 1] = '\0';
    sanitizeBleJsonCStringInPlace(ipBuf);
    doc["ip"] = ipBuf;
  }
  emitJsonDoc(doc);
}

void notifyRegion(const char* code, float freq, int power, int channel, uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "region";
  if (cmdId != 0) doc["cmdId"] = cmdId;
  char codeBuf[16];
  strncpy(codeBuf, code ? code : "", sizeof(codeBuf) - 1);
  codeBuf[sizeof(codeBuf) - 1] = '\0';
  sanitizeBleJsonCStringInPlace(codeBuf);
  doc["region"] = codeBuf;
  doc["freq"] = freq;
  doc["power"] = power;
  if (channel >= 0) doc["channel"] = channel;
  emitJsonDoc(doc);
}

void notifyGps(bool present, bool enabled, bool hasFix, int rx, int tx, int en) {
  const uint32_t cmdId = s_emitGpsCmdId;
  s_emitGpsCmdId = 0;
  JsonDocument doc;
  doc["evt"] = "gps";
  if (cmdId != 0) doc["cmdId"] = cmdId;
  doc["present"] = present;
  doc["enabled"] = enabled;
  doc["hasFix"] = hasFix;
  if (rx >= 0) doc["rx"] = rx;
  if (tx >= 0) doc["tx"] = tx;
  if (en >= 0) doc["en"] = en;
  emitJsonDoc(doc);
}

void notifyPong(const uint8_t* from, int rssi, uint16_t pingPktId) {
  if (!from) return;
  JsonDocument doc;
  doc["evt"] = "pong";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i * 2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  if (rssi != 0) doc["rssi"] = rssi;
  if (pingPktId != 0) doc["pingPktId"] = pingPktId;
  const uint32_t appCmdId = peekPingCmdIdForFrom(from);
  if (appCmdId != 0) doc["cmdId"] = appCmdId;
  char buf[kTxJsonMax];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) {
    RIFTLINK_DIAG("BLE", "event=PONG_JSON_SERIAL_FAIL n=%u cap=%u", (unsigned)n, (unsigned)sizeof(buf));
    return;
  }
  buf[n] = '\0';
  if (!enqueueJsonLine(buf)) {
    memcpy(s_pendingPongFrom, from, protocol::NODE_ID_LEN);
    s_pendingPongRssi = rssi;
    s_pendingPongPktId = pingPktId;
    s_pendingPong = true;
    return;
  }
  (void)takePingCmdIdForFrom(from);
}

void clearPingRetryForPeer(const uint8_t* from) {
  if (!from || !s_pingRetry.active) return;
  if (memcmp(s_pingRetry.to, from, protocol::NODE_ID_LEN) != 0) return;
  s_pingRetry.active = false;
  RIFTLINK_DIAG("PING", "event=PING_RETRY_CLEAR reason=pong from=%02X%02X", from[0], from[1]);
}

void notifySelftest(bool radioOk, bool displayOk, uint16_t batteryMv, uint32_t heapFree, uint32_t cmdId) {
  JsonDocument doc;
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
  emitJsonDoc(doc);
}

void notifyVoice(const uint8_t* from, const uint8_t* data, size_t dataLen, uint32_t msgId) {
  if (!from || !data || dataLen == 0) return;
  if (!s_connected || Bluefruit.connected() == 0) return;
  const size_t totalChunks = (dataLen + BLE_VOICE_CHUNK_RAW_MAX - 1) / BLE_VOICE_CHUNK_RAW_MAX;
  char fromHex[17];
  nodeIdToHexBuf(from, fromHex);
  for (size_t i = 0; i < totalChunks; i++) {
    size_t off = i * BLE_VOICE_CHUNK_RAW_MAX;
    size_t chunkLen = dataLen - off;
    if (chunkLen > BLE_VOICE_CHUNK_RAW_MAX) chunkLen = BLE_VOICE_CHUNK_RAW_MAX;
    char b64[BLE_VOICE_CHUNK_B64_BUF + 8];
    if (sodium_base64_encoded_len(chunkLen, sodium_base64_VARIANT_ORIGINAL) > sizeof(b64)) break;
    sodium_bin2base64(b64, sizeof(b64), data + off, chunkLen, sodium_base64_VARIANT_ORIGINAL);
    JsonDocument doc;
    doc["evt"] = "voice";
    doc["from"] = fromHex;
    doc["chunk"] = (int)i;
    doc["total"] = (int)totalChunks;
    doc["data"] = b64;
    if (msgId != 0) doc["msgId"] = msgId;
    emitJsonDoc(doc);
    if (i + 1 < totalChunks) delay(4);
  }
}

void notifyError(const char* code, const char* msg) {
  if (!s_connected || Bluefruit.connected() == 0 || !code || !msg) return;
  char codeBuf[96];
  char msgBuf[256];
  strncpy(codeBuf, code, sizeof(codeBuf) - 1);
  codeBuf[sizeof(codeBuf) - 1] = '\0';
  strncpy(msgBuf, msg, sizeof(msgBuf) - 1);
  msgBuf[sizeof(msgBuf) - 1] = '\0';
  sanitizeBleJsonCStringInPlace(codeBuf);
  sanitizeBleJsonCStringInPlace(msgBuf);
  JsonDocument doc;
  doc["evt"] = "error";
  doc["code"] = codeBuf;
  doc["msg"] = msgBuf;
  if (s_activeCmdId != 0) doc["cmdId"] = s_activeCmdId;
  emitJsonDoc(doc);
}

bool isConnected() {
  return s_connected && Bluefruit.connected() > 0;
}

void processCommand(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;
  if (len >= kRxLineMax) {
    notifyPayloadTooLong((uint16_t)len);
    return;
  }
  char tmp[kRxLineMax];
  memcpy(tmp, data, len);
  tmp[len] = '\0';
  handleJsonCommand(tmp, len);
}

void getAdvertisingName(char* out, size_t outLen) {
  if (!out || outLen < 20) return;
  const uint8_t* id = node::getId();
  /* Паритет с `ble.cpp` (ESP): RL- + полный 8-байтовый node id (16 hex). */
  snprintf(out, outLen, "RL-%02X%02X%02X%02X%02X%02X%02X%02X", id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
}

uint32_t getPasskey() {
  return s_passkey;
}

void regeneratePasskey() {
  s_passkey = (uint32_t)(random(900000) + 100000);
  (void)riftlink_kv::setU32(KV_PIN, s_passkey);
}

}  // namespace ble
