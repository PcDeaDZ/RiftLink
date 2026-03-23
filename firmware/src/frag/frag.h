/**
 * RiftLink — фрагментация длинных сообщений (MSG_FRAG)
 * Схема: Сжатие → Шифрование → Разбиение на фрагменты
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

namespace frag {

// Макс. размер данных в одном фрагменте (payload = MsgID 4 + Part 1 + Total 1 + Data)
constexpr size_t FRAG_HEADER_LEN = 6;  // MsgID 4 + Part 1 + Total 1
constexpr size_t FRAG_DATA_MAX = protocol::MAX_PAYLOAD - FRAG_HEADER_LEN;  // ~194
constexpr size_t MAX_MSG_PLAIN = 2048;   // макс. plaintext до сжатия
constexpr size_t MAX_FRAGMENTS = 32;     // макс. фрагментов на сообщение

// Отправка длинного сообщения (фрагментация)
bool send(const uint8_t* to, const uint8_t* plain, size_t plainLen, bool compressed);

// Обработка входящего фрагмента. Возвращает true если сообщение собрано полностью.
// compressed — флаг из заголовка пакета (payload до расшифровки был сжат)
bool onFragment(const uint8_t* from, const uint8_t* to, const uint8_t* payload, size_t payloadLen,
                bool compressed, uint8_t* out, size_t outMaxLen, size_t* outLen, uint32_t* outMsgId);

void init();

}  // namespace frag
