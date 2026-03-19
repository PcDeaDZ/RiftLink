/**
 * Differential Clock Drift — предсказание «тихого» момента цикла соседа
 * NOVEL_DELIVERY_IDEAS_2025.md §9.9
 *
 * При приёме HELLO: записываем t_receive. HELLO без payload — оцениваем drift
 * по периодичности прихода. Сосед передаёт в начале своего слота.
 * «Тихий» момент = середина между его передачами (когда он вряд ли TX).
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

namespace clock_drift {

constexpr int MAX_NEIGHBORS = 8;
constexpr uint32_t HELLO_PERIOD_MS = 8000;   // период HELLO ~8с (beacon_sync цикл)
constexpr uint32_t SLOT_MS = 500;
constexpr int N_SLOTS = 16;

void init();

/** Записать приём HELLO от соседа (t_receive = millis). Вызывать при каждом HELLO. */
void onHelloReceived(const uint8_t* from);

/** Задержка (ms) до «тихого» окна соседа. 0 = уже тихо или неизвестный сосед. */
uint32_t getQuietWindowMs(const uint8_t* neighborId);

}  // namespace clock_drift
