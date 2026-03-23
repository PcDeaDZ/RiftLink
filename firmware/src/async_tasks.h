/**
 * Async Tasks — packetTask, displayTask
 * Запуск из setup()
 */

#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include "async_queues.h"

/** Очереди + задачи + async radio/display — по первому использованию (экономия heap на старте). */
bool asyncInfraEnsure();

void asyncTasksStart();
/** Для Paper / отладки: задача `radioSchedulerTask` (nullptr до asyncTasksStart). */
TaskHandle_t asyncGetRadioSchedulerTaskHandle(void);
/** True если packetTask успешно создан и обрабатывает packetQueue. */
bool asyncHasPacketTask(void);

/** Фаза 0: watermark стеков display/packet/radio (слова FreeRTOS, чем больше — тем больше запас). */
void asyncMemoryDiagLogStacks(void);

#if defined(USE_EINK)
/**
 * Paper (EINK_USE_GLOBAL_SPI): запрос паузы радио — планировщик в standby, затем семафор «разрешено».
 * После `true` вызвать `radio::takeMutex` и SPI; в конце `releaseMutex` + `asyncSignalDisplaySpiSessionDone`.
 * Если семафоры не созданы — возвращает `false` (вызывающий делает один `takeMutex` как раньше).
 */
bool asyncRequestDisplaySpiSession(TickType_t timeoutTicks);
void asyncSignalDisplaySpiSessionDone(void);
#endif

/** txSf: 0 = baseSf, 7–12 = per-neighbor SF. priority=true → xQueueSendToFront.
 *  При отказе: reasonBuf — короткая причина (async / очередь / overflow). */
bool queueSend(const uint8_t* buf, size_t len, uint8_t txSf = 0, bool priority = false,
    char* reasonBuf = nullptr, size_t reasonLen = 0);

enum class TxRequestClass : uint8_t {
  critical = 0,
  control = 1,
  data = 2,
  voice = 3,
};

struct TxRequest {
  uint8_t buf[PACKET_BUF_SIZE];
  uint16_t len = 0;
  uint8_t txSf = 0;
  bool priority = false;
  TxRequestClass klass = TxRequestClass::data;
  uint32_t enqueueMs = 0;
};

/** Единая постановка TX-запроса в пайплайн Radio FSM (v2) или legacy scheduler (fallback). */
bool queueTxRequest(const TxRequest& req, char* reasonBuf = nullptr, size_t reasonLen = 0);
/** Удобный helper для существующих мест: автоматически собирает TxRequest. */
bool queueTxPacket(const uint8_t* buf, size_t len, uint8_t txSf, bool priority, TxRequestClass klass,
    char* reasonBuf = nullptr, size_t reasonLen = 0);

/** ACK с задержкой — без блокировки packetTask, RX window для следующего MSG */
void queueDeferredAck(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs = 50, bool applyAsym = true);
/** Любой пакет с задержкой — MSG copy2, broadcast 2–3, KEY_EXCHANGE jitter */
void queueDeferredSend(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs, bool applyAsym = true);
/** Relay с задержкой — Managed flooding: отмена при услышанной ретрансляции */
void queueDeferredRelay(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs,
    const uint8_t* from, uint32_t payloadHash, bool applyAsym = true);
/** Уведомить: услышали ретрансляцию (from+hash) — отменить наш pending relay */
void relayHeard(const uint8_t* from, uint32_t payloadHash);
/** Переносит готовые deferred ACK/send в radioCmdQueue (RadioCmd::Tx). Вызывается из radioSchedulerTask. */
void flushDeferredSends();

/** Поставить CMD_SET_LAST_MSG в displayQueue */
void queueDisplayLastMsg(const char* fromHex, const char* text);

/** Поставить CMD_REDRAW_SCREEN в displayQueue. priority=true → в начало очереди (смена вкладки кнопкой) */
void queueDisplayRedraw(uint8_t screen, bool priority = false);

/** Поставить CMD_REQUEST_INFO_REDRAW в displayQueue */
void queueDisplayRequestInfoRedraw();

/** Поставить CMD_LONG_PRESS в displayQueue (screen = текущая вкладка) */
void queueDisplayLongPress(uint8_t screen);
/** Поставить CMD_WAKE в displayQueue (пробуждение из sleep) */
void queueDisplayWake();
/** Поставить CMD_BLINK_LED — мигание без блокировки loop */
void queueDisplayLedBlink();

/** Текущее состояние основной TX-очереди (s_txRequestQueue). */
uint8_t asyncTxQueueFree();
uint8_t asyncTxQueueWaiting();

/** Feature flag Radio FSM v2 (continuous RX + arbiter). */
bool asyncIsRadioFsmV2Enabled();
void asyncSetRadioFsmV2Enabled(bool enabled);
