/**
 * Async Tasks — packetTask, displayTask, radioSchedulerTask (RX + drain)
 */

#include "async_tasks.h"
#include "log.h"
#include "async_queues.h"
#include "ack_coalesce/ack_coalesce.h"
#include "send_overflow/send_overflow.h"
#include "ui/display.h"
#include "led/led.h"
#include "radio/radio.h"
#include "protocol/packet.h"
#include "node/node.h"
#include <freertos/semphr.h>
#include "crypto/crypto.h"
#include "powersave/powersave.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
/** xTaskCreateWithCaps объявлен в ESP-IDF здесь, не в esp_task.h (Arduino 3 / IDF 5.x). */
#if __has_include("freertos/idf_additions.h")
#include "freertos/idf_additions.h"
#define RIFTLINK_HAVE_TASK_CREATE_WITH_CAPS 1
#elif __has_include(<freertos/idf_additions.h>)
#include <freertos/idf_additions.h>
#define RIFTLINK_HAVE_TASK_CREATE_WITH_CAPS 1
#endif
#include <string.h>
#include <stdio.h>
#include <atomic>

extern void getNextRxSlotParams(uint8_t* sfOut, uint32_t* slotMsOut);

// Full FSM path is enabled by default. Define RADIO_FSM_V2_DEFAULT_OFF for temporary rollback.
#if defined(RADIO_FSM_V2_DEFAULT_OFF)
static constexpr bool kRadioFsmV2Default = false;
#else
static constexpr bool kRadioFsmV2Default = true;
#endif

static std::atomic<bool> s_radioFsmV2Enabled{kRadioFsmV2Default};

static inline void queueSendReason(char* buf, size_t buflen, const char* msg) {
  if (buf && buflen > 0) {
    snprintf(buf, buflen, "%s", msg ? msg : "?");
  }
}

struct TxAsymMeta {
  uint8_t opcode = 0xFF;
  uint16_t selfHash = 0;
  uint16_t peerHash = 0;
  uint8_t slot = 0;
  bool broadcast = false;
  TxRequestClass klass = TxRequestClass::data;
};

static const char* txAsymClassName(TxRequestClass c) {
  switch (c) {
    case TxRequestClass::critical: return "critical";
    case TxRequestClass::control: return "control";
    case TxRequestClass::data: return "data";
    case TxRequestClass::voice: return "voice";
    default: return "data";
  }
}

static TxRequestClass classifyOpcode(uint8_t op) {
  switch (op) {
    case protocol::OP_SOS:
      return TxRequestClass::critical;
    case protocol::OP_HELLO:
    case protocol::OP_KEY_EXCHANGE:
    case protocol::OP_POLL:
    case protocol::OP_NACK:
    case protocol::OP_ACK:
    case protocol::OP_ACK_BATCH:
    case protocol::OP_ROUTE_REQ:
    case protocol::OP_ROUTE_REPLY:
    case protocol::OP_PING:
    case protocol::OP_PONG:
    case protocol::OP_SF_BEACON:
      return TxRequestClass::control;
    case protocol::OP_VOICE_MSG:
    case protocol::OP_MSG_FRAG:
      return TxRequestClass::voice;
    default:
      return TxRequestClass::data;
  }
}

static uint16_t idHash16Of(const uint8_t* id) {
  if (!id) return 0;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < protocol::NODE_ID_LEN; i++) {
    h ^= id[i];
    h *= 16777619u;
  }
  return (uint16_t)((h >> 16) ^ (h & 0xFFFFu));
}

static bool decodeTxAsymMeta(const uint8_t* buf, size_t len, TxAsymMeta* out) {
  if (!buf || !out) return false;
  const uint8_t* self = node::getId();
  out->selfHash = idHash16Of(self);
  if (len < 3 || buf[0] != protocol::SYNC_BYTE) {
    out->slot = (uint8_t)(out->selfHash & 0x01);
    out->klass = TxRequestClass::data;
    return false;
  }
  uint8_t ver = buf[1];
  bool isStrict = (ver & 0xF0) == protocol::VERSION_STRICT || (ver & 0xF0) == protocol::VERSION_V2_PKTID;
  if (!isStrict) {
    out->slot = (uint8_t)(out->selfHash & 0x01);
    out->klass = TxRequestClass::data;
    return false;
  }
  bool hasPktId = (ver & 0xF0) == protocol::VERSION_V2_PKTID;
  bool isBroadcast = (ver & 0x01) != 0;
  out->broadcast = isBroadcast;
  out->opcode = buf[2];
  out->klass = classifyOpcode(out->opcode);
  if (isBroadcast) {
    size_t fromOff = hasPktId ? 5u : 3u;
    if (len >= fromOff + protocol::NODE_ID_LEN) {
      out->peerHash = idHash16Of(buf + fromOff);
    }
    out->slot = (uint8_t)(out->selfHash & 0x01);
    return true;
  }
  size_t toOff = hasPktId ? 13u : 11u;
  if (len >= toOff + protocol::NODE_ID_LEN) {
    out->peerHash = idHash16Of(buf + toOff);
    out->slot = (out->selfHash < out->peerHash) ? 0u : 1u;
  } else {
    out->slot = (uint8_t)(out->selfHash & 0x01);
  }
  return true;
}

static uint32_t computeTxAsymSkewMs(const uint8_t* buf, size_t len, TxAsymMeta* metaOut) {
  TxAsymMeta meta{};
  decodeTxAsymMeta(buf, len, &meta);
  if (metaOut) *metaOut = meta;
  struct Profile { uint8_t base; uint8_t step; uint8_t span; uint8_t cap; };
  Profile p{};
  switch (meta.klass) {
    case TxRequestClass::critical: p = {0, 6, 6, 12}; break;
    case TxRequestClass::control:  p = {4, 16, 14, 34}; break;
    case TxRequestClass::voice:    p = {1, 4, 4, 9}; break;
    case TxRequestClass::data:
    default:                    p = {8, 24, 22, 58}; break;
  }
  uint32_t rnd = (p.span > 0) ? (uint32_t)(esp_random() % p.span) : 0;
  uint32_t skew = (uint32_t)p.base + (uint32_t)meta.slot * (uint32_t)p.step + rnd;
  if (skew > p.cap) skew = p.cap;
  return skew;
}

#define DISPLAY_HEARTBEAT_MS 10000  // лог раз в 10 с — проверить, что displayTask жив

// OLED: после Wi-Fi + очередей (64/48) в internal heap не хватает места под стек 32K+8K+4K — xTaskCreate(packet) даёт FAIL.
// Watermark на V4 при 32K был ~31K «свободно» (фактически байты/запас) — 16K достаточно для handlePacket.
#if defined(USE_EINK)
#define PACKET_TASK_STACK 32768
#else
// Align OLED with a wide safety margin for deep packet/notify/log call chains.
#define PACKET_TASK_STACK 32768
#endif
#define DISPLAY_TASK_STACK 8192   // 12KB — create FAIL (heap), 8KB — минимум для создания
#define PACKET_TASK_PRIO 2
#define DISPLAY_TASK_PRIO 1
#if defined(USE_EINK)
#define PACKET_TASK_PRIO_EINK 3   // Paper: выше displayTask — e-ink блокирует, packetTask важнее
#else
#define PACKET_TASK_PRIO_EINK PACKET_TASK_PRIO
#endif

// Forward — реализация в main.cpp (async)
extern void handlePacket(const uint8_t* buf, size_t len, int rssi, uint8_t sf);
/** HELLO: только из планировщика (main.cpp). */
extern void mainDrainHelloFromScheduler(void);
/** Текущий mesh SF (main.cpp) — для powersave RX в radioSchedulerTask. */
extern uint8_t getDiscoverySf(void);
/** V4: после TX на SF12 — следующий RX слот на SF12. Вызывать из radioSchedulerTask. */
extern void onRadioSchedulerTxSf12(void);

// Очереди + таски: обычно поднимаются в boot (main runBootStateMachine); иначе — при первом queueSend.
static bool s_asyncInfraReady = false;
static SemaphoreHandle_t s_asyncInfraMux = nullptr;
static QueueHandle_t s_txRequestQueue = nullptr;

static constexpr size_t TX_REQUEST_QUEUE_LEN =
#if defined(USE_EINK)
    64;
#else
    64;
#endif

