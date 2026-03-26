/**
 * ACK coalescing — буфер ACK по отправителю, flush как OP_ACK_BATCH (pipelining)
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace ack_coalesce {

void init();
void add(const uint8_t* from, uint32_t msgId, uint8_t txSf);
bool consumeForPeer(const uint8_t* peer, uint32_t* outIds, uint8_t maxIds, uint8_t* outCount);
bool hasPendingForPeer(const uint8_t* peer);
void flush();

}  // namespace ack_coalesce
