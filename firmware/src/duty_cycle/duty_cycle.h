/**
 * RiftLink Duty Cycle — ограничение времени в эфире (EU ETSI)
 * EU: 1% = 36 сек/час на канале
 */

#pragma once

#include <cstdint>

namespace duty_cycle {

/** Можно ли отправить пакет с данной длительностью (мкс)? */
bool canSend(uint32_t durationUs);

/** Записать отправку (после успешного TX) */
void recordSend(uint32_t durationUs);

/** Сброс при смене региона */
void reset();

}  // namespace duty_cycle