bool asyncInfraEnsure() {
  if (s_asyncInfraReady) return true;
  if (!s_asyncInfraMux) {
    s_asyncInfraMux = xSemaphoreCreateMutex();
    if (!s_asyncInfraMux) return false;
  }
  if (xSemaphoreTake(s_asyncInfraMux, portMAX_DELAY) != pdTRUE) return false;
  if (s_asyncInfraReady) {
    xSemaphoreGive(s_asyncInfraMux);
    return true;
  }
  if (!asyncQueuesInit()) {
    RIFTLINK_LOG_ERR("[RiftLink] Async queues init FAILED (lazy)\n");
    xSemaphoreGive(s_asyncInfraMux);
    return false;
  }
  if (!s_txRequestQueue) {
    s_txRequestQueue = xQueueCreate(TX_REQUEST_QUEUE_LEN, sizeof(TxRequest*));
  }
  if (!s_txRequestQueue) {
    RIFTLINK_LOG_ERR("[RiftLink] TX request queue init FAILED (lazy)\n");
    xSemaphoreGive(s_asyncInfraMux);
    return false;
  }
  asyncTasksStart();
  radio::setAsyncMode(true);
  displaySetButtonPolledExternally(true);
  s_asyncInfraReady = true;
  Serial.printf("[RiftLink] Async infra OK, heap=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  xSemaphoreGive(s_asyncInfraMux);
  return true;
}

#define ACK_RESERVE_SLOTS 4   // резерв для ACK — обычные пакеты отклоняются при ≤4 свободных
static bool queueSendInternal(const uint8_t* buf, size_t len, uint8_t txSf, bool priority,
    bool applyAsymSkew, char* reasonBuf, size_t reasonLen);
static bool queueTxRequestInternal(const TxRequest& req, char* reasonBuf, size_t reasonLen);

bool queueSend(const uint8_t* buf, size_t len, uint8_t txSf, bool priority,
    char* reasonBuf, size_t reasonLen) {
  return queueSendInternal(buf, len, txSf, priority, true, reasonBuf, reasonLen);
}

bool queueTxRequest(const TxRequest& req, char* reasonBuf, size_t reasonLen) {
  return queueTxRequestInternal(req, reasonBuf, reasonLen);
}

uint8_t asyncTxQueueFree() {
  if (!s_txRequestQueue) return 0;
  return (uint8_t)uxQueueSpacesAvailable(s_txRequestQueue);
}

uint8_t asyncTxQueueWaiting() {
  if (!s_txRequestQueue) return 0;
  return (uint8_t)uxQueueMessagesWaiting(s_txRequestQueue);
}

bool queueTxPacket(const uint8_t* buf, size_t len, uint8_t txSf, bool priority, TxRequestClass klass,
    char* reasonBuf, size_t reasonLen) {
  if (!buf || len == 0 || len > PACKET_BUF_SIZE) {
    queueSendReason(reasonBuf, reasonLen, "pkt_oversize");
    return false;
  }
  TxRequest req{};
  memcpy(req.buf, buf, len);
  req.len = (uint16_t)len;
  req.txSf = txSf;
  req.priority = priority;
  req.klass = klass;
  req.enqueueMs = millis();
  return queueTxRequestInternal(req, reasonBuf, reasonLen);
}

bool asyncIsRadioFsmV2Enabled() {
  return s_radioFsmV2Enabled.load(std::memory_order_relaxed);
}

void asyncSetRadioFsmV2Enabled(bool enabled) {
  s_radioFsmV2Enabled.store(enabled, std::memory_order_relaxed);
}

static bool queueTxRequestInternal(const TxRequest& inReq, char* reasonBuf, size_t reasonLen) {
  if (!asyncInfraEnsure()) {
    queueSendReason(reasonBuf, reasonLen, "async_infra");
    RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=async_infra priority=%u len=%u",
        (unsigned)inReq.priority, (unsigned)inReq.len);
    return false;
  }
  if (!s_txRequestQueue) {
    queueSendReason(reasonBuf, reasonLen, "no_txreq_queue");
    RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=no_txreq_queue priority=%u len=%u",
        (unsigned)inReq.priority, (unsigned)inReq.len);
    return false;
  }
  TxRequest req = inReq;
  // Project policy: TX always uses current modem SF from radio settings.
  // Any per-packet SF hints are ignored to keep deterministic fixed-SF behavior.
  req.txSf = radio::getSpreadingFactor();
  if (req.txSf < 7 || req.txSf > 12) req.txSf = 7;
  const bool fastLane = req.priority || req.klass == TxRequestClass::critical || req.klass == TxRequestClass::control;
  uint8_t congestion = radio::getCongestionLevel();
  uint8_t qWaiting = asyncTxQueueWaiting();
  uint8_t reserveSlots = ACK_RESERVE_SLOTS + (congestion >= 2 ? 2 : 0) + (qWaiting >= 32 ? 2 : 0);
  if (req.enqueueMs == 0) req.enqueueMs = millis();
  if (!asyncIsRadioFsmV2Enabled()) {
    if (!radioCmdQueue) {
      queueSendReason(reasonBuf, reasonLen, "no_send_queue");
      RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=no_send_queue priority=%u len=%u",
          (unsigned)req.priority, (unsigned)req.len);
      return false;
    }
    if (!fastLane && uxQueueSpacesAvailable(radioCmdQueue) <= reserveSlots) {
      if (send_overflow::push(req)) return true;
      queueSendReason(reasonBuf, reasonLen, "legacy_overflow_full");
      return false;
    }
    RadioCmd rcmd{};
    rcmd.type = RadioCmdType::Tx;
    rcmd.priority = req.priority;
    memcpy(rcmd.u.tx.buf, req.buf, req.len);
    rcmd.u.tx.len = req.len;
    rcmd.u.tx.txSf = req.txSf;
    BaseType_t ok = fastLane ? xQueueSendToFront(radioCmdQueue, &rcmd, 0)
                                 : xQueueSend(radioCmdQueue, &rcmd, 0);
    if (ok != pdTRUE) {
      if (send_overflow::push(req)) return true;
      queueSendReason(reasonBuf, reasonLen, "legacy_sendq_full");
      return false;
    }
    return true;
  }
  if (!fastLane && uxQueueSpacesAvailable(s_txRequestQueue) <= reserveSlots) {
    if (send_overflow::push(req)) {
      RIFTLINK_DIAG("QUEUE", "event=TX_DEFER lane=overflow cause=ack_reserve sf=%u len=%u free=%u",
          (unsigned)req.txSf, (unsigned)req.len, (unsigned)uxQueueSpacesAvailable(s_txRequestQueue));
      return true;
    }
    queueSendReason(reasonBuf, reasonLen, "overflow_norm_full");
    RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=overflow_norm_full sf=%u len=%u free=%u",
        (unsigned)req.txSf, (unsigned)req.len, (unsigned)uxQueueSpacesAvailable(s_txRequestQueue));
    return false;
  }
  TxRequest* slot = txRequestPool.alloc();
  if (!slot) {
    if (send_overflow::push(req)) {
      RIFTLINK_DIAG("QUEUE", "event=TX_DEFER lane=overflow cause=pool_empty priority=%u sf=%u len=%u",
          (unsigned)req.priority, (unsigned)req.txSf, (unsigned)req.len);
      return true;
    }
    queueSendReason(reasonBuf, reasonLen, "txreq_pool_empty");
    RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=txreq_pool_empty sf=%u len=%u",
        (unsigned)req.txSf, (unsigned)req.len);
    return false;
  }
  *slot = req;
  BaseType_t ok = fastLane ? xQueueSendToFront(s_txRequestQueue, &slot, 0)
                               : xQueueSend(s_txRequestQueue, &slot, 0);
  if (ok != pdTRUE) {
    txRequestPool.free(slot);
    if (send_overflow::push(req)) {
      RIFTLINK_DIAG("QUEUE", "event=TX_DEFER lane=overflow cause=txreq_busy priority=%u sf=%u len=%u",
          (unsigned)req.priority, (unsigned)req.txSf, (unsigned)req.len);
      return true;
    }
    queueSendReason(reasonBuf, reasonLen, req.priority ? "txreq_pri_ovfl_full" : "txreq_ovfl_full");
    RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=%s sf=%u len=%u",
        req.priority ? "txreq_pri_ovfl_full" : "txreq_ovfl_full", (unsigned)req.txSf, (unsigned)req.len);
    return false;
  }
  RIFTLINK_DIAG("QUEUE", "event=TX_ENQUEUE_OK priority=%u sf=%u len=%u free=%u",
      (unsigned)req.priority, (unsigned)req.txSf, (unsigned)req.len, (unsigned)uxQueueSpacesAvailable(s_txRequestQueue));
  return true;
}

static bool queueSendInternal(const uint8_t* buf, size_t len, uint8_t txSf, bool priority,
    bool applyAsymSkew, char* reasonBuf, size_t reasonLen) {
  if (!asyncInfraEnsure()) {
    queueSendReason(reasonBuf, reasonLen, "async_infra");
    RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=async_infra priority=%u len=%u",
        (unsigned)priority, (unsigned)len);
    return false;
  }
  if (len > PACKET_BUF_SIZE) {
    queueSendReason(reasonBuf, reasonLen, "pkt_oversize");
    RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=pkt_oversize priority=%u len=%u max=%u",
        (unsigned)priority, (unsigned)len, (unsigned)PACKET_BUF_SIZE);
    return false;
  }
  if (applyAsymSkew && buf && len >= 3) {
    TxAsymMeta meta{};
    uint32_t skewMs = computeTxAsymSkewMs(buf, len, &meta);
    if (skewMs > 0) {
      queueDeferredSend(buf, len, txSf, skewMs, false);
      RIFTLINK_DIAG("QUEUE", "event=TX_ASYM_APPLIED op=0x%02X class=%s slot=%u skew_ms=%lu source=queueSend",
          (unsigned)meta.opcode, txAsymClassName(meta.klass), (unsigned)meta.slot, (unsigned long)skewMs);
      queueSendReason(reasonBuf, reasonLen, "asym_defer");
      return true;
    }
  }
  TxRequest req{};
  memcpy(req.buf, buf, len);
  req.len = (uint16_t)len;
  req.txSf = txSf;
  req.priority = priority;
  req.enqueueMs = millis();
  req.klass = (buf && len >= 3 && buf[0] == protocol::SYNC_BYTE) ? classifyOpcode(buf[2]) : TxRequestClass::data;
  return queueTxRequestInternal(req, reasonBuf, reasonLen);
}

#define DEFERRED_ACK_SLOTS 8   // broadcast: несколько соседей шлют ACK почти одновременно
#define DEFERRED_SEND_SLOTS 32   // MSG copy2–3, broadcast 2–3, KEY/HELLO — при переполнении важен корректный klass у fallback
#define HEARD_RELAY_SIZE 8   // Managed flooding: отмена relay при услышанной ретрансляции
struct DeferredSlot {
  TxRequest req;
  uint32_t sendAfter;
  bool used;
  bool isRelay;   // relay: отменить если услышали ретрансляцию
  uint8_t relayFrom[protocol::NODE_ID_LEN];
  uint32_t relayHash;
};
struct HeardRelayEntry { uint8_t from[protocol::NODE_ID_LEN]; uint32_t hash; };
static HeardRelayEntry s_heardRelay[HEARD_RELAY_SIZE];
static uint8_t s_heardRelayIdx = 0;

static DeferredSlot s_deferredAck[DEFERRED_ACK_SLOTS];
static DeferredSlot s_deferredSend[DEFERRED_SEND_SLOTS];

static bool relayHeardCheckAndRemove(const uint8_t* from, uint32_t hash) {
  for (int i = 0; i < HEARD_RELAY_SIZE; i++) {
    if (memcmp(s_heardRelay[i].from, from, protocol::NODE_ID_LEN) == 0 && s_heardRelay[i].hash == hash) {
      s_heardRelay[i].hash = 0xFFFFFFFFU;
      return true;
    }
  }
  return false;
}

static void flushDeferredSlots(DeferredSlot* slots, int n) {
  if (!s_txRequestQueue) return;
  uint32_t now = millis();
  for (int i = 0; i < n; i++) {
    if (slots[i].used && slots[i].sendAfter <= now) {
      if (slots[i].isRelay && relayHeardCheckAndRemove(slots[i].relayFrom, slots[i].relayHash)) {
        slots[i].used = false;
        continue;
      }
      if (queueTxRequestInternal(slots[i].req, nullptr, 0)) {
        slots[i].used = false;
      }
    }
  }
}

void queueDeferredAck(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs, bool applyAsym) {
  (void)asyncInfraEnsure();
  TxAsymMeta meta{};
  uint32_t asymMs = applyAsym ? computeTxAsymSkewMs(pkt, len, &meta) : 0;
  uint32_t finalDelay = delayMs + asymMs;
  uint32_t sendAfter = millis() + finalDelay;
  for (int i = 0; i < DEFERRED_ACK_SLOTS; i++) {
    if (!s_deferredAck[i].used) {
      memset(&s_deferredAck[i].req, 0, sizeof(TxRequest));
      memcpy(s_deferredAck[i].req.buf, pkt, len);
      s_deferredAck[i].req.len = (uint16_t)len;
      s_deferredAck[i].req.txSf = (txSf >= 7 && txSf <= 12) ? txSf : 0;
      s_deferredAck[i].req.priority = true;
      s_deferredAck[i].req.klass = TxRequestClass::control;
      s_deferredAck[i].req.enqueueMs = millis();
      s_deferredAck[i].sendAfter = sendAfter;
      s_deferredAck[i].used = true;
      s_deferredAck[i].isRelay = false;
      if (applyAsym) {
        RIFTLINK_DIAG("QUEUE", "event=TX_ASYM_APPLIED op=0x%02X class=%s slot=%u skew_ms=%lu source=deferred_ack",
            (unsigned)meta.opcode, txAsymClassName(meta.klass), (unsigned)meta.slot, (unsigned long)asymMs);
      }
      return;
    }
  }
  (void)queueTxPacket(pkt, len, txSf, true, TxRequestClass::control, nullptr, 0);
}

