/**
 * RiftLink — фрагментация длинных сообщений (MSG_FRAG)
 * Схема: Сжатие → Шифрование → Разбиение на фрагменты
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

namespace frag {

// v3: payload = MsgID(4) + Part(1) + Total(1) + FragDataLen(1) + Flags(1) + Data
constexpr size_t FRAG_HEADER_LEN = 8;
constexpr size_t FRAG_DATA_MAX = protocol::MAX_PAYLOAD - FRAG_HEADER_LEN;  // ~194
constexpr size_t MAX_MSG_PLAIN = 2048;   // макс. plaintext до сжатия
// Must fit into FRAG_SLOT_BUF (see frag.cpp), otherwise hostile payload can overflow bookkeeping.
constexpr size_t MAX_FRAGMENTS = 21;

// Отправка длинного сообщения (фрагментация)
bool send(const uint8_t* to, const uint8_t* plain, size_t plainLen, bool compressed);

// Обработка входящего фрагмента. Возвращает true если сообщение собрано полностью.
// compressed — флаг из заголовка пакета (payload до расшифровки был сжат)
bool onFragment(const uint8_t* from, const uint8_t* to, const uint8_t* payload, size_t payloadLen,
                bool compressed, uint8_t* out, size_t outMaxLen, size_t* outLen, uint32_t* outMsgId);
// v3 selective fragment control: receiver bitmap of delivered parts.
void onFragCtrl(const uint8_t* from, uint32_t msgId, uint8_t total, uint32_t receivedMask);

void init();

}  // namespace frag
