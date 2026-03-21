/**
 * RiftLink Offline Queue — store-and-forward
 * Хранение сообщений для офлайн-узлов, доставка при HELLO
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

#define OFFLINE_MAX_MSGS  16
#define OFFLINE_MAX_LEN   280

namespace offline_queue {

void init();
/** Добавить сообщение для офлайн-узла. flags: bit0=compressed, bit1=critical lane */
bool enqueue(const uint8_t* to, const uint8_t* encPayload, size_t encLen, uint8_t opcode, uint8_t flags = 0);
/** Courier SCF: сохранить полный пакет relay до появления адресата. */
bool enqueueCourier(const uint8_t* pkt, size_t len);
/** Проверить, есть ли сообщения для узла (вызвать при HELLO) */
void onNodeOnline(const uint8_t* nodeId);
/** Количество сообщений в очереди (для evt info) */
int getPendingCount();
/** Количество courier-сообщений в очереди (SCF) */
int getCourierPendingCount();
/** Количество обычных (не-courier) сообщений в очереди */
int getDirectPendingCount();
void update();  // Вызывать из loop

}  // namespace offline_queue
