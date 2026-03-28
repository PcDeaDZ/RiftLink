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
enum TriggerType : uint8_t {
  TRIGGER_NONE = 0,
  TRIGGER_TARGET_ONLINE = 1,
  TRIGGER_DELIVER_AFTER = 2,
};

enum SendFailReason : uint8_t {
  SEND_FAIL_NONE = 0,
  SEND_FAIL_NOT_READY,
  SEND_FAIL_MUTEX_BUSY,
  SEND_FAIL_EMPTY,
  SEND_FAIL_PENDING_FULL,
  SEND_FAIL_NO_KEY,
  SEND_FAIL_KEY_BUSY,
  SEND_FAIL_BUILD_PACKET,
  SEND_FAIL_RADIO_QUEUE,
  /** Длинный текст: `frag::send` вернул false (на nRF фрагментация может быть отключена). */
  SEND_FAIL_FRAG_UNAVAILABLE,
};

void init();
void update();  // Вызывать из loop — проверка таймаутов, retransmit

// Отправка (добавляет в очередь, для unicast — ждёт ACK).
// critical=true -> CHANNEL_CRITICAL + приоритетная отправка.
// triggerType/triggerValueMs: Time Capsule MVP.
bool enqueue(const uint8_t* to, const char* text, uint8_t ttlMinutes = 0,
    bool critical = false, TriggerType triggerType = TRIGGER_NONE, uint32_t triggerValueMs = 0);

// Последняя причина отказа enqueue (для точного BLE-статуса без догадок через hasKeyFor()).
SendFailReason getLastSendFailReason();

// Отправка в группу (group_id 4B, broadcast, без ACK)
bool enqueueGroup(uint32_t groupId, const char* text);
/** Emergency flood (SOS): encrypted broadcast, отдельный opcode. */
bool enqueueSos(const char* text);

// Обработка входящего ACK.
// Возвращает true, если подтверждён unicast (для notifyDelivered).
bool onAckReceived(const uint8_t* from, const uint8_t* payload, size_t payloadLen,
    bool requireOnline = true, bool allowUnicast = true, bool allowBroadcast = true);

// Witness ACK (ECHO): учитывает только broadcast delivery, unicast не подтверждает.
bool onBroadcastAckWitness(const uint8_t* from, uint32_t msgId, bool requireOnline = true);

// Обработка ACK_BATCH: count(1) + msgId(4)* — для каждого msgId вызывает onAckReceived.
// onDelivered вызывается для каждого доставленного msgId (для ble::notifyDelivered).
void onAckBatchReceived(const uint8_t* from, const uint8_t* payload, size_t payloadLen, int rssi,
    void (*onDelivered)(const uint8_t* from, uint32_t msgId, int rssi));
// Selective ACK v3: bitmap for batch messages from peer.
void onSelectiveAckReceived(const uint8_t* from, uint16_t batchPktId, uint16_t ackBitmap, int rssi,
    void (*onDelivered)(const uint8_t* from, uint32_t msgId, int rssi));

// RIT: при получении POLL от получателя — ускорить отправку pending для него
void onPollReceived(const uint8_t* from);

// Packet Fusion: зарегистрировать batch для ACK tracking
void registerBatchSent(const uint8_t* to, const uint32_t* msgIds, int count, uint16_t batchPktId);
// Packet Fusion: single message flush — добавить в pending
bool registerPendingFromFusion(const uint8_t* to, uint32_t msgId, const uint8_t* pkt, size_t pktLen, uint8_t txSf);
// Retransmit specific pending message by msgId if still in pending table.
bool retransmitPendingByMsgId(const uint8_t* to, uint32_t msgId);

// Callback при успешной постановке unicast в очередь (для evt "sent")
void setOnUnicastSent(void (*cb)(const uint8_t* to, uint32_t msgId));
// Callback при неудачной доставке — ACK не получен после всех retry (для evt "undelivered")
void setOnUnicastUndelivered(void (*cb)(const uint8_t* to, uint32_t msgId));
// Callback при broadcast sent (для evt "sent" с msgId — сопоставление с delivery)
void setOnBroadcastSent(void (*cb)(uint32_t msgId));
// Callback при broadcast delivery — delivered, total (evt "broadcast_delivery" или "undelivered" при 0/total)
void setOnBroadcastDelivery(void (*cb)(uint32_t msgId, int delivered, int total));
// Callback когда Time Capsule триггер сработал и пакет пошел в эфир.
void setOnTimeCapsuleReleased(void (*cb)(const uint8_t* to, uint32_t msgId, uint8_t triggerType));

}  // namespace msg_queue
