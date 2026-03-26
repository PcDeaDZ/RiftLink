/**
 * Packet Fusion — объединение 2–4 MSG для одного получателя в один LoRa-пакет
 * Меньше передач → меньше коллизий
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

namespace packet_fusion {

constexpr int MAX_BATCH = 8;
constexpr uint32_t BATCH_WINDOW_MS = 100;

void init();

/** Добавить MSG в батч. Возвращает true если принято (не отправлять отдельно). */
bool offer(const uint8_t* to, const uint8_t* plainBuf, size_t plainLen,
    uint32_t msgId, bool useCompressed);

/** Вызвать из msg_queue::update() — flush по таймеру */
void flush();

/** Callback при отправке batch (для registerBatchSent в msg_queue) */
void setOnBatchSent(void (*cb)(const uint8_t* to, const uint32_t* msgIds, int count, uint16_t batchPktId));
/** Callback при single flush (для registerPendingFromFusion) */
void setOnSingleFlush(bool (*cb)(const uint8_t* to, uint32_t msgId, const uint8_t* pkt, size_t pktLen, uint8_t txSf));

}  // namespace packet_fusion
