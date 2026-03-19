/**
 * Async Tasks — packetTask, displayTask, radioSchedulerTask (RX + drain)
 */

#include "async_tasks.h"
#include "log.h"
#include "async_queues.h"
#include "ack_coalesce/ack_coalesce.h"
#include "send_overflow/send_overflow.h"
#include "ui/display.h"
#include "led/led.h"
#include "radio/radio.h"
#include "protocol/packet.h"
#include "crypto/crypto.h"
#include "powersave/powersave.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <string.h>

extern void getNextRxSlotParams(uint8_t* sfOut, uint32_t* slotMsOut);

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
/** V4: после TX на SF12 — следующий RX слот на SF12. Вызывать из radioSchedulerTask. */
extern void onRadioSchedulerTxSf12(void);

#define ACK_RESERVE_SLOTS 4   // резерв для ACK — обычные пакеты отклоняются при ≤4 свободных
bool queueSend(const uint8_t* buf, size_t len, uint8_t txSf, bool priority) {
  if (!sendQueue || len > PACKET_BUF_SIZE) return false;
  if (!priority && uxQueueSpacesAvailable(sendQueue) <= ACK_RESERVE_SLOTS) {
    return send_overflow::push(buf, len, txSf, false);  // fallback в send_overflow
  }
  SendQueueItem item;
  memcpy(item.buf, buf, len);
  item.len = (uint16_t)len;
  item.txSf = (txSf >= 7 && txSf <= 12) ? txSf : 0;
  BaseType_t ok = priority ? xQueueSendToFront(sendQueue, &item, 0) : xQueueSend(sendQueue, &item, 0);
  if (ok != pdTRUE) return send_overflow::push(buf, len, txSf, priority);  // fallback в send_overflow
  return true;
}

#define DEFERRED_ACK_SLOTS 8   // broadcast: несколько соседей шлют ACK почти одновременно
#define DEFERRED_SEND_SLOTS 24   // MSG copy2–3, broadcast 2–3, KEY_EXCHANGE — burst 1–15
#define HEARD_RELAY_SIZE 8   // Managed flooding: отмена relay при услышанной ретрансляции
struct DeferredSlot {
  uint8_t buf[PACKET_BUF_SIZE];
  uint16_t len;
  uint8_t txSf;
  uint32_t sendAfter;
  bool used;
  bool isRelay;   // relay: отменить если услышали ретрансляцию
  uint8_t relayFrom[protocol::NODE_ID_LEN];
  uint32_t relayHash;
};
struct HeardRelayEntry { uint8_t from[protocol::NODE_ID_LEN]; uint32_t hash; };
static HeardRelayEntry s_heardRelay[HEARD_RELAY_SIZE];
static uint8_t s_heardRelayIdx = 0;

static DeferredSlot s_deferredAck[DEFERRED_ACK_SLOTS];
static DeferredSlot s_deferredSend[DEFERRED_SEND_SLOTS];

static bool relayHeardCheckAndRemove(const uint8_t* from, uint32_t hash) {
  for (int i = 0; i < HEARD_RELAY_SIZE; i++) {
    if (memcmp(s_heardRelay[i].from, from, protocol::NODE_ID_LEN) == 0 && s_heardRelay[i].hash == hash) {
      s_heardRelay[i].hash = 0xFFFFFFFFU;
      return true;
    }
  }
  return false;
}

static void flushDeferredSlots(DeferredSlot* slots, int n) {
  if (!sendQueue) return;
  uint32_t now = millis();
  for (int i = 0; i < n; i++) {
    if (slots[i].used && slots[i].sendAfter <= now) {
      if (slots[i].isRelay && relayHeardCheckAndRemove(slots[i].relayFrom, slots[i].relayHash)) {
        slots[i].used = false;
        continue;
      }
      SendQueueItem item;
      memcpy(item.buf, slots[i].buf, slots[i].len);
      item.len = slots[i].len;
      item.txSf = slots[i].txSf;
      if (xQueueSendToFront(sendQueue, &item, 0) == pdTRUE) {
        slots[i].used = false;
      }
    }
  }
}

