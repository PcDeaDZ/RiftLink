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
#include <freertos/semphr.h>
#include "crypto/crypto.h"
#include "powersave/powersave.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
/** xTaskCreateWithCaps объявлен в ESP-IDF здесь, не в esp_task.h (Arduino 3 / IDF 5.x). */
#if __has_include("freertos/idf_additions.h")
#include "freertos/idf_additions.h"
#define RIFTLINK_HAVE_TASK_CREATE_WITH_CAPS 1
#elif __has_include(<freertos/idf_additions.h>)
#include <freertos/idf_additions.h>
#define RIFTLINK_HAVE_TASK_CREATE_WITH_CAPS 1
#endif
#include <string.h>
#include <stdio.h>
#include <atomic>

extern void getNextRxSlotParams(uint8_t* sfOut, uint32_t* slotMsOut);

static inline void queueSendReason(char* buf, size_t buflen, const char* msg) {
  if (buf && buflen > 0) {
    snprintf(buf, buflen, "%s", msg ? msg : "?");
  }
}

#define DISPLAY_HEARTBEAT_MS 10000  // лог раз в 10 с — проверить, что displayTask жив

// OLED: после Wi-Fi + очередей (64/48) в internal heap не хватает места под стек 32K+8K+4K — xTaskCreate(packet) даёт FAIL.
// Watermark на V4 при 32K был ~31K «свободно» (фактически байты/запас) — 16K достаточно для handlePacket.
#if defined(USE_EINK)
#define PACKET_TASK_STACK 32768
#else
#define PACKET_TASK_STACK 4096
#endif
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
/** HELLO: только из планировщика (main.cpp). */
extern void mainDrainHelloFromScheduler(void);
/** Текущий mesh SF (main.cpp) — для powersave RX в radioSchedulerTask. */
extern uint8_t getDiscoverySf(void);
/** V4: после TX на SF12 — следующий RX слот на SF12. Вызывать из radioSchedulerTask. */
extern void onRadioSchedulerTxSf12(void);

// Очереди + таски: обычно поднимаются в boot (main runBootStateMachine); иначе — при первом queueSend.
static bool s_asyncInfraReady = false;
static SemaphoreHandle_t s_asyncInfraMux = nullptr;

