/**
 * send_overflow — буфер при полной radioCmdQueue.
 * Pull on-demand: radio scheduler тянет отсюда при пустой очереди Tx.
 * Приоритетные слоты (ACK) обслуживаются первыми.
 */

#pragma once

#include "async_queues.h"
#include "async_tasks.h"
#include <cstddef>
#include <cstdint>

namespace send_overflow {

void init();
/** Добавить пакет. priority=true — ACK, идёт в приоритетные слоты. */
bool push(const TxRequest& req);
/** Забрать следующий TX_Request (приоритетные первыми). */
bool pop(TxRequest* req);

/** Снять с головы radioCmdQueue все ApplyRegion/ApplySf до первого Tx. Только под mutex радио. */
void drainApplyCommandsFromRadioQueue(void);

/** Единый источник: txRequestQueue/radioCmdQueue(Tx legacy) или send_overflow. */
bool getNextTxRequest(QueueHandle_t txRequestQueue, TxRequest* req);

}  // namespace send_overflow
