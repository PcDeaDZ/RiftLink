/**
 * send_overflow — буфер при полной radioCmdQueue (Tx) или при отказе queueSend.
 * Pull on-demand, приоритеты для ACK.
 */

#include "send_overflow.h"
#include "radio/radio.h"
#include "duty_cycle/duty_cycle.h"
#include <Arduino.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define PRIORITY_SLOTS 2   // ACK — обслуживаются первыми
#define NORMAL_SLOTS 4     // MSG, KEY_EXCHANGE и т.п.
#define MUTEX_TIMEOUT_MS 50

struct OverflowSlot {
  TxRequest req;
  bool used;
};

static OverflowSlot s_priority[PRIORITY_SLOTS];
static OverflowSlot s_normal[NORMAL_SLOTS];
static uint8_t s_priHead = 0, s_priTail = 0, s_priCount = 0;
static uint8_t s_normHead = 0, s_normTail = 0, s_normCount = 0;
static SemaphoreHandle_t s_mutex = nullptr;
static bool s_inited = false;

namespace send_overflow {

void init() {
  if (s_inited) return;
  s_mutex = xSemaphoreCreateMutex();
  memset(s_priority, 0, sizeof(s_priority));
  memset(s_normal, 0, sizeof(s_normal));
  s_inited = true;
}

bool push(const TxRequest& req) {
  if (!s_inited || req.len == 0 || req.len > PACKET_BUF_SIZE) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;

  OverflowSlot* arr = req.priority ? s_priority : s_normal;
  int cap = req.priority ? PRIORITY_SLOTS : NORMAL_SLOTS;
  uint8_t* tail = req.priority ? &s_priTail : &s_normTail;
  uint8_t* count = req.priority ? &s_priCount : &s_normCount;

  if (*count >= cap) {
    xSemaphoreGive(s_mutex);
    return false;
  }

  OverflowSlot* slot = &arr[*tail];
  slot->req = req;
  slot->req.txSf = (req.txSf >= 7 && req.txSf <= 12) ? req.txSf : 0;
  slot->used = true;
  *tail = (*tail + 1) % cap;
  (*count)++;
  xSemaphoreGive(s_mutex);
  return true;
}

bool pop(TxRequest* req) {
  if (!s_inited || !req) return false;
  if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;

  OverflowSlot* slot = nullptr;
  uint8_t* head = nullptr;
  int cap = 0;

  if (s_priCount > 0) {
    slot = &s_priority[s_priHead];
    head = &s_priHead;
    cap = PRIORITY_SLOTS;
    s_priCount--;
  } else if (s_normCount > 0) {
    slot = &s_normal[s_normHead];
    head = &s_normHead;
    cap = NORMAL_SLOTS;
    s_normCount--;
  }

  if (!slot) {
    xSemaphoreGive(s_mutex);
    return false;
  }

  *req = slot->req;
  slot->used = false;
  *head = (*head + 1) % cap;
  xSemaphoreGive(s_mutex);
  return true;
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
  if (txRequestQueue && xQueueReceive(txRequestQueue, req, 0) == pdTRUE) {
    return true;
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
