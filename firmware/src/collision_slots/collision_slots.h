/**
 * Predictive Slot Avoidance — гистограмма коллизий по времени
 * collision_times[slot]++ при len=13. При выборе задержки — избегать «шумных» слотов.
 */

#pragma once

#include <cstdint>

namespace collision_slots {

constexpr int N_SLOTS = 32;
constexpr uint32_t SLOT_MS = 1000;

void init();

/** Записать коллизию в текущий слот (вызывать при truncated/NACK) */
void recordCollision();

/** Получить дополнительную задержку (ms) для избежания шумных слотов. 0 = без смещения */
uint32_t getAvoidanceDelayMs();

}  // namespace collision_slots
