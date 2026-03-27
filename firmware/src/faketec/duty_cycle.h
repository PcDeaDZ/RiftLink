/**
 * RiftLink Duty Cycle — ограничение времени в эфире (EU ETSI)
 * EU/UK: 1% = 36 с/час на канале (как firmware/src/duty_cycle/).
 */

#pragma once

#include <cstdint>

namespace duty_cycle {

bool canSend(uint32_t durationUs);
void recordSend(uint32_t durationUs);
void reset();

}  // namespace duty_cycle
