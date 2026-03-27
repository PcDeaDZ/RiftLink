/**
 * Очередь TX для nRF (один loop): замена queueTxPacket / queueDeferredSend из async_tasks (ESP).
 * Параметр txSf в API сохранён для совместимости вызовов; фактический SF — `radio::getMeshTxSfForQueue()` (как на ESP).
 */

#pragma once

#include <cstddef>
#include <cstdint>

enum class TxRequestClass : uint8_t {
  critical = 0,
  control = 1,
  data = 2,
  voice = 3,
};

bool queueTxPacket(const uint8_t* buf, size_t len, uint8_t txSf, bool priority, TxRequestClass klass,
    char* reasonBuf = nullptr, size_t reasonLen = 0);

void queueDeferredSend(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs, bool applyAsym = true);

/** Как async_tasks::queueDeferredAck на ESP: отложенный ACK/control (без асимметрии TX на nRF). */
void queueDeferredAck(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs = 50, bool applyAsym = true);

/** Relay с задержкой — отмена при relayHeard (услышали чужую ретрансляцию). */
void queueDeferredRelay(const uint8_t* pkt, size_t len, uint8_t txSf, uint32_t delayMs, const uint8_t* from,
    uint32_t payloadHash, bool applyAsym = true);

void relayHeard(const uint8_t* from, uint32_t payloadHash);

/** Вызывать из loop() — отправка отложенных и дренаж очереди. */
void asyncTxPoll();
