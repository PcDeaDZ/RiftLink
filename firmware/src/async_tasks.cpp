/**
 * Async Tasks — packetTask, displayTask
 */

#include "async_tasks.h"
#include "log.h"
#include "async_queues.h"
#include "ui/display.h"
#include "radio/radio.h"
#include <Arduino.h>
#include <ESP.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <string.h>

#define DISPLAY_HEARTBEAT_MS 10000  // лог раз в 10 с — проверить, что displayTask жив

#define PACKET_TASK_STACK 32768
#define DISPLAY_TASK_STACK 8192   // 12KB — create FAIL (heap), 8KB — минимум для создания
#define PACKET_TASK_PRIO 2
#define DISPLAY_TASK_PRIO 1
#if defined(USE_EINK)
#define PACKET_TASK_PRIO_EINK 3   // Paper: выше displayTask — e-ink блокирует, packetTask важнее
#else
#define PACKET_TASK_PRIO_EINK PACKET_TASK_PRIO
#endif

// Forward — реализация в main.cpp (async)
extern void handlePacket(const uint8_t* buf, size_t len, int rssi, uint8_t sf);

bool queueSend(const uint8_t* buf, size_t len, uint8_t txSf, bool priority) {
  if (!sendQueue || len > PACKET_BUF_SIZE) return false;
  SendQueueItem item;
  memcpy(item.buf, buf, len);
  item.len = (uint16_t)len;
  item.txSf = (txSf >= 7 && txSf <= 12) ? txSf : 0;
  BaseType_t ok = priority ? xQueueSendToFront(sendQueue, &item, 0) : xQueueSend(sendQueue, &item, 0);
  return ok == pdTRUE;
}

void queueDisplayLastMsg(const char* fromHex, const char* text) {
  if (!displayQueue) {
    displaySetLastMsg(fromHex, text);
    return;
  }
  DisplayQueueItem item = {};
  item.cmd = CMD_SET_LAST_MSG;
  item.screen = 4;
  if (fromHex) strncpy(item.fromHex, fromHex, 16);
  if (text) strncpy(item.text, text, 63);
  xQueueSend(displayQueue, &item, 0);
}

void queueDisplayRedraw(uint8_t screen, bool priority) {
  if (!displayQueue) {
    displayShowScreen(screen);
    return;
  }
  DisplayQueueItem item = {};
  item.cmd = CMD_REDRAW_SCREEN;
  item.screen = screen;
  BaseType_t ok = priority ? xQueueSendToFront(displayQueue, &item, pdMS_TO_TICKS(200))
                           : xQueueSend(displayQueue, &item, pdMS_TO_TICKS(200));
  if (ok != pdTRUE) {
    Serial.println("[RiftLink] displayQueue full, fallback draw");
    displayShowScreen(screen);  // fallback при переполнении очереди
  }
}

void queueDisplayRequestInfoRedraw() {
  if (!displayQueue) {
    displayRequestInfoRedraw();
    return;
  }
  DisplayQueueItem item = {};
  item.cmd = CMD_REQUEST_INFO_REDRAW;
  item.screen = 1;
  xQueueSend(displayQueue, &item, 0);
}

void queueDisplayLongPress(uint8_t screen) {
  if (!displayQueue) {
    displayOnLongPress(screen);
    return;
  }
  DisplayQueueItem item = {};
  item.cmd = CMD_LONG_PRESS;
  item.screen = screen;
  if (xQueueSend(displayQueue, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
    Serial.println("[RiftLink] displayQueue full, fallback longPress");
    displayOnLongPress(screen);  // fallback при переполнении очереди
  }
}

void queueDisplayWake() {
  if (!displayQueue) {
    displayWake();
    displayShowScreen(displayGetCurrentScreen());
    return;
  }
  DisplayQueueItem item = {};
  item.cmd = CMD_WAKE;
  item.screen = 0;
  if (xQueueSend(displayQueue, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
    displayWake();
    displayShowScreen(displayGetCurrentScreen());
  }
}

static void packetTask(void* arg) {
  PacketQueueItem item;
  for (;;) {
    if (xQueueReceive(packetQueue, &item, portMAX_DELAY) == pdTRUE) {
      handlePacket(item.buf, item.len, (int)item.rssi, item.sf);
    }
  }
}

static void displayTask(void* arg) {
  vTaskDelay(pdMS_TO_TICKS(100));  // дать setup завершиться
  DisplayQueueItem item;
  uint32_t lastHeartbeat = millis();
  for (;;) {
    if (xQueueReceive(displayQueue, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
      switch (item.cmd) {
        case CMD_SET_LAST_MSG:
          displaySetLastMsg(item.fromHex, item.text);
          break;
        case CMD_REDRAW_SCREEN: {
          uint8_t lastScreen = item.screen;
          while (xQueuePeek(displayQueue, &item, 0) == pdTRUE && item.cmd == CMD_REDRAW_SCREEN) {
            xQueueReceive(displayQueue, &item, 0);
            lastScreen = item.screen;
          }
          displayShowScreen(lastScreen);
          break;
        }
        case CMD_REQUEST_INFO_REDRAW:
          displayRequestInfoRedraw();
          break;
        case CMD_LONG_PRESS:
          displayOnLongPress(item.screen);
          break;
        case CMD_WAKE:
          displayWake();
          displayShowScreen(displayGetCurrentScreen());
          break;
      }
    }
    displayUpdate();
    uint32_t now = millis();
    if (now - lastHeartbeat >= DISPLAY_HEARTBEAT_MS) {
      lastHeartbeat = now;
    }
  }
}

void asyncTasksStart() {
  BaseType_t okDisplay = xTaskCreate(displayTask, "display", DISPLAY_TASK_STACK, nullptr, DISPLAY_TASK_PRIO, nullptr);
  if (okDisplay == pdFAIL) {
    RIFTLINK_LOG_ERR("[RiftLink] displayTask create FAIL (need %u)\n", (unsigned)DISPLAY_TASK_STACK);
  }
  xTaskCreate(packetTask, "packet", PACKET_TASK_STACK, nullptr, PACKET_TASK_PRIO_EINK, nullptr);
}