void queueDeferredSend(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs, bool applyAsym) {
  (void)asyncInfraEnsure();
  TxAsymMeta meta{};
  uint32_t asymMs = applyAsym ? computeTxAsymSkewMs(pkt, len, &meta) : 0;
  uint32_t finalDelay = delayMs + asymMs;
  uint32_t sendAfter = millis() + finalDelay;
  for (int i = 0; i < DEFERRED_SEND_SLOTS; i++) {
    if (!s_deferredSend[i].used) {
      memset(&s_deferredSend[i].req, 0, sizeof(TxRequest));
      memcpy(s_deferredSend[i].req.buf, pkt, len);
      s_deferredSend[i].req.len = (uint16_t)len;
      s_deferredSend[i].req.txSf = (txSf >= 7 && txSf <= 12) ? txSf : 0;
      s_deferredSend[i].req.priority = true;
      s_deferredSend[i].req.klass = (pkt && len >= 3 && pkt[0] == protocol::SYNC_BYTE)
          ? classifyOpcode(pkt[2]) : TxRequestClass::data;
      s_deferredSend[i].req.enqueueMs = millis();
      s_deferredSend[i].sendAfter = sendAfter;
      s_deferredSend[i].used = true;
      s_deferredSend[i].isRelay = false;
      if (applyAsym) {
        RIFTLINK_DIAG("QUEUE", "event=TX_ASYM_APPLIED op=0x%02X class=%s slot=%u skew_ms=%lu source=deferred_send",
            (unsigned)meta.opcode, txAsymClassName(meta.klass), (unsigned)meta.slot, (unsigned long)asymMs);
      }
      return;
    }
  }
  TxRequestClass fallbackKlass = (pkt && len >= 3 && pkt[0] == protocol::SYNC_BYTE)
      ? classifyOpcode(pkt[2]) : TxRequestClass::data;
  if (!queueTxPacket(pkt, len, txSf, true, fallbackKlass, nullptr, 0)) {
    RIFTLINK_LOG_ERR("[RiftLink] deferred fallback txRequestQueue full, drop copy\n");
  }
}

void queueDeferredRelay(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs,
    const uint8_t* from, uint32_t payloadHash, bool applyAsym) {
  (void)asyncInfraEnsure();
  TxAsymMeta meta{};
  uint32_t asymMs = applyAsym ? computeTxAsymSkewMs(pkt, len, &meta) : 0;
  uint32_t finalDelay = delayMs + asymMs;
  uint32_t sendAfter = millis() + finalDelay;
  for (int i = 0; i < DEFERRED_SEND_SLOTS; i++) {
    if (!s_deferredSend[i].used) {
      memset(&s_deferredSend[i].req, 0, sizeof(TxRequest));
      memcpy(s_deferredSend[i].req.buf, pkt, len);
      s_deferredSend[i].req.len = (uint16_t)len;
      s_deferredSend[i].req.txSf = (txSf >= 7 && txSf <= 12) ? txSf : 0;
      s_deferredSend[i].req.priority = true;
      s_deferredSend[i].req.klass = (pkt && len >= 3 && pkt[0] == protocol::SYNC_BYTE)
          ? classifyOpcode(pkt[2]) : TxRequestClass::data;
      s_deferredSend[i].req.enqueueMs = millis();
      s_deferredSend[i].sendAfter = sendAfter;
      s_deferredSend[i].used = true;
      s_deferredSend[i].isRelay = true;
      memcpy(s_deferredSend[i].relayFrom, from, protocol::NODE_ID_LEN);
      s_deferredSend[i].relayHash = payloadHash;
      if (applyAsym) {
        RIFTLINK_DIAG("QUEUE", "event=TX_ASYM_APPLIED op=0x%02X class=%s slot=%u skew_ms=%lu source=deferred_relay",
            (unsigned)meta.opcode, txAsymClassName(meta.klass), (unsigned)meta.slot, (unsigned long)asymMs);
      }
      return;
    }
  }
  TxRequestClass relayFallbackKlass = (pkt && len >= 3 && pkt[0] == protocol::SYNC_BYTE)
      ? classifyOpcode(pkt[2]) : TxRequestClass::data;
  if (!queueTxPacket(pkt, len, txSf, true, relayFallbackKlass, nullptr, 0)) {
    RIFTLINK_LOG_ERR("[RiftLink] deferred relay fallback txRequestQueue full, drop\n");
  }
}

void relayHeard(const uint8_t* from, uint32_t payloadHash) {
  memcpy(s_heardRelay[s_heardRelayIdx].from, from, protocol::NODE_ID_LEN);
  s_heardRelay[s_heardRelayIdx].hash = payloadHash;
  s_heardRelayIdx = (s_heardRelayIdx + 1) % HEARD_RELAY_SIZE;
}

void flushDeferredSends() {
  if (!s_asyncInfraReady) return;
  // Pull on-demand: radio scheduler тянет из send_overflow при пустой/недоступной radioCmdQueue для Tx
  // Сначала deferred send — потом ACK. SendToFront: последний добавленный впереди. ACK впереди = доставка.
  ack_coalesce::flush();
  flushDeferredSlots(s_deferredSend, DEFERRED_SEND_SLOTS);
  flushDeferredSlots(s_deferredAck, DEFERRED_ACK_SLOTS);
}

void queueDisplayLastMsg(const char* fromHex, const char* text) {
  (void)asyncInfraEnsure();
  if (!displayQueue) {
    displaySetLastMsg(fromHex, text);
    return;
  }
  DisplayQueueItem item = {};
  item.cmd = CMD_SET_LAST_MSG;
  item.screen = 4;
  if (fromHex) strncpy(item.fromHex, fromHex, 16);
  if (text) strncpy(item.text, text, 63);
  if (xQueueSend(displayQueue, &item, 0) != pdTRUE) {
    displaySetLastMsg(fromHex, text);  // fallback — не терять сообщение при переполнении
  }
}

void queueDisplayRedraw(uint8_t screen, bool priority) {
  (void)asyncInfraEnsure();
  if (!displayQueue) {
    displayShowScreen(screen);
    return;
  }
  DisplayQueueItem item = {};
  item.cmd = CMD_REDRAW_SCREEN;
  item.screen = screen;
  BaseType_t ok = priority ? xQueueSendToFront(displayQueue, &item, pdMS_TO_TICKS(200))
                           : xQueueSend(displayQueue, &item, pdMS_TO_TICKS(200));
  if (ok != pdTRUE) {
    static uint32_t s_lastFallbackLog = 0;
    uint32_t now = millis();
    if (now - s_lastFallbackLog >= 10000) {  // не спамить при OOM (heap ~5KB, queue=4)
      s_lastFallbackLog = now;
      Serial.println("[RiftLink] displayQueue full, fallback draw");
    }
    displayShowScreen(screen);  // fallback при переполнении очереди
  }
}

/** Только флаг s_needRedrawInfo — без очереди. Отрисовка в displayUpdate() при s_currentScreen==1 */
void queueDisplayRequestInfoRedraw() {
  displayRequestInfoRedraw();
}

