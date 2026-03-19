/**
 * Network Coding XOR — ретранслятор шлёт A⊕B вместо A и B
 * Получатель с A декодирует B, с B — декодирует A.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

namespace network_coding {

void init();

/** Добавить пакет в кэш для XOR. Возвращает true если есть пара (можно отправить XOR). */
bool addForXor(const uint8_t* pkt, size_t len, const uint8_t* from, const uint8_t* to);

/** Получить XOR-пакет для relay. lenOut — длина. Вызывать после addForXor==true. */
bool getXorPacket(uint8_t* out, size_t maxOut, size_t* lenOut);

/** Инфо о «другом» пакете пары — для relayHeard (отмена pending relay). */
void getLastPairOther(uint8_t* fromOut, uint32_t* payloadHashOut);

/** При приёме OP_XOR_RELAY: попытка декодировать. Возвращает true если decoded — вызвать handlePacket. */
bool onXorRelayReceived(const uint8_t* buf, size_t len, uint8_t* decodedOut, size_t* decodedLenOut);

/** После addForXor: если есть pending XOR с этим пакетом — декодировать другой. */
bool getDecodedFromPending(const uint8_t* pkt, size_t len, const uint8_t* from, const uint8_t* to, uint16_t pktId,
                           uint8_t* decodedOut, size_t* decodedLenOut);

}  // namespace network_coding
