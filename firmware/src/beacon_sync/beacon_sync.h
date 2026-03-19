/**
 * Beacon-sync — локальная синхронизация по beacon (arXiv:2401.15168)
 * Слот по hash(nodeId). При выборе задержки — избегать слотов соседей.
 */

#pragma once

#include <cstdint>
#include "protocol/packet.h"

namespace beacon_sync {

void init();

/** Слот узла (0..15) по hash(nodeId) */
uint8_t getSlotFor(const uint8_t* nodeId);

/** Доп. задержка (ms) для избежания слотов соседей. 0 = без смещения */
uint32_t getAvoidanceDelayMs();

/** Записать beacon от соседа (при HELLO) */
void onBeaconReceived(const uint8_t* from);

}  // namespace beacon_sync
