/**
 * Async Queues — инициализация очередей
 * При OOM (WiFi timer alloc, heap exhaustion) — fallback с уменьшенными размерами
 */

#include "async_queues.h"
#include <esp_heap_caps.h>

QueueHandle_t packetQueue = nullptr;
QueueHandle_t sendQueue = nullptr;
QueueHandle_t displayQueue = nullptr;

static constexpr size_t PACKET_QUEUE_LEN =
#if defined(USE_EINK)
    32;   // Paper: ~16KB heap; 64+WiFi давало OOM (ret=101)
#else
    16;   // OLED: MSG+HELLO+KEY_EXCHANGE — не дропать при всплеске
#endif
static constexpr size_t SEND_QUEUE_LEN = 32;  // burst 1–15, ACK, relay, KEY_EXCHANGE
static constexpr size_t DISPLAY_QUEUE_LEN = 8;  // E-Ink обновления долгие — буфер для кнопки

static bool tryCreateQueues(size_t pktLen, size_t sendLen, size_t dispLen) {
  if (packetQueue) { vQueueDelete(packetQueue); packetQueue = nullptr; }
  if (sendQueue) { vQueueDelete(sendQueue); sendQueue = nullptr; }
  if (displayQueue) { vQueueDelete(displayQueue); displayQueue = nullptr; }

  packetQueue = xQueueCreate(pktLen, sizeof(PacketQueueItem));
  sendQueue = xQueueCreate(sendLen, sizeof(SendQueueItem));
  displayQueue = xQueueCreate(dispLen, sizeof(DisplayQueueItem));

  if (!packetQueue || !sendQueue || !displayQueue) {
    if (packetQueue) vQueueDelete(packetQueue);
    if (sendQueue) vQueueDelete(sendQueue);
    if (displayQueue) vQueueDelete(displayQueue);
    packetQueue = sendQueue = displayQueue = nullptr;
    return false;
  }
  return true;
}

bool asyncQueuesInit() {
  if (tryCreateQueues(PACKET_QUEUE_LEN, SEND_QUEUE_LEN, DISPLAY_QUEUE_LEN)) {
    return true;
  }
  // Fallback: WiFi+ESP-NOW исчерпывают heap (fail to alloc timer) — уменьшаем очереди
  size_t pkt = (PACKET_QUEUE_LEN >= 16) ? 16 : PACKET_QUEUE_LEN;
  size_t send = (SEND_QUEUE_LEN >= 16) ? 16 : SEND_QUEUE_LEN;
  size_t disp = (DISPLAY_QUEUE_LEN >= 4) ? 4 : DISPLAY_QUEUE_LEN;
  if (tryCreateQueues(pkt, send, disp)) {
    Serial.printf("[RiftLink] Async queues: fallback %zu/%zu/%zu (heap %zu)\n",
        pkt, send, disp, (size_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
  }
  return false;
}
