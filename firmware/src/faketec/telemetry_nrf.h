#pragma once

#include <cstdint>

namespace telemetry_nrf {

void init();
/** Периодический OP_TELEMETRY (broadcast), паритет с telemetry::send на ESP. */
void tick();

}  // namespace telemetry_nrf
