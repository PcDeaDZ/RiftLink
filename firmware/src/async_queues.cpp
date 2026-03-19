/**
 * Async Queues — инициализация очередей
 */

#include "async_queues.h"

QueueHandle_t packetQueue = nullptr;
QueueHandle_t sendQueue = nullptr;
QueueHandle_t displayQueue = nullptr;

static constexpr size_t PACKET_QUEUE_LEN =
#if defined(USE_EINK)
    64;   // Paper: ~16KB heap; 192 давало init FAILED (OOM)
#else
    16;   // OLED: MSG+HELLO+KEY_EXCHANGE — не дропать при всплеске
#endif
static constexpr size_t SEND_QUEUE_LEN = 24;  // ACK/relay приоритет — больше буфер для burst
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