bool asyncInfraEnsure() {
  if (s_asyncInfraReady) return true;
  if (!s_asyncInfraMux) {
    s_asyncInfraMux = xSemaphoreCreateMutex();
    if (!s_asyncInfraMux) return false;
  }
  if (xSemaphoreTake(s_asyncInfraMux, portMAX_DELAY) != pdTRUE) return false;
  if (s_asyncInfraReady) {
    xSemaphoreGive(s_asyncInfraMux);
    return true;
  }
  if (!asyncQueuesInit()) {
    RIFTLINK_LOG_ERR("[RiftLink] Async queues init FAILED (lazy)\n");
    xSemaphoreGive(s_asyncInfraMux);
    return false;
  }
  asyncTasksStart();
  radio::setAsyncMode(true);
  displaySetButtonPolledExternally(true);
  s_asyncInfraReady = true;
  Serial.printf("[RiftLink] Async infra OK, heap=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  xSemaphoreGive(s_asyncInfraMux);
  return true;
}

#define ACK_RESERVE_SLOTS 4   // резерв для ACK — обычные пакеты отклоняются при ≤4 свободных
bool queueSend(const uint8_t* buf, size_t len, uint8_t txSf, bool priority,
    char* reasonBuf, size_t reasonLen) {
  if (!asyncInfraEnsure()) {
    queueSendReason(reasonBuf, reasonLen, "async_infra");
    RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=async_infra priority=%u len=%u",
        (unsigned)priority, (unsigned)len);
    return false;
  }
  if (!radioCmdQueue) {
    queueSendReason(reasonBuf, reasonLen, "no_send_queue");
    RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=no_send_queue priority=%u len=%u",
        (unsigned)priority, (unsigned)len);
    return false;
  }
  if (len > PACKET_BUF_SIZE) {
    queueSendReason(reasonBuf, reasonLen, "pkt_oversize");
    RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=pkt_oversize priority=%u len=%u max=%u",
        (unsigned)priority, (unsigned)len, (unsigned)PACKET_BUF_SIZE);
    return false;
  }
  // txSf: 7–12 от radio::send (SF этого пакета; иначе там подставлен getSpreadingFactor()).
  uint8_t fixedSf = txSf;
  if (fixedSf < 7 || fixedSf > 12) fixedSf = radio::getSpreadingFactor();
  if (fixedSf < 7 || fixedSf > 12) fixedSf = 7;
  if (!priority && uxQueueSpacesAvailable(radioCmdQueue) <= ACK_RESERVE_SLOTS) {
    if (send_overflow::push(buf, len, fixedSf, false)) {
      RIFTLINK_DIAG("QUEUE", "event=TX_DEFER lane=overflow cause=ack_reserve sf=%u len=%u free=%u",
          (unsigned)fixedSf, (unsigned)len, (unsigned)uxQueueSpacesAvailable(radioCmdQueue));
      return true;
    }
    queueSendReason(reasonBuf, reasonLen, "overflow_norm_full");
    RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=overflow_norm_full sf=%u len=%u free=%u",
        (unsigned)fixedSf, (unsigned)len, (unsigned)uxQueueSpacesAvailable(radioCmdQueue));
    return false;
  }
  RadioCmd rcmd;
  rcmd.type = RadioCmdType::Tx;
  rcmd.priority = priority;
  memcpy(rcmd.u.tx.buf, buf, len);
  rcmd.u.tx.len = (uint16_t)len;
  rcmd.u.tx.txSf = fixedSf;
  BaseType_t ok = priority ? xQueueSendToFront(radioCmdQueue, &rcmd, 0) : xQueueSend(radioCmdQueue, &rcmd, 0);
  if (ok != pdTRUE) {
    if (send_overflow::push(buf, len, fixedSf, priority)) {
      RIFTLINK_DIAG("QUEUE", "event=TX_DEFER lane=overflow cause=send_queue_busy priority=%u sf=%u len=%u",
          (unsigned)priority, (unsigned)fixedSf, (unsigned)len);
      return true;
    }
    queueSendReason(reasonBuf, reasonLen, priority ? "sendq_pri_ovfl_full" : "sendq_ovfl_full");
    RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=%s sf=%u len=%u",
        priority ? "sendq_pri_ovfl_full" : "sendq_ovfl_full", (unsigned)fixedSf, (unsigned)len);
    return false;
  }
  RIFTLINK_DIAG("QUEUE", "event=TX_ENQUEUE_OK priority=%u sf=%u len=%u free=%u",
      (unsigned)priority, (unsigned)fixedSf, (unsigned)len, (unsigned)uxQueueSpacesAvailable(radioCmdQueue));
  return true;
}

#define DEFERRED_ACK_SLOTS 8   // broadcast: несколько соседей шлют ACK почти одновременно
#define DEFERRED_SEND_SLOTS 16   // MSG copy2–3, broadcast 2–3, KEY_EXCHANGE — burst 1–15
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
  if (!radioCmdQueue) return;
  uint32_t now = millis();
  for (int i = 0; i < n; i++) {
    if (slots[i].used && slots[i].sendAfter <= now) {
      if (slots[i].isRelay && relayHeardCheckAndRemove(slots[i].relayFrom, slots[i].relayHash)) {
        slots[i].used = false;
        continue;
      }
      RadioCmd rcmd;
      rcmd.type = RadioCmdType::Tx;
      rcmd.priority = true;
      memcpy(rcmd.u.tx.buf, slots[i].buf, slots[i].len);
      rcmd.u.tx.len = slots[i].len;
      uint8_t fixedSf = radio::getSpreadingFactor();
      rcmd.u.tx.txSf = (fixedSf >= 7 && fixedSf <= 12) ? fixedSf : 7;
      if (xQueueSendToFront(radioCmdQueue, &rcmd, 0) == pdTRUE) {
        slots[i].used = false;
      }
    }
  }
}

void queueDeferredAck(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs) {
  (void)asyncInfraEnsure();
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
  (void)asyncInfraEnsure();
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
    RIFTLINK_LOG_ERR("[RiftLink] deferred fallback radioCmdQueue full, drop copy\n");
  }
}

void queueDeferredRelay(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs,
    const uint8_t* from, uint32_t payloadHash) {
  (void)asyncInfraEnsure();
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
    RIFTLINK_LOG_ERR("[RiftLink] deferred relay fallback radioCmdQueue full, drop\n");
  }
}

void relayHeard(const uint8_t* from, uint32_t payloadHash) {
  memcpy(s_heardRelay[s_heardRelayIdx].from, from, protocol::NODE_ID_LEN);
  s_heardRelay[s_heardRelayIdx].hash = payloadHash;
  s_heardRelayIdx = (s_heardRelayIdx + 1) % HEARD_RELAY_SIZE;
}

