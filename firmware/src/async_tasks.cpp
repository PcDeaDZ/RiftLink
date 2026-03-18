/**
 * Async Tasks — packetTask, displayTask, rxTask
 */

#include "async_tasks.h"
#include "log.h"
#include "async_queues.h"
#include "ui/display.h"
#include "radio/radio.h"
#include "protocol/packet.h"
#include "crypto/crypto.h"
#include "powersave/powersave.h"
#include <Arduino.h>
#include <ESP.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <string.h>

extern void getNextRxSlotParams(uint8_t* sfOut, uint32_t* slotMsOut);

#define DISPLAY_HEARTBEAT_MS 10000  // лог раз в 10 с — проверить, что displayTask жив

#define PACKET_TASK_STACK 32768
#define RX_TASK_STACK 4096
#define DISPLAY_TASK_STACK 8192   // 12KB — create FAIL (heap), 8KB — минимум для создания
#define PACKET_TASK_PRIO 2
#define RX_TASK_PRIO 4   // выше packetTask — приём важнее обработки
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

static void rxTask(void* arg) {
  static uint8_t rxBuf[protocol::SYNC_LEN + protocol::HEADER_LEN + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  vTaskDelay(pdMS_TO_TICKS(500));  // дать setup завершиться
  for (;;) {
    if (powersave::canSleep()) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    uint8_t sf;
    uint32_t slotMs;
    getNextRxSlotParams(&sf, &slotMs);
    if (!radio::takeMutex(pdMS_TO_TICKS(10))) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    radio::setSpreadingFactor(sf);
    radio::startReceiveWithTimeout(slotMs);
    radio::releaseMutex();
    vTaskDelay(pdMS_TO_TICKS(slotMs));
    if (!radio::takeMutex(pdMS_TO_TICKS(50))) continue;
    int n = radio::receiveAsync(rxBuf, sizeof(rxBuf));
    radio::releaseMutex();
    if (n > 0) {
      int rssi = radio::getLastRssi();
      if (packetQueue) {
#if defined(USE_EINK)
        // При почти полной очереди дропаем HELLO (дубликаты) — оставляем место для KEY_EXCHANGE
        UBaseType_t qLen = uxQueueMessagesWaiting(packetQueue);
        bool isHello = (n == 13 && rxBuf[0] == protocol::SYNC_BYTE && rxBuf[2] == protocol::OP_HELLO);
        if (!(isHello && qLen >= 48))  // 75% от 64 — дроп HELLO, сохраняем KEY_EXCHANGE
#endif
        {
          PacketQueueItem pitem;
          if ((size_t)n <= sizeof(pitem.buf)) {
            memcpy(pitem.buf, rxBuf, (size_t)n);
            pitem.len = (uint16_t)n;
            pitem.rssi = (int8_t)rssi;
            pitem.sf = sf;
            if (xQueueSend(packetQueue, &pitem, pdMS_TO_TICKS(30)) != pdTRUE) {
              RIFTLINK_LOG_ERR("[RiftLink] packetQueue full, drop\n");
            }
          }
        }
      } else {
        handlePacket(rxBuf, (size_t)n, rssi, sf);
      }
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
#if defined(USE_EINK)
  // Paper: displayTask на ядре 0 — e-ink display() блокирует ~600ms
  // packetTask и rxTask на ядре 1 — не блокируются e-ink, обрабатывают пакеты без задержек
  BaseType_t okDisplay = xTaskCreatePinnedToCore(displayTask, "display", DISPLAY_TASK_STACK, nullptr, DISPLAY_TASK_PRIO, nullptr, 0);
#else
  BaseType_t okDisplay = xTaskCreate(displayTask, "display", DISPLAY_TASK_STACK, nullptr, DISPLAY_TASK_PRIO, nullptr);
#endif
  if (okDisplay == pdFAIL) {
    RIFTLINK_LOG_ERR("[RiftLink] displayTask create FAIL (need %u)\n", (unsigned)DISPLAY_TASK_STACK);
  }
#if defined(USE_EINK)
  BaseType_t okPacket = xTaskCreatePinnedToCore(packetTask, "packet", PACKET_TASK_STACK, nullptr, PACKET_TASK_PRIO_EINK, nullptr, 1);
  if (okPacket == pdFAIL) {
    RIFTLINK_LOG_ERR("[RiftLink] packetTask create FAIL — loop будет обрабатывать packetQueue\n");
  }
  BaseType_t okRx = xTaskCreatePinnedToCore(rxTask, "rx", RX_TASK_STACK, nullptr, RX_TASK_PRIO, nullptr, 1);
#else
  xTaskCreate(packetTask, "packet", PACKET_TASK_STACK, nullptr, PACKET_TASK_PRIO_EINK, nullptr);
  BaseType_t okRx = xTaskCreate(rxTask, "rx", RX_TASK_STACK, nullptr, RX_TASK_PRIO, nullptr);
#endif
  if (okRx == pdFAIL) {
    RIFTLINK_LOG_ERR("[RiftLink] rxTask create FAIL\n");
  }
}
