/**
 * send_overflow — буфер пакетов при полной sendQueue.
 * Pull on-demand: radio scheduler тянет отсюда при пустой sendQueue.
 * Приоритетные слоты (ACK) обслуживаются первыми.
 */

#pragma once

#include "async_queues.h"
#include <cstddef>
#include <cstdint>

namespace send_overflow {

void init();
/** Добавить пакет. priority=true — ACK, идёт в приоритетные слоты. */
bool push(const uint8_t* buf, size_t len, uint8_t txSf, bool priority);
/** Забрать следующий пакет для TX (приоритетные первыми). Возвращает true если есть. */
bool pop(SendQueueItem* item);

/** Единый источник: получить следующий пакет для TX (sendQueue или send_overflow). */
bool getNextTxPacket(SendQueueItem* item);

}  // namespace send_overflow
