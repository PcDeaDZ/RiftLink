/**
 * Заглушки async_tasks / очередей для nRF — синхронный TX через radio::sendDirectInternal.
 */

#include "ack_coalesce/ack_coalesce.h"
#include "async_tasks.h"
#include "async_queues.h"
#include "radio/radio.h"
#include "protocol/packet.h"
#include "display_nrf.h"
#include "memory_diag/memory_diag.h"
#include <Arduino.h>
#include <string.h>

bool asyncInfraEnsure() {
  return true;
}

void asyncTasksStart() {}

TaskHandle_t asyncGetRadioSchedulerTaskHandle(void) {
  return nullptr;
}

bool asyncHasPacketTask(void) {
  return false;
}

void asyncMemoryDiagLogStacks() {
  memoryDiagLog("async_stub");
}

bool queueSend(const uint8_t* buf, size_t len, uint8_t txSf, bool priority, char* reasonBuf, size_t reasonLen) {
  return queueTxPacket(buf, len, txSf, priority, TxRequestClass::data, reasonBuf, reasonLen);
}

bool queueTxRequest(const TxRequest& req, char* reasonBuf, size_t reasonLen) {
  return queueTxPacket(req.buf, req.len, req.txSf, req.priority, req.klass, reasonBuf, reasonLen);
}

bool queueTxPacket(const uint8_t* buf, size_t len, uint8_t, bool, TxRequestClass, char* reasonBuf, size_t reasonLen) {
  if (!buf || len == 0 || len > PACKET_BUF_SIZE) {
    if (reasonBuf && reasonLen > 0) snprintf(reasonBuf, reasonLen, "%s", "pkt_oversize");
    return false;
  }
  if (!radio::takeMutex(pdMS_TO_TICKS(2000))) {
    if (reasonBuf && reasonLen > 0) snprintf(reasonBuf, reasonLen, "%s", "mutex");
    return false;
  }
  bool ok = radio::sendDirectInternal(buf, len, reasonBuf, reasonLen, false);
  radio::releaseMutex();
  return ok;
}

struct DeferredSlot {
  uint8_t buf[PACKET_BUF_SIZE];
  uint16_t len = 0;
  uint32_t dueMs = 0;
  bool used = false;
};

static constexpr int kDeferredSlots = 24;
static DeferredSlot s_deferred[kDeferredSlots];

static bool deferPacket(const uint8_t* pkt, size_t len, uint32_t delayMs) {
  if (!pkt || len == 0 || len > PACKET_BUF_SIZE) return false;
  for (int i = 0; i < kDeferredSlots; i++) {
    if (s_deferred[i].used) continue;
    memcpy(s_deferred[i].buf, pkt, len);
    s_deferred[i].len = (uint16_t)len;
    s_deferred[i].dueMs = millis() + delayMs;
    s_deferred[i].used = true;
    return true;
  }
  return false;
}

void queueDeferredAck(const uint8_t* pkt, size_t len, uint8_t, uint32_t delayMs, bool) {
  (void)deferPacket(pkt, len, delayMs);
}

void queueDeferredSend(const uint8_t* pkt, size_t len, uint8_t, uint32_t delayMs, bool) {
  (void)deferPacket(pkt, len, delayMs);
}

void queueDeferredRelay(const uint8_t* pkt, size_t len, uint8_t, uint32_t delayMs, const uint8_t*, uint32_t, bool) {
  (void)deferPacket(pkt, len, delayMs);
}

void relayHeard(const uint8_t*, uint32_t) {}

void flushDeferredSends() {
  ack_coalesce::flush();
  uint32_t now = millis();
  for (int i = 0; i < kDeferredSlots; i++) {
    if (!s_deferred[i].used) continue;
    if ((int32_t)(now - s_deferred[i].dueMs) < 0) continue;
    if (radio::takeMutex(pdMS_TO_TICKS(500))) {
      (void)radio::sendDirectInternal(s_deferred[i].buf, s_deferred[i].len, nullptr, 0, false);
      radio::releaseMutex();
    }
    s_deferred[i].used = false;
  }
}

void queueDisplayLastMsg(const char* fromHex, const char* text) {
  display_nrf::queue_last_msg(fromHex, text);
}
void queueDisplayRedraw(uint8_t, bool) {}
void queueDisplayRequestInfoRedraw() {}
void queueDisplayLongPress(uint8_t) {}
void queueDisplayWake() {}
void queueDisplayLedBlink() {}

static uint8_t deferred_used_count() {
  uint8_t n = 0;
  for (int i = 0; i < kDeferredSlots; i++) {
    if (s_deferred[i].used) n++;
  }
  return n;
}

uint8_t asyncTxQueueFree() {
  uint8_t u = deferred_used_count();
  return (u >= (uint8_t)kDeferredSlots) ? 0 : (uint8_t)(kDeferredSlots - u);
}

uint8_t asyncTxQueueWaiting() {
  return deferred_used_count();
}

bool asyncIsRadioFsmV2Enabled() {
  return false;
}

void asyncSetRadioFsmV2Enabled(bool) {}
