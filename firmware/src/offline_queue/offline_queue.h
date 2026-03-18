/**
 * RiftLink Offline Queue — store-and-forward
 * Хранение сообщений для офлайн-узлов, доставка при HELLO
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

#define OFFLINE_MAX_MSGS  16
#define OFFLINE_MAX_LEN   200

namespace offline_queue {

void init();
/** Добавить сообщение для офлайн-узла. flags: bit0=compressed */
bool enqueue(const uint8_t* to, const uint8_t* encPayload, size_t encLen, uint8_t opcode, uint8_t flags = 0);
/** Проверить, есть ли сообщения для узла (вызвать при HELLO) */
void onNodeOnline(const uint8_t* nodeId);
/** Количество сообщений в очереди (для evt info) */
int getPendingCount();
void update();  // Вызывать из loop

}  // namespace offline_queue
