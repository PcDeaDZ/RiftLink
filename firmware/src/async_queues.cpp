/**
 * Async Queues — инициализация очередей
 * При OOM (WiFi timer alloc, heap exhaustion) — fallback с уменьшенными размерами
 */

#include "async_queues.h"
#include <esp_heap_caps.h>

QueueHandle_t packetQueue = nullptr;
QueueHandle_t radioCmdQueue = nullptr;
QueueHandle_t displayQueue = nullptr;

static constexpr size_t PACKET_QUEUE_LEN =
#if defined(USE_EINK)
    32;   // Paper: ~16KB heap; 64+WiFi давало OOM (ret=101)
#else
    // OLED: после Wi-Fi largest free block часто <16K — 64/48 съедают heap и мешают xTaskCreate(packet).
    // 32/32: меньше фрагментации; burst HELLO/KEY — см. send_overflow и ACK reserve.
    32;
#endif
// Paper: экономия RAM. V3/V4: после lazy-init TX burst (KEY+HELLO) — 32 давало частые drop у HELLO.
static constexpr size_t SEND_QUEUE_LEN =
#if defined(USE_EINK)
    32;
#else
    32;
#endif
static constexpr size_t DISPLAY_QUEUE_LEN = 12;  // burst HELLO → много Info redraw; coalesce в displayTask

static bool tryCreateQueues(size_t pktLen, size_t sendLen, size_t dispLen) {
  if (packetQueue) { vQueueDelete(packetQueue); packetQueue = nullptr; }
  if (radioCmdQueue) { vQueueDelete(radioCmdQueue); radioCmdQueue = nullptr; }
  if (displayQueue) { vQueueDelete(displayQueue); displayQueue = nullptr; }

  // Сначала маленькая displayQueue — оставляет больший contiguous блок под крупные packet/send.
  displayQueue = xQueueCreate(dispLen, sizeof(DisplayQueueItem));
  packetQueue = xQueueCreate(pktLen, sizeof(PacketQueueItem));
  radioCmdQueue = xQueueCreate(sendLen, sizeof(RadioCmd));

  if (!packetQueue || !radioCmdQueue || !displayQueue) {
    if (displayQueue) vQueueDelete(displayQueue);
    if (packetQueue) vQueueDelete(packetQueue);
    if (radioCmdQueue) vQueueDelete(radioCmdQueue);
    packetQueue = radioCmdQueue = displayQueue = nullptr;
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