void flushDeferredSends() {
  if (!s_asyncInfraReady) return;
  // Pull on-demand: radio scheduler тянет из send_overflow при пустой/недоступной radioCmdQueue для Tx
  // Сначала deferred send — потом ACK. SendToFront: последний добавленный впереди. ACK впереди = доставка.
  ack_coalesce::flush();
  flushDeferredSlots(s_deferredSend, DEFERRED_SEND_SLOTS);
  flushDeferredSlots(s_deferredAck, DEFERRED_ACK_SLOTS);
}

void queueDisplayLastMsg(const char* fromHex, const char* text) {
  (void)asyncInfraEnsure();
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
  (void)asyncInfraEnsure();
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
  (void)asyncInfraEnsure();
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
  (void)asyncInfraEnsure();
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
  (void)asyncInfraEnsure();
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

static TaskHandle_t s_packetTaskHandle = nullptr;
static TaskHandle_t s_displayTaskHandle = nullptr;
static TaskHandle_t s_radioSchedulerTaskHandle = nullptr;

TaskHandle_t asyncGetRadioSchedulerTaskHandle(void) {
  return s_radioSchedulerTaskHandle;
}

void asyncMemoryDiagLogStacks(void) {
  // uxTaskGetStackHighWaterMark: минимум неиспользованного стека за всё время. На ESP32-Arduino размер в xTaskCreate — байты;
  // возвращаемое значение на практике сопоставимо с байтами неиспользованного хвоста (см. alloc ниже). 0 = нет задачи / overflow.
  unsigned wd = s_displayTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(s_displayTaskHandle) : 0U;
  unsigned wp = s_packetTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(s_packetTaskHandle) : 0U;
  unsigned wr = s_radioSchedulerTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(s_radioSchedulerTaskHandle) : 0U;
  Serial.printf("[RiftLink] Task stack high-water (unused / alloc B): display=%u/%u packet=%u/%u radio=%u/%u\n",
      wd, (unsigned)DISPLAY_TASK_STACK, wp, (unsigned)PACKET_TASK_STACK, wr, (unsigned)RADIO_SCHEDULER_STACK);
  if (!s_packetTaskHandle) {
    RIFTLINK_LOG_ERR("[RiftLink] packetTask не создан — xTaskCreate FAIL? Нужен heap под стек %u B (см. PACKET_TASK_STACK)\n",
        (unsigned)PACKET_TASK_STACK);
  }
}

#if defined(USE_EINK)
static SemaphoreHandle_t s_displaySpiGranted = nullptr;
static SemaphoreHandle_t s_displaySpiDone = nullptr;
static std::atomic<bool> s_displaySpiRequested{false};

bool asyncRequestDisplaySpiSession(TickType_t timeoutTicks) {
  if (!s_displaySpiGranted || !s_displaySpiDone) {
    return false;  // вызывающий делает один takeMutex (legacy)
  }
  s_displaySpiRequested.store(true, std::memory_order_release);
  TaskHandle_t h = s_radioSchedulerTaskHandle;
  if (h) xTaskNotifyGive(h);
  if (xSemaphoreTake(s_displaySpiGranted, timeoutTicks) != pdTRUE) {
    s_displaySpiRequested.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

void asyncSignalDisplaySpiSessionDone(void) {
  if (s_displaySpiDone) xSemaphoreGive(s_displaySpiDone);
}
#endif

static TickType_t s_packetRxDropLogTick = 0;

/** Доставка RX из планировщика в packetTask или прямой handlePacket (общая логика normal + powersave RX). */
static void deliverRxToPacketQueue(uint8_t* rxBuf, int n, int rssi, uint8_t sf) {
  if (n <= 0) return;
  if (packetQueue) {
    PacketQueueItem pitem;
    if ((size_t)n <= sizeof(pitem.buf)) {
      memcpy(pitem.buf, rxBuf, (size_t)n);
      pitem.len = (uint16_t)n;
      pitem.rssi = (int8_t)rssi;
      pitem.sf = sf;
      bool isHello = (n == 13 && rxBuf[0] == protocol::SYNC_BYTE && rxBuf[2] == protocol::OP_HELLO);
      auto tryEnqueueSpill = [&]() {
        bool added = false;
        PacketQueueItem discarded;
        if (xQueueReceive(packetQueue, &discarded, 0) != pdTRUE) return false;
        bool frontWasHello = (discarded.len == 13 && discarded.buf[0] == protocol::SYNC_BYTE &&
            discarded.buf[2] == protocol::OP_HELLO);
        if (isHello) {
          added = (xQueueSendToFront(packetQueue, &pitem, 0) == pdTRUE);
        } else if (frontWasHello) {
          added = (xQueueSend(packetQueue, &pitem, 0) == pdTRUE);
        }
        if (!added) (void)xQueueSendToFront(packetQueue, &discarded, 0);
        return added;
      };
      BaseType_t ok = isHello ? xQueueSendToFront(packetQueue, &pitem, pdMS_TO_TICKS(30))
                              : xQueueSend(packetQueue, &pitem, pdMS_TO_TICKS(30));
      if (ok != pdTRUE && !tryEnqueueSpill()) {
        TickType_t t = xTaskGetTickCount();
        if (t - s_packetRxDropLogTick >= pdMS_TO_TICKS(5000)) {
          s_packetRxDropLogTick = t;
          uint8_t op = (n > 2 && rxBuf[0] == protocol::SYNC_BYTE) ? rxBuf[2] : 0xFF;
          RIFTLINK_DIAG("QUEUE", "event=RX_DROP queue=packetQueue len=%u rssi=%d sf=%u op=0x%02X",
              (unsigned)n, rssi, (unsigned)sf, (unsigned)op);
          RIFTLINK_LOG_ERR("[RiftLink] packetQueue full, drop\n");
        }
      }
    }
  } else {
    handlePacket(rxBuf, (size_t)n, rssi, sf);
  }
}

/** Radio scheduler: один владелец радио, чередует RX и TX. Нет конкуренции drain vs rx. */
static void radioSchedulerTask(void* arg) {
  static uint8_t rxBuf[protocol::SYNC_LEN + protocol::HEADER_LEN + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
  static uint32_t lastRxAliveLog = 0;
#if !defined(SF_FORCE_7)
  static int highSfDrained = 0;  // счётчик подряд отправленных SF10+ — макс 2, иначе RX не успевает
#endif
  vTaskDelay(pdMS_TO_TICKS(500));  // дать setup завершиться
  uint32_t lastQueueDiagMs = 0;
  for (;;) {
    uint32_t nowLoop = millis();
    if (nowLoop - lastQueueDiagMs >= 30000) {
      lastQueueDiagMs = nowLoop;
      UBaseType_t txFree = radioCmdQueue ? uxQueueSpacesAvailable(radioCmdQueue) : 0;
      UBaseType_t txUsed = radioCmdQueue ? uxQueueMessagesWaiting(radioCmdQueue) : 0;
      UBaseType_t rxFree = packetQueue ? uxQueueSpacesAvailable(packetQueue) : 0;
      UBaseType_t rxUsed = packetQueue ? uxQueueMessagesWaiting(packetQueue) : 0;
      RIFTLINK_DIAG("QUEUE", "event=QUEUE_SNAPSHOT tx_waiting=%u tx_free=%u rx_waiting=%u rx_free=%u heap=%u",
          (unsigned)txUsed, (unsigned)txFree, (unsigned)rxUsed, (unsigned)rxFree,
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    flushDeferredSends();
    mainDrainHelloFromScheduler();
#if defined(USE_EINK)
    if (s_displaySpiGranted && s_displaySpiDone &&
        s_displaySpiRequested.load(std::memory_order_acquire)) {
      if (radio::takeMutex(pdMS_TO_TICKS(200)) == pdTRUE) {
        s_displaySpiRequested.store(false, std::memory_order_release);
        send_overflow::drainApplyCommandsFromRadioQueue();
        radio::setRxListenActive(false);
        radio::standbyChipUnderMutex();
        radio::releaseMutex();
        xSemaphoreGive(s_displaySpiGranted);
        if (xSemaphoreTake(s_displaySpiDone, pdMS_TO_TICKS(120000)) != pdTRUE) {
          RIFTLINK_LOG_ERR("[RiftLink] E-Ink SPI: session end timeout (scheduler)\n");
        }
      }
    }
#endif
    if (powersave::canSleep()) {
      if (radio::takeMutex(pdMS_TO_TICKS(100)) == pdTRUE) {
        send_overflow::drainApplyCommandsFromRadioQueue();
        uint8_t dsf = getDiscoverySf();
        radio::applyHardwareSpreadingFactor(dsf);
        radio::startReceiveWithTimeout(1000);
        radio::setRxListenActive(true);
        radio::releaseMutex();
        powersave::lightSleepWake();
        int n = 0;
        if (radio::takeMutex(pdMS_TO_TICKS(200)) == pdTRUE) {
          n = radio::receiveAsync(rxBuf, sizeof(rxBuf));
          radio::setRxListenActive(false);
          radio::releaseMutex();
          uint32_t nowPs = millis();
          if (nowPs - lastRxAliveLog >= RX_ALIVE_LOG_INTERVAL_MS) {
            lastRxAliveLog = nowPs;
            Serial.printf("[RiftLink] RX alive heap=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
          }
          if (n > 0) {
            int rssi = radio::getLastRssi();
            deliverRxToPacketQueue(rxBuf, n, rssi, dsf);
          }
        } else {
          radio::setRxListenActive(false);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (radio::takeMutex(pdMS_TO_TICKS(200)) != pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    send_overflow::drainApplyCommandsFromRadioQueue();
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
        // KEY_EXCHANGE раньше откладывали на 0.5–2 с — на столе это давало поздний TX и лишние коллизии с HELLO.
        lastToaUs = radio::getTimeOnAir(item.len);
        radio::sendDirectInternal(item.buf, item.len);
        didTx = true;
        if (item.buf[0] == protocol::SYNC_BYTE && item.buf[2] == protocol::OP_MSG) {
          vTaskDelay(pdMS_TO_TICKS(60));
        }
#else
        if (item.txSf >= 10 && highSfDrained >= 2) {
          RadioCmd rcmd;
          rcmd.type = RadioCmdType::Tx;
          rcmd.priority = true;
          memcpy(rcmd.u.tx.buf, item.buf, item.len);
          rcmd.u.tx.len = (uint16_t)item.len;
          rcmd.u.tx.txSf = item.txSf;
          xQueueSendToFront(radioCmdQueue, &rcmd, 0);
          pushedBack = true;
          break;  // не drain дальше — ждём RX
        } else {
          if (item.txSf >= 7 && item.txSf <= 12) {
            radio::applyHardwareSpreadingFactor(item.txSf);
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
#endif
    }  // for drainIdx
    if (didTx && lastToaUs > 0) {
      uint32_t toaMs = (lastToaUs + 999) / 1000;
      if (toaMs > 0 && toaMs < 500) vTaskDelay(pdMS_TO_TICKS(toaMs));
    }
#if !defined(SF_FORCE_7)
    if (!didTx && !pushedBack) highSfDrained = 0;  // очередь пуста — сброс. pushedBack — не сбрасывать
#endif
    // RX: mutex не держим на vTaskDelay — иначе E-Ink/CAD/msg_queue залипают на сотни мс.
    uint8_t sf;
    uint32_t slotMs;
    getNextRxSlotParams(&sf, &slotMs);
    radio::applyHardwareSpreadingFactor(sf);
    radio::startReceiveWithTimeout(slotMs);
    radio::setRxListenActive(true);
    radio::releaseMutex();
    vTaskDelay(pdMS_TO_TICKS(slotMs));
    if (radio::takeMutex(pdMS_TO_TICKS(800)) != pdTRUE) {
      radio::setRxListenActive(false);
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    int n = radio::receiveAsync(rxBuf, sizeof(rxBuf));
    radio::setRxListenActive(false);
    radio::releaseMutex();
    uint32_t now = millis();
    if (now - lastRxAliveLog >= RX_ALIVE_LOG_INTERVAL_MS) {
      lastRxAliveLog = now;
      Serial.printf("[RiftLink] RX alive heap=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    if (n > 0) {
      int rssi = radio::getLastRssi();
      deliverRxToPacketQueue(rxBuf, n, rssi, sf);
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
      unsigned wm = (unsigned)uxTaskGetStackHighWaterMark(nullptr);
      if (wm < 512) {
        Serial.printf("[displayTask] stack watermark LOW: %u B\n", wm);
      }
    }
  }
}

#if !defined(USE_EINK)
/**
 * OLED: packetTask — самый большой стек; при PSRAM сначала стек в SPIRAM (не ест contiguous internal),
 * иначе после BLE/Wi‑Fi часто largest < 16K и xTaskCreate падает.
 * Нужен CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY в sdkconfig (у Heltec V4 с PSRAM обычно включён).
 */
static BaseType_t createPacketTaskOled() {
#if defined(RIFTLINK_HAVE_TASK_CREATE_WITH_CAPS)
  const size_t psramTot = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (psramTot > 0) {
    BaseType_t ok = xTaskCreateWithCaps(packetTask, "packet", PACKET_TASK_STACK, nullptr,
        (UBaseType_t)PACKET_TASK_PRIO_EINK, &s_packetTaskHandle,
        (UBaseType_t)MALLOC_CAP_SPIRAM);
    if (ok == pdPASS) {
      Serial.printf("[RiftLink] packetTask: стек в SPIRAM (internal largest=%u)\n",
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      return ok;
    }
    Serial.printf("[RiftLink] packetTask: SPIRAM stack не удался — fallback internal (largest=%u)\n",
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }
#endif
  return xTaskCreate(packetTask, "packet", PACKET_TASK_STACK, nullptr, PACKET_TASK_PRIO_EINK, &s_packetTaskHandle);
}
#endif

#if defined(USE_EINK)
/** Paper: стек packetTask в SPIRAM, чтобы не съедать ~32K internal (как на OLED). Иначе после wifi::init free падает. */
static BaseType_t createPacketTaskPaper() {
#if defined(RIFTLINK_HAVE_TASK_CREATE_WITH_CAPS)
  const size_t psramTot = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (psramTot > 0) {
    BaseType_t ok = xTaskCreateWithCaps(packetTask, "packet", PACKET_TASK_STACK, nullptr,
        (UBaseType_t)PACKET_TASK_PRIO_EINK, &s_packetTaskHandle, (UBaseType_t)MALLOC_CAP_SPIRAM);
    if (ok == pdPASS) {
      Serial.printf("[RiftLink] packetTask (Paper): стек в SPIRAM, internal largest=%u\n",
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      return ok;
    }
    Serial.printf("[RiftLink] packetTask (Paper): SPIRAM stack не удался — fallback internal (largest=%u)\n",
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }
#endif
  return xTaskCreatePinnedToCore(packetTask, "packet", PACKET_TASK_STACK, nullptr, PACKET_TASK_PRIO_EINK,
      &s_packetTaskHandle, 1);
}
#endif

void asyncTasksStart() {
  static bool s_asyncTasksStarted = false;
  if (s_asyncTasksStarted) return;
#if defined(USE_EINK)
  if (!s_displaySpiGranted) s_displaySpiGranted = xSemaphoreCreateBinary();
  if (!s_displaySpiDone) s_displaySpiDone = xSemaphoreCreateBinary();
#endif
#if defined(USE_EINK)
  // Paper: сначала стек packet в SPIRAM (если есть) — освобождает ~32K internal; иначе fallback internal + core 1.
  BaseType_t okPacket = createPacketTaskPaper();
  if (okPacket == pdFAIL) {
    RIFTLINK_LOG_ERR("[RiftLink] packetTask create FAIL — loop будет обрабатывать packetQueue\n");
  }
  BaseType_t okDisplay = xTaskCreatePinnedToCore(displayTask, "display", DISPLAY_TASK_STACK, nullptr, DISPLAY_TASK_PRIO,
      &s_displayTaskHandle, 0);
#else
  BaseType_t okPacket = createPacketTaskOled();
  if (okPacket == pdFAIL) {
    RIFTLINK_LOG_ERR("[RiftLink] packetTask create FAIL (need %u) — loop будет обрабатывать packetQueue\n",
        (unsigned)PACKET_TASK_STACK);
  }
  BaseType_t okDisplay = xTaskCreate(displayTask, "display", DISPLAY_TASK_STACK, nullptr, DISPLAY_TASK_PRIO, &s_displayTaskHandle);
#endif
  if (okDisplay == pdFAIL) {
    RIFTLINK_LOG_ERR("[RiftLink] displayTask create FAIL (need %u)\n", (unsigned)DISPLAY_TASK_STACK);
  }
#if defined(USE_EINK)
  BaseType_t okRx = xTaskCreatePinnedToCore(radioSchedulerTask, "radio", RADIO_SCHEDULER_STACK, nullptr, RADIO_SCHEDULER_PRIO,
      &s_radioSchedulerTaskHandle, 1);
#else
  BaseType_t okRx = xTaskCreate(radioSchedulerTask, "radio", RADIO_SCHEDULER_STACK, nullptr, RADIO_SCHEDULER_PRIO,
      &s_radioSchedulerTaskHandle);
#endif
  if (okRx == pdFAIL) {
    RIFTLINK_LOG_ERR("[RiftLink] radioSchedulerTask create FAIL\n");
  }
  s_asyncTasksStarted = true;
}