void queueDeferredAck(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs) {
  uint32_t sendAfter = millis() + delayMs;
  for (int i = 0; i < DEFERRED_ACK_SLOTS; i++) {
    if (!s_deferredAck[i].used) {
      memcpy(s_deferredAck[i].buf, pkt, len);
      s_deferredAck[i].len = (uint16_t)len;
      s_deferredAck[i].txSf = (txSf >= 7 && txSf <= 12) ? txSf : 0;
      s_deferredAck[i].sendAfter = sendAfter;
      s_deferredAck[i].used = true;
      s_deferredAck[i].isRelay = false;
      return;
    }
  }
  queueSend(pkt, len, txSf, true);
}

void queueDeferredSend(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs) {
  uint32_t sendAfter = millis() + delayMs;
  for (int i = 0; i < DEFERRED_SEND_SLOTS; i++) {
    if (!s_deferredSend[i].used) {
      memcpy(s_deferredSend[i].buf, pkt, len);
      s_deferredSend[i].len = (uint16_t)len;
      s_deferredSend[i].txSf = (txSf >= 7 && txSf <= 12) ? txSf : 0;
      s_deferredSend[i].sendAfter = sendAfter;
      s_deferredSend[i].used = true;
      s_deferredSend[i].isRelay = false;
      return;
    }
  }
  if (!queueSend(pkt, len, txSf, true)) {
    RIFTLINK_LOG_ERR("[RiftLink] deferred fallback sendQueue full, drop copy\n");
  }
}

void queueDeferredRelay(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs,
    const uint8_t* from, uint32_t payloadHash) {
  uint32_t sendAfter = millis() + delayMs;
  for (int i = 0; i < DEFERRED_SEND_SLOTS; i++) {
    if (!s_deferredSend[i].used) {
      memcpy(s_deferredSend[i].buf, pkt, len);
      s_deferredSend[i].len = (uint16_t)len;
      s_deferredSend[i].txSf = (txSf >= 7 && txSf <= 12) ? txSf : 0;
      s_deferredSend[i].sendAfter = sendAfter;
      s_deferredSend[i].used = true;
      s_deferredSend[i].isRelay = true;
      memcpy(s_deferredSend[i].relayFrom, from, protocol::NODE_ID_LEN);
      s_deferredSend[i].relayHash = payloadHash;
      return;
    }
  }
  if (!queueSend(pkt, len, txSf, true)) {
    RIFTLINK_LOG_ERR("[RiftLink] deferred relay fallback sendQueue full, drop\n");
  }
}

void relayHeard(const uint8_t* from, uint32_t payloadHash) {
  memcpy(s_heardRelay[s_heardRelayIdx].from, from, protocol::NODE_ID_LEN);
  s_heardRelay[s_heardRelayIdx].hash = payloadHash;
  s_heardRelayIdx = (s_heardRelayIdx + 1) % HEARD_RELAY_SIZE;
}

void flushDeferredSends() {
  // Pull on-demand: radio scheduler тянет из send_overflow при пустой sendQueue
  // Сначала deferred send — потом ACK. SendToFront: последний добавленный впереди. ACK впереди = доставка.
  ack_coalesce::flush();
  flushDeferredSlots(s_deferredSend, DEFERRED_SEND_SLOTS);
  flushDeferredSlots(s_deferredAck, DEFERRED_ACK_SLOTS);
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
  if (xQueueSend(displayQueue, &item, 0) != pdTRUE) {
    displaySetLastMsg(fromHex, text);  // fallback — не терять сообщение при переполнении
  }
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
    static uint32_t s_lastFallbackLog = 0;
    uint32_t now = millis();
    if (now - s_lastFallbackLog >= 10000) {  // не спамить при OOM (heap ~5KB, queue=4)
      s_lastFallbackLog = now;
      Serial.println("[RiftLink] displayQueue full, fallback draw");
    }
    displayShowScreen(screen);  // fallback при переполнении очереди
  }
}

