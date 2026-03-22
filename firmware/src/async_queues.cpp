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
    16;
#else
    16;
#endif
static constexpr size_t SEND_QUEUE_LEN =
#if defined(USE_EINK)
    16;
#else
    16;
#endif
static constexpr size_t DISPLAY_QUEUE_LEN =
#if defined(USE_EINK)
    12;
#else
    12;
#endif

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
  // Fallback: WiFi+BLE — мало contiguous под крупные PacketQueueItem
  const struct {
    size_t pkt, send, disp;
  } steps[] = {
      {4, 4, 4},
      {4, 4, 2},
      {3, 3, 2},
  };
  for (const auto& st : steps) {
    size_t pkt = (PACKET_QUEUE_LEN >= st.pkt) ? st.pkt : PACKET_QUEUE_LEN;
    size_t send = (SEND_QUEUE_LEN >= st.send) ? st.send : SEND_QUEUE_LEN;
    size_t disp = (DISPLAY_QUEUE_LEN >= st.disp) ? st.disp : DISPLAY_QUEUE_LEN;
    if (tryCreateQueues(pkt, send, disp)) {
      Serial.printf("[RiftLink] Async queues: fallback %zu/%zu/%zu (heap %zu)\n",
          pkt, send, disp, (size_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      return true;
    }
  }
  return false;
}
