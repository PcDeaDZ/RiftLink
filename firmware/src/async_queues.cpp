/**
 * Async Queues — инициализация очередей
 */

#include "async_queues.h"

QueueHandle_t packetQueue = nullptr;
QueueHandle_t sendQueue = nullptr;
QueueHandle_t displayQueue = nullptr;

static constexpr size_t PACKET_QUEUE_LEN =
#if defined(USE_EINK)
    96;  // Paper: e-ink + burst — 48 недостаточно при плотном трафике
#else
    8;
#endif
static constexpr size_t SEND_QUEUE_LEN = 8;
static constexpr size_t DISPLAY_QUEUE_LEN = 8;  // E-Ink обновления долгие — буфер для кнопки

bool asyncQueuesInit() {
  packetQueue = xQueueCreate(PACKET_QUEUE_LEN, sizeof(PacketQueueItem));
  sendQueue = xQueueCreate(SEND_QUEUE_LEN, sizeof(SendQueueItem));
  displayQueue = xQueueCreate(DISPLAY_QUEUE_LEN, sizeof(DisplayQueueItem));

  if (!packetQueue || !sendQueue || !displayQueue) {
    return false;
  }
  return true;
}