/** Только флаг s_needRedrawInfo — без очереди. Отрисовка в displayUpdate() при s_currentScreen==1 */
void queueDisplayRequestInfoRedraw() {
  displayRequestInfoRedraw();
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
    static uint32_t s_lastLongPressFallbackLog = 0;
    uint32_t now = millis();
    if (now - s_lastLongPressFallbackLog >= 10000) {
      s_lastLongPressFallbackLog = now;
      Serial.println("[RiftLink] displayQueue full, fallback longPress");
    }
    displayOnLongPress(screen);  // fallback при переполнении очереди
  }
}

void queueDisplayLedBlink() {
  if (!displayQueue) {
    ledBlink(20);
    return;
  }
  DisplayQueueItem item = {};
  item.cmd = CMD_BLINK_LED;
  item.screen = 0;
  xQueueSend(displayQueue, &item, 0);
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

#define RX_ALIVE_LOG_INTERVAL_MS 120000  // 2 мин — диагностика
#define RADIO_SCHEDULER_STACK 4096
#define RADIO_SCHEDULER_PRIO 4

/** Radio scheduler: один владелец радио, чередует RX и TX. Нет конкуренции drain vs rx. */
static void radioSchedulerTask(void* arg) {
  static uint8_t rxBuf[protocol::SYNC_LEN + protocol::HEADER_LEN + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  static TickType_t lastPacketQueueDropLog = 0;
  static uint32_t lastRxAliveLog = 0;
#if !defined(SF_FORCE_7)
  static int highSfDrained = 0;  // счётчик подряд отправленных SF10+ — макс 2, иначе RX не успевает
#endif
  vTaskDelay(pdMS_TO_TICKS(500));  // дать setup завершиться
  for (;;) {
    if (powersave::canSleep()) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    flushDeferredSends();
    if (radio::takeMutex(pdMS_TO_TICKS(200)) != pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    // TX: до 6 пакетов за цикл при SF7/9 (pipelining), 2 при SF10/11, 1 при SF12
    bool didTx = false;
    uint32_t lastToaUs = 0;
    bool pushedBack = false;
    int drainLimit = 6;
    for (int drainIdx = 0; drainIdx < drainLimit; drainIdx++) {
    SendQueueItem item;
    if (!send_overflow::getNextTxPacket(&item)) break;
    if (drainIdx > 0 && item.txSf >= 10) { drainLimit = drainIdx + 1; }  // SF10+: только 1 доп. пакет
    if (drainIdx >= 2 && item.txSf >= 10) break;  // SF10+ после 2-го — стоп
#if defined(SF_FORCE_7)
        if (item.len >= 54 && item.buf[0] == protocol::SYNC_BYTE && item.buf[2] == protocol::OP_KEY_EXCHANGE) {
          queueDeferredSend(item.buf, item.len, item.txSf, 500 + (esp_random() % 1501));
        } else {
          lastToaUs = radio::getTimeOnAir(item.len);
          radio::sendDirectInternal(item.buf, item.len);
          didTx = true;
          if (item.buf[0] == protocol::SYNC_BYTE && item.buf[2] == protocol::OP_MSG) {
            vTaskDelay(pdMS_TO_TICKS(60));
          }
        }
#else
        if (item.txSf >= 10 && highSfDrained >= 2) {
          xQueueSendToFront(sendQueue, &item, 0);
          pushedBack = true;
          break;  // не drain дальше — ждём RX
        } else {
          if (item.len >= 54 && item.buf[0] == protocol::SYNC_BYTE && item.buf[2] == protocol::OP_KEY_EXCHANGE) {
            queueDeferredSend(item.buf, item.len, item.txSf, 500 + (esp_random() % 1501));
          } else {
            if (item.txSf >= 7 && item.txSf <= 12) {
              radio::setSpreadingFactor(item.txSf);
              if (item.txSf >= 10) {
                highSfDrained++;
                onRadioSchedulerTxSf12();
              } else {
                highSfDrained = 0;
              }
            }
            lastToaUs = radio::getTimeOnAir(item.len);
            radio::sendDirectInternal(item.buf, item.len);
            didTx = true;
            if (item.buf[0] == protocol::SYNC_BYTE && item.buf[2] == protocol::OP_MSG) {
              vTaskDelay(pdMS_TO_TICKS(60));
            }
          }
        }
#endif
    }  // for drainIdx
    if (didTx && lastToaUs > 0) {
      uint32_t toaMs = (lastToaUs + 999) / 1000;
      if (toaMs > 0 && toaMs < 500) vTaskDelay(pdMS_TO_TICKS(toaMs));
    }
#if !defined(SF_FORCE_7)
    if (!didTx && !pushedBack) highSfDrained = 0;  // очередь пуста — сброс. pushedBack — не сбрасывать
#endif
    // RX: основной режим
    uint8_t sf;
    uint32_t slotMs;
    getNextRxSlotParams(&sf, &slotMs);
    radio::setSpreadingFactor(sf);
    radio::startReceiveWithTimeout(slotMs);
    vTaskDelay(pdMS_TO_TICKS(slotMs));
    int n = radio::receiveAsync(rxBuf, sizeof(rxBuf));
    radio::releaseMutex();
    uint32_t now = millis();
    if (now - lastRxAliveLog >= RX_ALIVE_LOG_INTERVAL_MS) {
      lastRxAliveLog = now;
      Serial.printf("[RiftLink] RX alive heap=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    if (n > 0) {
      int rssi = radio::getLastRssi();
      if (packetQueue) {
        UBaseType_t qCap = uxQueueSpacesAvailable(packetQueue) + uxQueueMessagesWaiting(packetQueue);
        UBaseType_t qLen = uxQueueMessagesWaiting(packetQueue);
        bool isHello = (n == 13 && rxBuf[0] == protocol::SYNC_BYTE && rxBuf[2] == protocol::OP_HELLO);
        if (!(isHello && qLen >= (qCap * 3 / 4))) {
          PacketQueueItem pitem;
          if ((size_t)n <= sizeof(pitem.buf)) {
            memcpy(pitem.buf, rxBuf, (size_t)n);
            pitem.len = (uint16_t)n;
            pitem.rssi = (int8_t)rssi;
            pitem.sf = sf;
            if (xQueueSend(packetQueue, &pitem, pdMS_TO_TICKS(30)) != pdTRUE) {
              bool added = false;
              PacketQueueItem discarded;
              if (xQueueReceive(packetQueue, &discarded, 0) == pdTRUE) {
                bool wasHello = (discarded.len == 13 && discarded.buf[0] == protocol::SYNC_BYTE &&
                    discarded.buf[2] == protocol::OP_HELLO);
                if (wasHello) {
                  added = (xQueueSend(packetQueue, &pitem, 0) == pdTRUE);
                } else {
                  xQueueSendToFront(packetQueue, &discarded, 0);
                }
              }
              if (!added) {
                TickType_t t = xTaskGetTickCount();
                if (t - lastPacketQueueDropLog >= pdMS_TO_TICKS(5000)) {
                  lastPacketQueueDropLog = t;
                  RIFTLINK_LOG_ERR("[RiftLink] packetQueue full, drop\n");
                }
              }
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
        case CMD_BLINK_LED:
          ledBlink(20);
          break;
      }
    }
    ledUpdate();
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
  BaseType_t okRx = xTaskCreatePinnedToCore(radioSchedulerTask, "radio", RADIO_SCHEDULER_STACK, nullptr, RADIO_SCHEDULER_PRIO, nullptr, 1);
#else
  xTaskCreate(packetTask, "packet", PACKET_TASK_STACK, nullptr, PACKET_TASK_PRIO_EINK, nullptr);
  BaseType_t okRx = xTaskCreate(radioSchedulerTask, "radio", RADIO_SCHEDULER_STACK, nullptr, RADIO_SCHEDULER_PRIO, nullptr);
#endif
  if (okRx == pdFAIL) {
    RIFTLINK_LOG_ERR("[RiftLink] radioSchedulerTask create FAIL\n");
  }
}
