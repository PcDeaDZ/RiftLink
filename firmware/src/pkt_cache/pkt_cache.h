/**
 * Packet Cache — кэш отправленных пакетов с pktId для retransmit по NACK
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

namespace pkt_cache {

void init();

/** Добавить пакет в кэш. to + pktId — ключ. */
void add(const uint8_t* to, uint16_t pktId, const uint8_t* pkt, size_t len);

/** По NACK от from с pktId — retransmit если в кэше. Возвращает true если отправлено. */
bool retransmitOnNack(const uint8_t* from, uint16_t pktId);

}  // namespace pkt_cache
