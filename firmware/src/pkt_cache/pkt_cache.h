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

/** Overhearing: кэш подслушанного пакета from→to (не для нас). */
void addOverheard(const uint8_t* from, const uint8_t* to, uint16_t pktId, const uint8_t* pkt, size_t len);

/** По NACK от nackFrom к nackTo с pktId — retransmit из overhear cache если есть. */
bool retransmitOverheard(const uint8_t* nackFrom, const uint8_t* nackTo, uint16_t pktId);

/** Packet Fusion: batch с несколькими pktId — retransmit при NACK любого */
void addBatch(const uint8_t* to, const uint16_t* pktIds, int count, const uint8_t* pkt, size_t len);
/** Проверить batch cache в retransmitOnNack */
bool retransmitBatchOnNack(const uint8_t* from, uint16_t pktId);

}  // namespace pkt_cache