void queueDisplayLongPress(uint8_t screen) {
  (void)asyncInfraEnsure();
  if (!displayQueue) {
    displayOnLongPress(screen);
    return;
  }
  DisplayQueueItem item = {};
  item.cmd = CMD_LONG_PRESS;
  item.screen = screen;
  if (xQueueSend(displayQueue, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
    static uint32_t s_lastLongPressFallbackLog = 0;
    uint32_t now = millis();
    if (now - s_lastLongPressFallbackLog >= 10000) {
      s_lastLongPressFallbackLog = now;
      Serial.println("[RiftLink] displayQueue full, fallback longPress");
    }
    displayOnLongPress(screen);  // fallback при переполнении очереди
  }
}

void queueDisplayLedBlink() {
  (void)asyncInfraEnsure();
  if (!displayQueue) {
    ledBlink(20);
    return;
  }
  DisplayQueueItem item = {};
  item.cmd = CMD_BLINK_LED;
  item.screen = 0;
  xQueueSend(displayQueue, &item, 0);
}

void queueDisplayWake() {
  (void)asyncInfraEnsure();
  if (!displayQueue) {
    displayWake();
    displayShowScreen(displayGetCurrentScreen());
    return;
  }
  DisplayQueueItem item = {};
  item.cmd = CMD_WAKE;
  item.screen = 0;
  if (xQueueSend(displayQueue, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
    displayWake();
    displayShowScreen(displayGetCurrentScreen());
  }
}

static void packetTask(void* arg) {
  for (;;) {
    PacketQueueItem* item = nullptr;
    if (xQueueReceive(packetQueue, &item, portMAX_DELAY) == pdTRUE && item) {
      handlePacket(item->buf, item->len, (int)item->rssi, item->sf);
      packetPool.free(item);
    }
  }
}

#define RX_ALIVE_LOG_INTERVAL_MS 120000  // 2 мин — диагностика
#define RADIO_SCHEDULER_STACK 4096
#define RADIO_SCHEDULER_PRIO 4

static TaskHandle_t s_packetTaskHandle = nullptr;
static TaskHandle_t s_displayTaskHandle = nullptr;
static TaskHandle_t s_radioSchedulerTaskHandle = nullptr;

TaskHandle_t asyncGetRadioSchedulerTaskHandle(void) {
  return s_radioSchedulerTaskHandle;
}

bool asyncHasPacketTask(void) {
  return s_packetTaskHandle != nullptr;
}

void asyncMemoryDiagLogStacks(void) {
  // uxTaskGetStackHighWaterMark: минимум неиспользованного стека за всё время. На ESP32-Arduino размер в xTaskCreate — байты;
  // возвращаемое значение на практике сопоставимо с байтами неиспользованного хвоста (см. alloc ниже). 0 = нет задачи / overflow.
  unsigned wd = s_displayTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(s_displayTaskHandle) : 0U;
  unsigned wp = s_packetTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(s_packetTaskHandle) : 0U;
  unsigned wr = s_radioSchedulerTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(s_radioSchedulerTaskHandle) : 0U;
  Serial.printf("[RiftLink] Task stack high-water (unused / alloc B): display=%u/%u packet=%u/%u radio=%u/%u\n",
      wd, (unsigned)DISPLAY_TASK_STACK, wp, (unsigned)PACKET_TASK_STACK, wr, (unsigned)RADIO_SCHEDULER_STACK);
  if (!s_packetTaskHandle) {
    RIFTLINK_LOG_ERR("[RiftLink] packetTask не создан — xTaskCreate FAIL? Нужен heap под стек %u B (см. PACKET_TASK_STACK)\n",
        (unsigned)PACKET_TASK_STACK);
  }
}

#if defined(USE_EINK)
static SemaphoreHandle_t s_displaySpiGranted = nullptr;
static SemaphoreHandle_t s_displaySpiDone = nullptr;
static std::atomic<bool> s_displaySpiRequested{false};
static std::atomic<bool> s_displaySpiSessionActive{false};
static std::atomic<uint32_t> s_displaySpiRetryNotBeforeMs{0};
static std::atomic<uint32_t> s_displaySpiSessionStartMs{0};
static std::atomic<uint32_t> s_displayGrantCount{0};
static std::atomic<uint32_t> s_displayDoneCount{0};
static std::atomic<uint32_t> s_displaySkipBusyCount{0};
static std::atomic<uint32_t> s_displayForceDoneCount{0};

bool asyncRequestDisplaySpiSession(TickType_t timeoutTicks) {
  if (!s_displaySpiGranted || !s_displaySpiDone) {
    return false;  // вызывающий делает один takeMutex (legacy)
  }
  s_displaySpiRequested.store(true, std::memory_order_release);
  TaskHandle_t h = s_radioSchedulerTaskHandle;
  if (h) xTaskNotifyGive(h);
  if (xSemaphoreTake(s_displaySpiGranted, timeoutTicks) != pdTRUE) {
    s_displaySpiRequested.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

void asyncSignalDisplaySpiSessionDone(void) {
  if (s_displaySpiDone) xSemaphoreGive(s_displaySpiDone);
}
#endif

static TickType_t s_packetRxDropLogTick = 0;
static std::atomic<uint32_t> s_rxIrqGapSuppressed{0};
static std::atomic<uint32_t> s_rxHealthRearmCount{0};

enum class RadioFsmState : uint8_t {
  Idle = 0,
  RXListen,
  TXCommit,
  DisplayHold,
  Recovery,
};

static constexpr uint32_t FSM_STATE_LOG_INTERVAL_MS = 3000;
static constexpr uint32_t FSM_PREEMPT_LOG_INTERVAL_MS = 5000;
static constexpr uint32_t FSM_CONT_RX_TIMEOUT_MS = 30000;
static constexpr uint32_t FSM_IDLE_POLL_MS = 20;
static constexpr uint32_t FSM_RX_DUP_GUARD_MS = 120;
static constexpr uint32_t FSM_RX_IRQ_MIN_GAP_MS = 35;
static constexpr uint32_t FSM_TX_PREEMPT_BUDGET_MS = 1800;
static constexpr uint32_t FSM_RX_DUP_LOG_INTERVAL_MS = 500;
static constexpr uint8_t FSM_RX_DUP_REARM_THRESHOLD = 6;
static constexpr uint32_t FSM_RX_IRQ_SILENCE_REARM_MS = 5000;
static constexpr uint32_t FSM_DISPLAY_HOLD_MAX_MS = 1500;
static constexpr bool FSM_STATE_HEARTBEAT_LOG_ENABLED = false;

static inline bool shouldLogRxDone(uint8_t op) {
  // Reduce high-frequency service noise while keeping data/control visibility.
  return !(op == protocol::OP_ECHO ||
           op == protocol::OP_HELLO ||
           op == protocol::OP_POLL ||
           op == protocol::OP_SF_BEACON);
}

static inline uint32_t computeRxIrqGapGuardMs() {
  // Physics-based guard: minimal on-air time for smallest valid broadcast frame on current modem.
  // This avoids stale FIFO re-reads while still allowing valid packets on fast modem presets.
  uint32_t toaUs = radio::getTimeOnAir(protocol::HEADER_LEN_BROADCAST);
  uint32_t guardMs = toaUs ? ((toaUs + 999) / 1000) : FSM_RX_IRQ_MIN_GAP_MS;
  if (guardMs < 8) guardMs = 8;
  if (guardMs > 120) guardMs = 120;
  return guardMs;
}

static const char* fsmStateName(RadioFsmState s) {
  switch (s) {
    case RadioFsmState::Idle: return "IDLE";
    case RadioFsmState::RXListen: return "RX_LISTEN";
    case RadioFsmState::TXCommit: return "TX_COMMIT";
    case RadioFsmState::DisplayHold: return "DISPLAY_HOLD";
    case RadioFsmState::Recovery: return "RECOVERY";
    default: return "IDLE";
  }
}

static inline void fsmTransition(RadioFsmState* state, RadioFsmState next, const char* reason) {
  if (!state || *state == next) return;
  static uint32_t s_lastRoutineTransitionLogMs = 0;
  const char* why = reason ? reason : "-";
  bool routine = (strcmp(why, "tx_drain") == 0) ||
      (strcmp(why, "rx_window") == 0) ||
      (strcmp(why, "tx_pending_preempt") == 0) ||
      (strcmp(why, "tx_done_resume_rx") == 0);
  uint32_t now = millis();
  if (!routine || (now - s_lastRoutineTransitionLogMs) >= FSM_STATE_LOG_INTERVAL_MS) {
    RIFTLINK_DIAG("FSM", "event=FSM_TRANSITION from=%s to=%s reason=%s",
        fsmStateName(*state), fsmStateName(next), why);
    if (routine) s_lastRoutineTransitionLogMs = now;
  }
  *state = next;
}

static uint32_t simpleRxFingerprint(const uint8_t* buf, int len, int /*rssi*/) {
  if (!buf || len <= 0) return 0;
  uint32_t h = 2166136261u;
  int sample = (len < 48) ? len : 48;
  for (int i = 0; i < sample; i++) {
    h ^= (uint32_t)buf[i];
    h *= 16777619u;
  }
  h ^= (uint32_t)(len & 0xFFFF);
  h *= 16777619u;
  return h;
}

/** Доставка RX из планировщика в packetTask или прямой handlePacket (общая логика normal + powersave RX). */
static void deliverRxToPacketQueue(uint8_t* rxBuf, int n, int rssi, uint8_t sf) {
  if (n <= 0) return;
  uint8_t op = (n > 2 && rxBuf[0] == protocol::SYNC_BYTE) ? rxBuf[2] : 0xFF;
  RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_rx_queue action=enqueue len=%u rssi=%d sf=%u op=0x%02X",
      (unsigned)n, rssi, (unsigned)sf, (unsigned)op);
  if (packetQueue && packetPool.ready()) {
    if ((size_t)n <= PACKET_BUF_SIZE) {
      PacketQueueItem* slot = packetPool.alloc();
      if (!slot) {
        bool isHello = (n == 13 && rxBuf[0] == protocol::SYNC_BYTE && rxBuf[2] == protocol::OP_HELLO);
        if (isHello) {
          PacketQueueItem* discarded = nullptr;
          if (xQueueReceive(packetQueue, &discarded, 0) == pdTRUE) {
            bool frontWasHello = (discarded->len == 13 && discarded->buf[0] == protocol::SYNC_BYTE &&
                discarded->buf[2] == protocol::OP_HELLO);
            if (!frontWasHello) {
              packetPool.free(discarded);
              slot = packetPool.alloc();
            } else {
              (void)xQueueSendToFront(packetQueue, &discarded, 0);
            }
          }
        }
      }
      if (slot) {
        memcpy(slot->buf, rxBuf, (size_t)n);
        slot->len = (uint16_t)n;
        slot->rssi = (int8_t)rssi;
        slot->sf = sf;
        bool isHello = (n == 13 && rxBuf[0] == protocol::SYNC_BYTE && rxBuf[2] == protocol::OP_HELLO);
        BaseType_t ok = isHello ? xQueueSendToFront(packetQueue, &slot, pdMS_TO_TICKS(30))
                                : xQueueSend(packetQueue, &slot, pdMS_TO_TICKS(30));
        if (ok != pdTRUE) {
          packetPool.free(slot);
          TickType_t t = xTaskGetTickCount();
          if (t - s_packetRxDropLogTick >= pdMS_TO_TICKS(5000)) {
            s_packetRxDropLogTick = t;
            RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_rx_queue action=drop reason=queue_full queue=packetQueue len=%u rssi=%d sf=%u op=0x%02X",
                (unsigned)n, rssi, (unsigned)sf, (unsigned)op);
            RIFTLINK_DIAG("QUEUE", "event=RX_DROP queue=packetQueue len=%u rssi=%d sf=%u op=0x%02X",
                (unsigned)n, rssi, (unsigned)sf, (unsigned)op);
            RIFTLINK_LOG_ERR("[RiftLink] packetQueue full, drop\n");
          }
        }
      } else {
        TickType_t t = xTaskGetTickCount();
        if (t - s_packetRxDropLogTick >= pdMS_TO_TICKS(5000)) {
          s_packetRxDropLogTick = t;
          RIFTLINK_DIAG("QUEUE", "event=RX_DROP queue=packetQueue reason=pool_empty len=%u rssi=%d sf=%u op=0x%02X",
              (unsigned)n, rssi, (unsigned)sf, (unsigned)op);
        }
      }
    }
  } else {
    RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_rx_queue action=direct reason=no_packet_task len=%u rssi=%d sf=%u",
        (unsigned)n, rssi, (unsigned)sf);
    handlePacket(rxBuf, (size_t)n, rssi, sf);
  }
}

/** Push a TxRequest back to the front of the pointer-based s_txRequestQueue. */
static bool pushBackTxRequest(const TxRequest& item) {
  TxRequest* slot = txRequestPool.alloc();
  if (!slot) return false;
  *slot = item;
  if (xQueueSendToFront(s_txRequestQueue, &slot, 0) != pdTRUE) {
    txRequestPool.free(slot);
    return false;
  }
  return true;
}

/** Radio scheduler: один владелец радио, чередует RX и TX. Нет конкуренции drain vs rx. */
static void radioSchedulerTask(void* arg) {
  static uint8_t rxBuf[protocol::SYNC_LEN + protocol::HEADER_LEN + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  static uint32_t lastRxAliveLog = 0;
  static uint32_t rxOkCount = 0;
  static uint32_t rxTimeoutCount = 0;
  static uint32_t rxErrCount = 0;
  static uint32_t rxMutexTimeoutCount = 0;
  static uint32_t fsmRecoveryCount = 0;
  static uint32_t txWaitToAirMaxMs = 0;
  static uint32_t rxEventToHandleMaxMs = 0;
  static uint32_t lastFsmStateLogMs = 0;
  static uint32_t lastFsmPreemptLogMs = 0;
  static uint32_t lastContRxArmLogMs = 0;
  static uint32_t lastRxFingerprint = 0;
  static uint32_t lastRxFingerprintMs = 0;
  static uint32_t lastRxDupLogMs = 0;
  static uint32_t rxDupWindowStartMs = 0;
  static uint8_t rxDupBurstCount = 0;
  static uint32_t lastRxIrqHandleMs = 0;
  static uint32_t lastRxHealthRearmMs = 0;
  static uint32_t lastRxGapSuppressedLogMs = 0;
  static bool contRxArmed = false;
  static RadioFsmState lastFsmStateLogged = RadioFsmState::Idle;
  RadioFsmState fsmState = RadioFsmState::Idle;
#if !defined(SF_FORCE_7)
  static int highSfDrained = 0;  // счётчик подряд отправленных SF10+ — макс 2, иначе RX не успевает
#endif
  vTaskDelay(pdMS_TO_TICKS(500));  // дать setup завершиться
  uint32_t lastQueueDiagMs = 0;
  for (;;) {
    bool useFsmV2 = asyncIsRadioFsmV2Enabled();
    if (useFsmV2) {
      uint32_t now = millis();
      if (fsmState != lastFsmStateLogged ||
          (FSM_STATE_HEARTBEAT_LOG_ENABLED && (now - lastFsmStateLogMs) >= FSM_STATE_LOG_INTERVAL_MS)) {
        RIFTLINK_DIAG("FSM", "event=FSM_STATE state=%s", fsmStateName(fsmState));
        lastFsmStateLogged = fsmState;
        lastFsmStateLogMs = now;
      }
    }
    uint32_t nowLoop = millis();
    if (nowLoop - lastQueueDiagMs >= 30000) {
      lastQueueDiagMs = nowLoop;
      UBaseType_t txFree = s_txRequestQueue ? uxQueueSpacesAvailable(s_txRequestQueue) : 0;
      UBaseType_t txUsed = s_txRequestQueue ? uxQueueMessagesWaiting(s_txRequestQueue) : 0;
      UBaseType_t rxFree = packetQueue ? uxQueueSpacesAvailable(packetQueue) : 0;
      UBaseType_t rxUsed = packetQueue ? uxQueueMessagesWaiting(packetQueue) : 0;
      RIFTLINK_DIAG("QUEUE", "event=QUEUE_SNAPSHOT tx_waiting=%u tx_free=%u rx_waiting=%u rx_free=%u heap=%u",
          (unsigned)txUsed, (unsigned)txFree, (unsigned)rxUsed, (unsigned)rxFree,
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      RIFTLINK_DIAG("RADIO", "event=RX_STATS ok=%lu timeout=%lu err=%lu mutex_timeout=%lu",
          (unsigned long)rxOkCount, (unsigned long)rxTimeoutCount,
          (unsigned long)rxErrCount, (unsigned long)rxMutexTimeoutCount);
      uint32_t oversizeDrops = 0;
      uint32_t shortReads = 0;
      uint32_t readErrors = 0;
      radio::getRxDiagCounters(&oversizeDrops, &shortReads, &readErrors);
      RIFTLINK_DIAG("RADIO", "event=RX_CHAIN_STATS oversize_drop=%lu short_read=%lu read_error=%lu irq_gap_suppressed=%lu",
          (unsigned long)oversizeDrops, (unsigned long)shortReads, (unsigned long)readErrors,
          (unsigned long)s_rxIrqGapSuppressed.load(std::memory_order_relaxed));
      if (useFsmV2) {
        RIFTLINK_DIAG("FSM", "event=FSM_STATS recovery=%lu tx_wait_to_air_max_ms=%lu rx_event_to_handle_max_ms=%lu health_rearm=%lu",
            (unsigned long)fsmRecoveryCount, (unsigned long)txWaitToAirMaxMs, (unsigned long)rxEventToHandleMaxMs,
            (unsigned long)s_rxHealthRearmCount.load(std::memory_order_relaxed));
#if defined(USE_EINK)
        RIFTLINK_DIAG("FSM", "event=FSM_HOLD_STATS requested=%u active=%u grant=%lu done=%lu skip_busy=%lu force_done=%lu",
            (unsigned)s_displaySpiRequested.load(std::memory_order_relaxed),
            (unsigned)s_displaySpiSessionActive.load(std::memory_order_relaxed),
            (unsigned long)s_displayGrantCount.load(std::memory_order_relaxed),
            (unsigned long)s_displayDoneCount.load(std::memory_order_relaxed),
            (unsigned long)s_displaySkipBusyCount.load(std::memory_order_relaxed),
            (unsigned long)s_displayForceDoneCount.load(std::memory_order_relaxed));
#endif
      }
    }
    flushDeferredSends();
    mainDrainHelloFromScheduler();
#if defined(USE_EINK)
    bool displaySessionActive = s_displaySpiSessionActive.load(std::memory_order_acquire);
    uint32_t nowDisplay = millis();
    // Non-blocking DISPLAY_HOLD arbitration:
    // request -> grant (when mutex available) -> done (poll semaphore), without blocking scheduler task.
    if (s_displaySpiGranted && s_displaySpiDone) {
      if (!displaySessionActive && s_displaySpiRequested.load(std::memory_order_acquire)) {
        uint32_t notBefore = s_displaySpiRetryNotBeforeMs.load(std::memory_order_relaxed);
        if ((int32_t)(nowDisplay - notBefore) >= 0) {
          if (radio::takeMutex(pdMS_TO_TICKS(20)) == pdTRUE) {
            // Clear stale done token from previous sessions.
            while (xSemaphoreTake(s_displaySpiDone, 0) == pdTRUE) {}
            s_displaySpiRequested.store(false, std::memory_order_release);
            send_overflow::drainApplyCommandsFromRadioQueue();
            radio::setRxListenActive(false);
            radio::standbyChipUnderMutex();
            radio::releaseMutex();
            s_displaySpiSessionActive.store(true, std::memory_order_release);
            s_displaySpiSessionStartMs.store(nowDisplay, std::memory_order_relaxed);
            s_displayGrantCount.fetch_add(1, std::memory_order_relaxed);
            if (useFsmV2) fsmTransition(&fsmState, RadioFsmState::DisplayHold, "display_grant");
            xSemaphoreGive(s_displaySpiGranted);
            displaySessionActive = true;
          } else {
            s_displaySpiRetryNotBeforeMs.store(nowDisplay + 40, std::memory_order_relaxed);
            s_displaySkipBusyCount.fetch_add(1, std::memory_order_relaxed);
            if (useFsmV2) {
              static uint32_t s_lastDisplaySkipLogMs = 0;
              if (nowDisplay - s_lastDisplaySkipLogMs >= 1000) {
                s_lastDisplaySkipLogMs = nowDisplay;
                RIFTLINK_DIAG("FSM", "event=FSM_HOLD_SKIPPED reason=mutex_busy");
              }
            }
          }
        }
      }
      if (displaySessionActive && xSemaphoreTake(s_displaySpiDone, 0) == pdTRUE) {
        s_displaySpiSessionActive.store(false, std::memory_order_release);
        s_displaySpiSessionStartMs.store(0, std::memory_order_relaxed);
        s_displayDoneCount.fetch_add(1, std::memory_order_relaxed);
        if (useFsmV2) {
          RIFTLINK_DIAG("FSM", "event=FSM_HOLD source=display done=1");
          fsmTransition(&fsmState, RadioFsmState::RXListen, "display_done");
        }
        displaySessionActive = false;
      }
      if (displaySessionActive) {
        uint32_t startedMs = s_displaySpiSessionStartMs.load(std::memory_order_relaxed);
        uint32_t holdMs = startedMs ? (nowDisplay - startedMs) : 0;
        if (startedMs != 0 && holdMs > FSM_DISPLAY_HOLD_MAX_MS) {
          // Hard guard: recover radio scheduler if display path got stuck and never sends done.
          radio::setArbiterHold(false);
          s_displaySpiRequested.store(false, std::memory_order_release);
          s_displaySpiSessionActive.store(false, std::memory_order_release);
          s_displaySpiSessionStartMs.store(0, std::memory_order_relaxed);
          s_displayForceDoneCount.fetch_add(1, std::memory_order_relaxed);
          RIFTLINK_DIAG("FSM", "event=FSM_HOLD_FORCE_RELEASE hold_ms=%lu max_ms=%u",
              (unsigned long)holdMs, (unsigned)FSM_DISPLAY_HOLD_MAX_MS);
          if (useFsmV2) {
            fsmTransition(&fsmState, RadioFsmState::RXListen, "display_force_release");
          }
          displaySessionActive = false;
        }
      }
      if (displaySessionActive) {
        // Do not compete for radio mutex while display session owns SPI path.
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
    }
#endif
    if (powersave::canSleep()) {
      if (radio::takeMutex(pdMS_TO_TICKS(100)) == pdTRUE) {
        send_overflow::drainApplyCommandsFromRadioQueue();
        uint8_t dsf = getDiscoverySf();
        radio::applyHardwareSpreadingFactor(dsf);
        RIFTLINK_DIAG("RADIO", "event=RX_SLOT_START mode=powersave sf=%u slot_ms=%u",
            (unsigned)dsf, (unsigned)1000);
        radio::startReceiveWithTimeout(1000);
        radio::setRxListenActive(true);
        radio::releaseMutex();
        powersave::lightSleepWake();
        int n = 0;
        if (radio::takeMutex(pdMS_TO_TICKS(200)) == pdTRUE) {
          n = radio::receiveAsync(rxBuf, sizeof(rxBuf));
          radio::setRxListenActive(false);
          radio::releaseMutex();
          if (n > 0) {
            rxOkCount++;
            int rssi = radio::getLastRssi();
            uint8_t op = (n > 2 && rxBuf[0] == protocol::SYNC_BYTE) ? rxBuf[2] : 0xFF;
            RIFTLINK_DIAG("RADIO", "event=RX_RESULT mode=powersave result=ok len=%u rssi=%d sf=%u op=0x%02X",
                (unsigned)n, rssi, (unsigned)dsf, (unsigned)op);
            if (shouldLogRxDone(op)) {
              RIFTLINK_DIAG("RADIO", "event=RX_DONE len=%u rssi=%d sf=%u op=0x%02X",
                  (unsigned)n, rssi, (unsigned)dsf, (unsigned)op);
            }
            deliverRxToPacketQueue(rxBuf, n, rssi, dsf);
          } else if (n == 0) {
            rxTimeoutCount++;
            RIFTLINK_DIAG("RADIO", "event=RX_RESULT mode=powersave result=timeout sf=%u",
                (unsigned)dsf);
          } else {
            rxErrCount++;
            RIFTLINK_DIAG("RADIO", "event=RX_RESULT mode=powersave result=err sf=%u",
                (unsigned)dsf);
          }
          uint32_t nowPs = millis();
          if (nowPs - lastRxAliveLog >= RX_ALIVE_LOG_INTERVAL_MS) {
            lastRxAliveLog = nowPs;
            Serial.printf("[RiftLink] RX alive heap=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
          }
        } else {
          rxMutexTimeoutCount++;
          RIFTLINK_DIAG("RADIO", "event=RX_MUTEX_TIMEOUT mode=powersave stage=post_sleep");
          radio::setRxListenActive(false);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (useFsmV2) {
      #if defined(USE_EINK)
      if (s_displaySpiSessionActive.load(std::memory_order_acquire)) {
        if (fsmState != RadioFsmState::DisplayHold) {
          fsmTransition(&fsmState, RadioFsmState::DisplayHold, "display_active");
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      #endif
      // Continuous RX: держим RX включенным постоянно, preempt только под TX/display/recovery.
      if (!contRxArmed) {
        if (radio::takeMutex(pdMS_TO_TICKS(120)) == pdTRUE) {
          uint8_t sf; uint32_t _unusedSlotMs;
          getNextRxSlotParams(&sf, &_unusedSlotMs);
          radio::applyHardwareSpreadingFactor(sf);
          radio::standbyChipUnderMutex();
          if (radio::startReceiveWithTimeout(FSM_CONT_RX_TIMEOUT_MS)) {
            radio::setRxListenActive(true);
            contRxArmed = true;
            uint32_t now = millis();
            if (now - lastContRxArmLogMs >= 5000) {
              RIFTLINK_DIAG("RADIO", "event=RX_CONT_ARM sf=%u timeout_ms=%u",
                  (unsigned)sf, (unsigned)FSM_CONT_RX_TIMEOUT_MS);
              lastContRxArmLogMs = now;
            }
          }
          radio::releaseMutex();
        } else {
          rxMutexTimeoutCount++;
          fsmTransition(&fsmState, RadioFsmState::Recovery, "mutex_timeout_arm_cont_rx");
          fsmRecoveryCount++;
        }
      }

      uint32_t txWaiting = (uint32_t)(s_txRequestQueue ? uxQueueMessagesWaiting(s_txRequestQueue) : 0);
      if (txWaiting > 0) {
        fsmTransition(&fsmState, RadioFsmState::TXCommit, "tx_pending_preempt");
        if (radio::takeMutex(pdMS_TO_TICKS(200)) != pdTRUE) {
          rxMutexTimeoutCount++;
          fsmTransition(&fsmState, RadioFsmState::Recovery, "mutex_timeout_preempt_tx");
          fsmRecoveryCount++;
          vTaskDelay(pdMS_TO_TICKS(5));
          continue;
        }
        radio::setRxListenActive(false);
        radio::standbyChipUnderMutex();
        send_overflow::drainApplyCommandsFromRadioQueue();

        bool didTx = false;
        uint32_t lastToaUs = 0;
        uint32_t burstToaUs = 0;
        bool pushedBack = false;
        uint8_t congestion = radio::getCongestionLevel();
        int drainLimit = (congestion >= 3) ? 8 : 12;
        int dataBudget = (congestion >= 3) ? 3 : (congestion >= 2 ? 5 : 8);
        int dataDrained = 0;
        for (int drainIdx = 0; drainIdx < drainLimit; drainIdx++) {
          TxRequest item{};
          if (!send_overflow::getNextTxRequest(s_txRequestQueue, &item)) break;
          bool fastLaneItem = item.priority || item.klass == TxRequestClass::critical || item.klass == TxRequestClass::control;
          if (!fastLaneItem && dataDrained >= dataBudget) {
            item.priority = true;
            item.enqueueMs = millis();
            (void)pushBackTxRequest(item);
            pushedBack = true;
            break;
          }
          if (drainIdx > 0 && item.txSf >= 10) { drainLimit = drainIdx + 1; }
          if (drainIdx >= 2 && item.txSf >= 10) break;
#if !defined(SF_FORCE_7)
          if (item.txSf >= 10 && highSfDrained >= 2) {
            item.priority = true;
            item.enqueueMs = millis();
            (void)pushBackTxRequest(item);
            pushedBack = true;
            break;
          }
#endif
          if (item.txSf >= 7 && item.txSf <= 12) {
            radio::applyHardwareSpreadingFactor(item.txSf);
#if !defined(SF_FORCE_7)
            if (item.txSf >= 10) {
              highSfDrained++;
              onRadioSchedulerTxSf12();
            } else {
              highSfDrained = 0;
            }
#endif
          }
          uint32_t pktToaUs = radio::getTimeOnAir(item.len);
          if (didTx && (burstToaUs + pktToaUs) > (FSM_TX_PREEMPT_BUDGET_MS * 1000UL)) {
            item.priority = true;
            item.enqueueMs = millis();
            (void)pushBackTxRequest(item);
            RIFTLINK_DIAG("FSM", "event=FSM_PREEMPT reason=tx_burst_budget budget_ms=%u burst_ms=%lu",
                (unsigned)FSM_TX_PREEMPT_BUDGET_MS, (unsigned long)(burstToaUs / 1000UL));
            pushedBack = true;
            break;
          }
          uint32_t queuedMs = item.enqueueMs ? (millis() - item.enqueueMs) : 0;
          if (queuedMs > txWaitToAirMaxMs) txWaitToAirMaxMs = queuedMs;
          lastToaUs = pktToaUs;
          burstToaUs += pktToaUs;
          if (!radio::sendDirectInternal(item.buf, item.len)) {
            queueDeferredSend(item.buf, item.len, item.txSf, 80, item.priority);
            RIFTLINK_DIAG("RADIO", "event=TX_FAIL_REQUEUE len=%u sf=%u", (unsigned)item.len, (unsigned)item.txSf);
          }
          didTx = true;
          if (!fastLaneItem) dataDrained++;
          // TX/RX interleave: release mutex between packets, check for pending RX
          if (drainIdx + 1 < drainLimit) {
            radio::releaseMutex();
            vTaskDelay(pdMS_TO_TICKS(1));
            if (radio::consumeIrqEvent()) {
              if (radio::takeMutex(pdMS_TO_TICKS(50)) == pdTRUE) {
                uint8_t interBuf[protocol::SYNC_LEN + protocol::HEADER_LEN + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
                (void)radio::startReceiveWithTimeout(50);
                int rn = radio::receiveAsync(interBuf, sizeof(interBuf));
                if (rn > 0) {
                  int rrssi = radio::getLastRssi();
                  radio::releaseMutex();
                  deliverRxToPacketQueue(interBuf, rn, rrssi, radio::getSpreadingFactor());
                } else {
                  radio::releaseMutex();
                }
              }
            }
            if (radio::takeMutex(pdMS_TO_TICKS(200)) != pdTRUE) {
              rxMutexTimeoutCount++;
              break;
            }
            radio::standbyChipUnderMutex();
          }
        }
        if (didTx) {
          uint8_t rxSf; uint32_t _unusedSlotMs;
          getNextRxSlotParams(&rxSf, &_unusedSlotMs);
          radio::applyHardwareSpreadingFactor(rxSf);
          (void)radio::startReceiveWithTimeout(FSM_CONT_RX_TIMEOUT_MS);
          radio::setRxListenActive(true);
          contRxArmed = true;
        }
        radio::releaseMutex();

        if (didTx && lastToaUs > 0) {
          uint32_t toaMs = (lastToaUs + 999) / 1000;
          if (toaMs > 0 && toaMs < 500) vTaskDelay(pdMS_TO_TICKS(toaMs));
        }
#if !defined(SF_FORCE_7)
        if (!didTx) highSfDrained = 0;
#endif
        if (!didTx) {
          if (radio::takeMutex(pdMS_TO_TICKS(120)) == pdTRUE) {
            uint8_t sf; uint32_t _unusedSlotMs;
            getNextRxSlotParams(&sf, &_unusedSlotMs);
            radio::applyHardwareSpreadingFactor(sf);
            (void)radio::startReceiveWithTimeout(FSM_CONT_RX_TIMEOUT_MS);
            radio::setRxListenActive(true);
            contRxArmed = true;
            radio::releaseMutex();
          } else {
            contRxArmed = false;
          }
          fsmTransition(&fsmState, RadioFsmState::RXListen, "tx_none_resume_rx");
          vTaskDelay(pdMS_TO_TICKS(8));
          continue;
        }
        contRxArmed = false;
        fsmTransition(&fsmState, RadioFsmState::RXListen, "tx_done_resume_rx");
        continue;
      }

      int n = 0;
      int rssi = 0;
      uint8_t sfNow = radio::getSpreadingFactor();
      bool forceRxRearm = false;
      bool rxIrq = radio::consumeIrqEvent();
      uint32_t nowPoll = millis();
      if (rxIrq) {
        uint32_t gapGuardMs = computeRxIrqGapGuardMs();
        if ((nowPoll - lastRxIrqHandleMs) < gapGuardMs) {
          if (radio::takeMutex(pdMS_TO_TICKS(20)) == pdTRUE) {
            if (radio::isRxPacketReadyUnderMutex()) {
              lastRxIrqHandleMs = nowPoll;
            } else {
              rxIrq = false;
              s_rxIrqGapSuppressed.fetch_add(1, std::memory_order_relaxed);
            }
            radio::releaseMutex();
          } else {
            rxIrq = false;
            s_rxIrqGapSuppressed.fetch_add(1, std::memory_order_relaxed);
          }
          if (!rxIrq && (nowPoll - lastRxGapSuppressedLogMs) >= 1000) {
            lastRxGapSuppressedLogMs = nowPoll;
            RIFTLINK_DIAG("FSM", "event=FSM_RX_IRQ_SUPPRESS delta_ms=%lu guard_ms=%lu sf=%u",
                (unsigned long)(nowPoll - lastRxIrqHandleMs), (unsigned long)gapGuardMs, (unsigned)sfNow);
          }
        } else {
          lastRxIrqHandleMs = nowPoll;
        }
      }
      // Корневой анти-реплей: читаем RX только по DIO1 IRQ, не по periodic poll.
      if (rxIrq && radio::takeMutex(pdMS_TO_TICKS(50)) == pdTRUE) {
        if (radio::isRxPacketReadyUnderMutex()) {
          uint32_t rxHandleStart = millis();
          n = radio::readReceivedPacketUnderMutex(rxBuf, sizeof(rxBuf));
          rssi = radio::getLastRssi();
          radio::standbyChipUnderMutex();
          (void)radio::startReceiveWithTimeout(FSM_CONT_RX_TIMEOUT_MS);
          radio::setRxListenActive(true);
          contRxArmed = true;
          uint32_t rxLatency = millis() - rxHandleStart;
          if (rxLatency > rxEventToHandleMaxMs) rxEventToHandleMaxMs = rxLatency;
        }
        radio::releaseMutex();
      }
      if (n > 0) {
        uint8_t op = (n > 2 && rxBuf[0] == protocol::SYNC_BYTE) ? rxBuf[2] : 0xFF;
        if (shouldLogRxDone(op)) {
          RIFTLINK_DIAG("RADIO", "event=RX_CHAIN stage=scheduler_read delivered_len=%u rssi=%d sf=%u op=0x%02X",
              (unsigned)n, rssi, (unsigned)sfNow, (unsigned)op);
        }
        uint32_t nowRx = millis();
        uint32_t fp = simpleRxFingerprint(rxBuf, n, rssi);
        bool dupBurst = (fp == lastRxFingerprint) && ((nowRx - lastRxFingerprintMs) <= FSM_RX_DUP_GUARD_MS);
        lastRxFingerprint = fp;
        lastRxFingerprintMs = nowRx;
        bool selfFrame = false;
        if (n >= (int)protocol::HEADER_LEN_BROADCAST && rxBuf[0] == protocol::SYNC_BYTE) {
          uint8_t ver = rxBuf[1];
          bool hasPktId = ((ver & 0xF0) == protocol::VERSION_V2_PKTID);
          bool isBroadcast = (ver & 0x01) != 0;
          size_t fromOff = hasPktId ? 5u : 3u;
          if (isBroadcast && (size_t)n >= fromOff + protocol::NODE_ID_LEN) {
            selfFrame = node::isForMe(rxBuf + fromOff);
          }
        }
        if (dupBurst || selfFrame) {
          uint32_t nowDup = millis();
          if ((nowDup - rxDupWindowStartMs) > 250) {
            rxDupWindowStartMs = nowDup;
            rxDupBurstCount = 0;
          }
          if (rxDupBurstCount < 255) rxDupBurstCount++;
          if ((nowDup - lastRxDupLogMs) >= FSM_RX_DUP_LOG_INTERVAL_MS) {
            RIFTLINK_DIAG("RADIO", "event=RX_DROP_DUP cause=%s len=%u rssi=%d sf=%u op=0x%02X burst=%u",
                selfFrame ? "self_echo" : "dup_guard", (unsigned)n, rssi, (unsigned)sfNow, (unsigned)op,
                (unsigned)rxDupBurstCount);
            lastRxDupLogMs = nowDup;
          }
          if (rxDupBurstCount >= FSM_RX_DUP_REARM_THRESHOLD) {
            forceRxRearm = true;
          }
        } else {
          rxDupBurstCount = 0;
          rxOkCount++;
          if (shouldLogRxDone(op)) {
            RIFTLINK_DIAG("RADIO", "event=RX_DONE len=%u rssi=%d sf=%u op=0x%02X",
                (unsigned)n, rssi, (unsigned)sfNow, (unsigned)op);
          }
          deliverRxToPacketQueue(rxBuf, n, rssi, sfNow);
        }
      } else if (n < 0) {
        rxErrCount++;
        fsmTransition(&fsmState, RadioFsmState::Recovery, "rx_err_continuous");
        fsmRecoveryCount++;
        contRxArmed = false;
      } else {
        vTaskDelay(pdMS_TO_TICKS(FSM_IDLE_POLL_MS));
      }

      if (forceRxRearm) {
        if (radio::takeMutex(pdMS_TO_TICKS(80)) == pdTRUE) {
          radio::setRxListenActive(false);
          radio::standbyChipUnderMutex();
          (void)radio::startReceiveWithTimeout(FSM_CONT_RX_TIMEOUT_MS);
          radio::setRxListenActive(true);
          radio::releaseMutex();
          contRxArmed = true;
          rxDupBurstCount = 0;
          RIFTLINK_DIAG("FSM", "event=FSM_RECOVERY action=rx_dup_rearm");
        } else {
          contRxArmed = false;
        }
      }

      // Health rearm: если долго нет IRQ (шум/пропущенный edge), перевооружаем RX
      // без чтения FIFO, чтобы не порождать ложные RX_DONE.
      if (!rxIrq && (nowPoll - lastRxIrqHandleMs) >= FSM_RX_IRQ_SILENCE_REARM_MS &&
          (nowPoll - lastRxHealthRearmMs) >= FSM_RX_IRQ_SILENCE_REARM_MS) {
        if (radio::takeMutex(pdMS_TO_TICKS(80)) == pdTRUE) {
          radio::setRxListenActive(false);
          radio::standbyChipUnderMutex();
          (void)radio::startReceiveWithTimeout(FSM_CONT_RX_TIMEOUT_MS);
          radio::setRxListenActive(true);
          radio::releaseMutex();
          contRxArmed = true;
          lastRxHealthRearmMs = nowPoll;
          s_rxHealthRearmCount.fetch_add(1, std::memory_order_relaxed);
          RIFTLINK_DIAG("FSM", "event=FSM_RECOVERY action=rx_irq_silence_rearm total=%lu",
              (unsigned long)s_rxHealthRearmCount.load(std::memory_order_relaxed));
        }
      }

      if (fsmState == RadioFsmState::Recovery) {
        if (radio::takeMutex(pdMS_TO_TICKS(60)) == pdTRUE) {
          radio::setRxListenActive(false);
          radio::standbyChipUnderMutex();
          radio::releaseMutex();
        }
        RIFTLINK_DIAG("FSM", "event=FSM_RECOVERY action=cont_rx_rearm");
        contRxArmed = false;
        fsmTransition(&fsmState, RadioFsmState::RXListen, "recovered");
      }
      continue;
    }
    if (radio::takeMutex(pdMS_TO_TICKS(200)) != pdTRUE) {
      rxMutexTimeoutCount++;
      RIFTLINK_DIAG("RADIO", "event=RX_MUTEX_TIMEOUT mode=normal stage=pre_tx_rx");
      if (useFsmV2) {
        fsmTransition(&fsmState, RadioFsmState::Recovery, "mutex_timeout_pre_txrx");
        fsmRecoveryCount++;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    send_overflow::drainApplyCommandsFromRadioQueue();
    bool didTx = false;
    uint32_t lastToaUs = 0;
    uint32_t burstToaUs = 0;
    bool pushedBack = false;
    uint8_t congestion = radio::getCongestionLevel();
    uint8_t curSf = radio::getSpreadingFactor();
    int drainLimit = (curSf >= 10) ? 3 : (congestion >= 3 ? 8 : 14);
    int dataBudget = (congestion >= 3) ? 3 : (congestion >= 2 ? 6 : 10);
    int dataDrained = 0;
    if (useFsmV2) fsmTransition(&fsmState, RadioFsmState::TXCommit, "tx_drain");
    for (int drainIdx = 0; drainIdx < drainLimit; drainIdx++) {
    TxRequest item{};
    if (!send_overflow::getNextTxRequest(s_txRequestQueue, &item)) break;
    bool fastLaneItem = item.priority || item.klass == TxRequestClass::critical || item.klass == TxRequestClass::control;
    if (!fastLaneItem && dataDrained >= dataBudget) {
      item.priority = true;
      item.enqueueMs = millis();
      (void)pushBackTxRequest(item);
      pushedBack = true;
      break;
    }
    if (drainIdx > 0 && item.txSf >= 10) { drainLimit = drainIdx + 1; }  // SF10+: только 1 доп. пакет
    if (drainIdx >= 2 && item.txSf >= 10) break;  // SF10+ после 2-го — стоп
#if defined(SF_FORCE_7)
        // KEY_EXCHANGE раньше откладывали на 0.5–2 с — на столе это давало поздний TX и лишние коллизии с HELLO.
        uint32_t queuedMs = item.enqueueMs ? (millis() - item.enqueueMs) : 0;
        if (queuedMs > txWaitToAirMaxMs) txWaitToAirMaxMs = queuedMs;
        lastToaUs = radio::getTimeOnAir(item.len);
        if (!radio::sendDirectInternal(item.buf, item.len)) {
          queueDeferredSend(item.buf, item.len, item.txSf, 80, item.priority);
          RIFTLINK_DIAG("RADIO", "event=TX_FAIL_REQUEUE len=%u sf=%u", (unsigned)item.len, (unsigned)item.txSf);
        }
        didTx = true;
        if (item.buf[0] == protocol::SYNC_BYTE && item.buf[2] == protocol::OP_MSG) {
          uint32_t msgGapMs = (lastToaUs > 0) ? ((lastToaUs / 1000 + 3) / 4) : 1;
          if (msgGapMs < 1) msgGapMs = 1;
          if (msgGapMs > 30) msgGapMs = 30;
          vTaskDelay(pdMS_TO_TICKS(msgGapMs));
        }
#else
        if (item.txSf >= 10 && highSfDrained >= 2) {
          item.priority = true;
          item.enqueueMs = millis();
          (void)pushBackTxRequest(item);
          pushedBack = true;
          break;
        } else {
          if (item.txSf >= 7 && item.txSf <= 12) {
            radio::applyHardwareSpreadingFactor(item.txSf);
            if (item.txSf >= 10) {
              highSfDrained++;
              onRadioSchedulerTxSf12();
            } else {
              highSfDrained = 0;
            }
          }
          uint32_t queuedMs = item.enqueueMs ? (millis() - item.enqueueMs) : 0;
          if (queuedMs > txWaitToAirMaxMs) txWaitToAirMaxMs = queuedMs;
          uint32_t pktToaUs = radio::getTimeOnAir(item.len);
          if (didTx && (burstToaUs + pktToaUs) > (FSM_TX_PREEMPT_BUDGET_MS * 1000UL)) {
            item.priority = true;
            item.enqueueMs = millis();
            (void)pushBackTxRequest(item);
            RIFTLINK_DIAG("FSM", "event=FSM_PREEMPT reason=tx_burst_budget budget_ms=%u burst_ms=%lu",
                (unsigned)FSM_TX_PREEMPT_BUDGET_MS, (unsigned long)(burstToaUs / 1000UL));
            pushedBack = true;
            break;
          }
          lastToaUs = pktToaUs;
          burstToaUs += pktToaUs;
          if (!radio::sendDirectInternal(item.buf, item.len)) {
            queueDeferredSend(item.buf, item.len, item.txSf, 80, item.priority);
            RIFTLINK_DIAG("RADIO", "event=TX_FAIL_REQUEUE len=%u sf=%u", (unsigned)item.len, (unsigned)item.txSf);
          }
          didTx = true;
          if (!fastLaneItem) dataDrained++;
          vTaskDelay(pdMS_TO_TICKS(1));
          if (item.buf[0] == protocol::SYNC_BYTE && item.buf[2] == protocol::OP_MSG) {
            uint32_t msgGapMs = (pktToaUs > 0) ? ((pktToaUs / 1000 + 3) / 4) : 1;
            if (msgGapMs < 1) msgGapMs = 1;
            if (msgGapMs > 30) msgGapMs = 30;
            vTaskDelay(pdMS_TO_TICKS(msgGapMs));
          }
        }
#endif
    }  // for drainIdx
    if (didTx) {
      uint8_t rxSfLeg; uint32_t _unusedSlotLeg;
      getNextRxSlotParams(&rxSfLeg, &_unusedSlotLeg);
      radio::applyHardwareSpreadingFactor(rxSfLeg);
      (void)radio::startReceiveWithTimeout(FSM_CONT_RX_TIMEOUT_MS);
      radio::setRxListenActive(true);
    }
    radio::releaseMutex();
    if (didTx && lastToaUs > 0) {
      uint32_t toaMs = (lastToaUs + 999) / 1000;
      if (toaMs > 0 && toaMs < 500) vTaskDelay(pdMS_TO_TICKS(toaMs));
    }
#if !defined(SF_FORCE_7)
    if (!didTx && !pushedBack) highSfDrained = 0;
#endif
    uint8_t sf;
    uint32_t slotMs;
    getNextRxSlotParams(&sf, &slotMs);
    if (didTx) {
      vTaskDelay(pdMS_TO_TICKS(slotMs > 220 ? 220 : slotMs));
      continue;
    }
    if (useFsmV2 && slotMs > 220) {
      slotMs = 220;
      uint32_t txWaiting = (uint32_t)(s_txRequestQueue ? uxQueueMessagesWaiting(s_txRequestQueue) : 0);
      uint32_t now = millis();
      if (txWaiting > 0 && (now - lastFsmPreemptLogMs) >= FSM_PREEMPT_LOG_INTERVAL_MS) {
        RIFTLINK_DIAG("FSM", "event=FSM_PREEMPT reason=rx_slice_cap slot_ms=%u tx_waiting=%u",
            (unsigned)slotMs, (unsigned)txWaiting);
        lastFsmPreemptLogMs = now;
      }
    }
    if (radio::takeMutex(pdMS_TO_TICKS(200)) == pdTRUE) {
      radio::applyHardwareSpreadingFactor(sf);
      if (useFsmV2) fsmTransition(&fsmState, RadioFsmState::RXListen, "rx_window");
      RIFTLINK_DIAG("RADIO", "event=RX_SLOT_START mode=normal sf=%u slot_ms=%u",
          (unsigned)sf, (unsigned)slotMs);
      radio::startReceiveWithTimeout(slotMs);
      radio::setRxListenActive(true);
      radio::releaseMutex();
    }
    vTaskDelay(pdMS_TO_TICKS(slotMs));
    if (radio::takeMutex(pdMS_TO_TICKS(800)) != pdTRUE) {
      rxMutexTimeoutCount++;
      RIFTLINK_DIAG("RADIO", "event=RX_MUTEX_TIMEOUT mode=normal stage=post_slot");
      if (useFsmV2) {
        fsmTransition(&fsmState, RadioFsmState::Recovery, "mutex_timeout_post_slot");
        fsmRecoveryCount++;
      }
      radio::setRxListenActive(false);
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    uint32_t rxHandleStart = millis();
    int n = radio::receiveAsync(rxBuf, sizeof(rxBuf));
    radio::setRxListenActive(false);
    radio::releaseMutex();
    uint32_t now = millis();
    if (now - lastRxAliveLog >= RX_ALIVE_LOG_INTERVAL_MS) {
      lastRxAliveLog = now;
      Serial.printf("[RiftLink] RX alive heap=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    if (n > 0) {
      rxOkCount++;
      int rssi = radio::getLastRssi();
      uint8_t op = (n > 2 && rxBuf[0] == protocol::SYNC_BYTE) ? rxBuf[2] : 0xFF;
      RIFTLINK_DIAG("RADIO", "event=RX_RESULT mode=normal result=ok len=%u rssi=%d sf=%u op=0x%02X",
          (unsigned)n, rssi, (unsigned)sf, (unsigned)op);
      if (shouldLogRxDone(op)) {
        RIFTLINK_DIAG("RADIO", "event=RX_DONE len=%u rssi=%d sf=%u op=0x%02X",
            (unsigned)n, rssi, (unsigned)sf, (unsigned)op);
      }
      deliverRxToPacketQueue(rxBuf, n, rssi, sf);
      uint32_t rxLatency = millis() - rxHandleStart;
      if (rxLatency > rxEventToHandleMaxMs) rxEventToHandleMaxMs = rxLatency;
    } else if (n == 0) {
      rxTimeoutCount++;
      RIFTLINK_DIAG("RADIO", "event=RX_RESULT mode=normal result=timeout sf=%u",
          (unsigned)sf);
    } else {
      rxErrCount++;
      RIFTLINK_DIAG("RADIO", "event=RX_RESULT mode=normal result=err sf=%u",
          (unsigned)sf);
      if (useFsmV2) {
        fsmTransition(&fsmState, RadioFsmState::Recovery, "rx_err");
        fsmRecoveryCount++;
      }
    }
    if (useFsmV2 && fsmState == RadioFsmState::Recovery) {
      if (radio::takeMutex(pdMS_TO_TICKS(50)) == pdTRUE) {
        radio::setRxListenActive(false);
        radio::standbyChipUnderMutex();
        radio::releaseMutex();
      }
      RIFTLINK_DIAG("FSM", "event=FSM_RECOVERY action=standby_reset");
      fsmTransition(&fsmState, RadioFsmState::RXListen, "recovered");
    }
  }
}

static void displayTask(void* arg) {
  vTaskDelay(pdMS_TO_TICKS(100));  // дать setup завершиться
  DisplayQueueItem item;
  uint32_t lastHeartbeat = millis();
  for (;;) {
    if (xQueueReceive(displayQueue, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
      switch (item.cmd) {
        case CMD_SET_LAST_MSG:
          displaySetLastMsg(item.fromHex, item.text);
          break;
        case CMD_REDRAW_SCREEN: {
          uint8_t lastScreen = item.screen;
          while (xQueuePeek(displayQueue, &item, 0) == pdTRUE && item.cmd == CMD_REDRAW_SCREEN) {
            xQueueReceive(displayQueue, &item, 0);
            lastScreen = item.screen;
          }
          displayShowScreen(lastScreen);
          break;
        }
        case CMD_REQUEST_INFO_REDRAW:
          displayRequestInfoRedraw();
          break;
        case CMD_LONG_PRESS:
          displayOnLongPress(item.screen);
          break;
        case CMD_WAKE:
          displayWake();
          displayShowScreen(displayGetCurrentScreen());
          break;
        case CMD_BLINK_LED:
          ledBlink(20);
          break;
      }
    }
    ledUpdate();
    displayUpdate();
    uint32_t now = millis();
    if (now - lastHeartbeat >= DISPLAY_HEARTBEAT_MS) {
      lastHeartbeat = now;
      unsigned wm = (unsigned)uxTaskGetStackHighWaterMark(nullptr);
      if (wm < 512) {
        Serial.printf("[displayTask] stack watermark LOW: %u B\n", wm);
      }
    }
  }
}

#if !defined(USE_EINK)
/**
 * OLED: packetTask — самый большой стек; при PSRAM сначала стек в SPIRAM (не ест contiguous internal),
 * иначе после BLE/Wi‑Fi часто largest < 16K и xTaskCreate падает.
 * Нужен CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY в sdkconfig (у Heltec V4 с PSRAM обычно включён).
 */
static BaseType_t createPacketTaskOled() {
#if defined(RIFTLINK_HAVE_TASK_CREATE_WITH_CAPS)
  const size_t psramTot = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (psramTot > 0) {
    BaseType_t ok = xTaskCreateWithCaps(packetTask, "packet", PACKET_TASK_STACK, nullptr,
        (UBaseType_t)PACKET_TASK_PRIO_EINK, &s_packetTaskHandle,
        (UBaseType_t)MALLOC_CAP_SPIRAM);
    if (ok == pdPASS) {
      Serial.printf("[RiftLink] packetTask: стек в SPIRAM (internal largest=%u)\n",
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      return ok;
    }
    Serial.printf("[RiftLink] packetTask: SPIRAM stack не удался — fallback internal (largest=%u)\n",
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }
#endif
  return xTaskCreate(packetTask, "packet", PACKET_TASK_STACK, nullptr, PACKET_TASK_PRIO_EINK, &s_packetTaskHandle);
}
#endif

#if defined(USE_EINK)
/** Paper: стек packetTask в SPIRAM, чтобы не съедать ~32K internal (как на OLED). Иначе после wifi::init free падает. */
static BaseType_t createPacketTaskPaper() {
#if defined(RIFTLINK_HAVE_TASK_CREATE_WITH_CAPS)
  const size_t psramTot = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (psramTot > 0) {
    BaseType_t ok = xTaskCreateWithCaps(packetTask, "packet", PACKET_TASK_STACK, nullptr,
        (UBaseType_t)PACKET_TASK_PRIO_EINK, &s_packetTaskHandle, (UBaseType_t)MALLOC_CAP_SPIRAM);
    if (ok == pdPASS) {
      Serial.printf("[RiftLink] packetTask (Paper): стек в SPIRAM, internal largest=%u\n",
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      return ok;
    }
    Serial.printf("[RiftLink] packetTask (Paper): SPIRAM stack не удался — fallback internal (largest=%u)\n",
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }
#endif
  return xTaskCreatePinnedToCore(packetTask, "packet", PACKET_TASK_STACK, nullptr, PACKET_TASK_PRIO_EINK,
      &s_packetTaskHandle, 1);
}
#endif

void asyncTasksStart() {
  static bool s_asyncTasksStarted = false;
  if (s_asyncTasksStarted) return;
#if defined(USE_EINK)
  if (!s_displaySpiGranted) s_displaySpiGranted = xSemaphoreCreateBinary();
  if (!s_displaySpiDone) s_displaySpiDone = xSemaphoreCreateBinary();
#endif
#if defined(USE_EINK)
  // Paper: сначала стек packet в SPIRAM (если есть) — освобождает ~32K internal; иначе fallback internal + core 1.
  BaseType_t okPacket = createPacketTaskPaper();
  if (okPacket == pdFAIL) {
    RIFTLINK_LOG_ERR("[RiftLink] packetTask create FAIL — loop будет обрабатывать packetQueue\n");
  }
  BaseType_t okDisplay = xTaskCreatePinnedToCore(displayTask, "display", DISPLAY_TASK_STACK, nullptr, DISPLAY_TASK_PRIO,
      &s_displayTaskHandle, 0);
#else
  BaseType_t okPacket = createPacketTaskOled();
  if (okPacket == pdFAIL) {
    RIFTLINK_LOG_ERR("[RiftLink] packetTask create FAIL (need %u) — loop будет обрабатывать packetQueue\n",
        (unsigned)PACKET_TASK_STACK);
  }
  BaseType_t okDisplay = xTaskCreate(displayTask, "display", DISPLAY_TASK_STACK, nullptr, DISPLAY_TASK_PRIO, &s_displayTaskHandle);
#endif
  if (okDisplay == pdFAIL) {
    RIFTLINK_LOG_ERR("[RiftLink] displayTask create FAIL (need %u)\n", (unsigned)DISPLAY_TASK_STACK);
  }
#if defined(USE_EINK)
  BaseType_t okRx = xTaskCreatePinnedToCore(radioSchedulerTask, "radio", RADIO_SCHEDULER_STACK, nullptr, RADIO_SCHEDULER_PRIO,
      &s_radioSchedulerTaskHandle, 1);
#else
  BaseType_t okRx = xTaskCreate(radioSchedulerTask, "radio", RADIO_SCHEDULER_STACK, nullptr, RADIO_SCHEDULER_PRIO,
      &s_radioSchedulerTaskHandle);
#endif
  if (okRx == pdFAIL) {
    RIFTLINK_LOG_ERR("[RiftLink] radioSchedulerTask create FAIL\n");
  }
  RIFTLINK_DIAG("FSM", "event=FSM_FLAG name=RADIO_FSM_V2 enabled=%u",
      (unsigned)asyncIsRadioFsmV2Enabled());
  s_asyncTasksStarted = true;
}
