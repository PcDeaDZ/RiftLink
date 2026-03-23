/**
 * send_overflow — lock-free MPSC overflow buffer for TX requests.
 * Push from any context (BLE, main, msg_queue), pop only from radioSchedulerTask.
 * Priority lane (ACK) is drained first.
 * Uses FreeRTOS queues internally for pointer passing (4 bytes per item, negligible critical section).
 */

#include "send_overflow.h"
#include "radio/radio.h"
#include "duty_cycle/duty_cycle.h"
#include <Arduino.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_heap_caps.h>

#define PRIORITY_SLOTS 4
#define NORMAL_SLOTS 8

struct OverflowSlot {
  TxRequest req;
};

static OverflowSlot s_priStorage[PRIORITY_SLOTS];
static OverflowSlot s_normStorage[NORMAL_SLOTS];
static QueueHandle_t s_priFreeList = nullptr;
static QueueHandle_t s_normFreeList = nullptr;
static QueueHandle_t s_priQueue = nullptr;
static QueueHandle_t s_normQueue = nullptr;
static bool s_inited = false;

namespace send_overflow {

void init() {
  if (s_inited) return;
  s_priFreeList = xQueueCreate(PRIORITY_SLOTS, sizeof(OverflowSlot*));
  s_normFreeList = xQueueCreate(NORMAL_SLOTS, sizeof(OverflowSlot*));
  s_priQueue = xQueueCreate(PRIORITY_SLOTS, sizeof(OverflowSlot*));
  s_normQueue = xQueueCreate(NORMAL_SLOTS, sizeof(OverflowSlot*));
  if (!s_priFreeList || !s_normFreeList || !s_priQueue || !s_normQueue) return;
  for (int i = 0; i < PRIORITY_SLOTS; i++) {
    OverflowSlot* p = &s_priStorage[i];
    xQueueSend(s_priFreeList, &p, 0);
  }
  for (int i = 0; i < NORMAL_SLOTS; i++) {
    OverflowSlot* p = &s_normStorage[i];
    xQueueSend(s_normFreeList, &p, 0);
  }
  s_inited = true;
}

bool push(const TxRequest& req) {
  if (!s_inited || req.len == 0 || req.len > PACKET_BUF_SIZE) return false;
  QueueHandle_t freeList = req.priority ? s_priFreeList : s_normFreeList;
  QueueHandle_t queue = req.priority ? s_priQueue : s_normQueue;
  OverflowSlot* slot = nullptr;
  if (xQueueReceive(freeList, &slot, 0) != pdTRUE || !slot) return false;
  slot->req = req;
  slot->req.txSf = (req.txSf >= 7 && req.txSf <= 12) ? req.txSf : 0;
  if (xQueueSend(queue, &slot, 0) != pdTRUE) {
    xQueueSend(freeList, &slot, 0);
    return false;
  }
  return true;
}

bool pop(TxRequest* req) {
  if (!s_inited || !req) return false;
  OverflowSlot* slot = nullptr;
  if (s_priQueue && xQueueReceive(s_priQueue, &slot, 0) == pdTRUE && slot) {
    *req = slot->req;
    xQueueSend(s_priFreeList, &slot, 0);
    return true;
  }
  if (s_normQueue && xQueueReceive(s_normQueue, &slot, 0) == pdTRUE && slot) {
    *req = slot->req;
    xQueueSend(s_normFreeList, &slot, 0);
    return true;
  }
  return false;
}

void drainApplyCommandsFromRadioQueue(void) {
  RadioCmd cmd;
  for (;;) {
    if (!radioCmdQueue) return;
    if (xQueuePeek(radioCmdQueue, &cmd, 0) != pdTRUE) return;
    if (cmd.type == RadioCmdType::Tx) return;
    if (xQueueReceive(radioCmdQueue, &cmd, 0) != pdTRUE) return;
    if (cmd.type == RadioCmdType::ApplyRegion) {
      float f = (float)cmd.u.region.freqHz / 1000000.0f;
      radio::applyRegion(f, (int)cmd.u.region.power);
      duty_cycle::reset();
    } else if (cmd.type == RadioCmdType::ApplySf) {
      radio::setSpreadingFactor(cmd.u.spread.sf);
    } else if (cmd.type == RadioCmdType::ApplyModem) {
      if (cmd.u.modem.preset < 4) {
        radio::setModemPreset((radio::ModemPreset)cmd.u.modem.preset);
      } else {
        radio::setCustomModem(cmd.u.modem.sf, (float)cmd.u.modem.bw10 / 10.0f, cmd.u.modem.cr);
      }
    }
  }
}

bool getNextTxRequest(QueueHandle_t txRequestQueue, TxRequest* req) {
  if (!req) return false;
  drainApplyCommandsFromRadioQueue();
  if (txRequestQueue) {
    TxRequest* ptr = nullptr;
    if (xQueueReceive(txRequestQueue, &ptr, 0) == pdTRUE && ptr) {
      *req = *ptr;
      txRequestPool.free(ptr);
      return true;
    }
  }
  RadioCmd cmd;
  if (radioCmdQueue && xQueueReceive(radioCmdQueue, &cmd, 0) == pdTRUE) {
    if (cmd.type == RadioCmdType::Tx) {
      memset(req, 0, sizeof(*req));
      memcpy(req->buf, cmd.u.tx.buf, cmd.u.tx.len);
      req->len = cmd.u.tx.len;
      req->txSf = cmd.u.tx.txSf;
      req->priority = cmd.priority;
      req->klass = TxRequestClass::data;
      req->enqueueMs = millis();
      return true;
    }
    xQueueSendToFront(radioCmdQueue, &cmd, 0);
  }
  return pop(req);
}

}  // namespace send_overflow
