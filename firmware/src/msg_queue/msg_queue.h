/**
 * RiftLink — очередь сообщений с ACK и retransmit
 * Фаза 4: Mesh и стабильность
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

namespace msg_queue {

// msg_id в plaintext: первые 4 байта для unicast
constexpr size_t MSG_ID_LEN = 4;

void init();
void update();  // Вызывать из loop — проверка таймаутов, retransmit

// Отправка (добавляет в очередь, для unicast — ждёт ACK). ttlMinutes: 0 = постоянное
bool enqueue(const uint8_t* to, const char* text, uint8_t ttlMinutes = 0);

// Отправка в группу (group_id 4B, broadcast, без ACK)
bool enqueueGroup(uint32_t groupId, const char* text);

// Обработка входящего ACK (удаляет из pending)
void onAckReceived(const uint8_t* payload, size_t payloadLen);

// Callback при успешной постановке unicast в очередь (для evt "sent")
void setOnUnicastSent(void (*cb)(const uint8_t* to, uint32_t msgId));

}  // namespace msg_queue
