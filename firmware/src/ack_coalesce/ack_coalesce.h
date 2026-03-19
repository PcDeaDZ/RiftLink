/**
 * ACK coalescing — буфер ACK по отправителю, flush как OP_ACK_BATCH (pipelining)
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace ack_coalesce {

void init();
void add(const uint8_t* from, uint32_t msgId, uint8_t txSf);
void flush();

}  // namespace ack_coalesce
