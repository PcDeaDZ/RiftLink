/**
 * RiftLink — голосовые сообщения (VOICE_MSG)
 * Формат как MSG_FRAG: msgId, part, total, data. Без сжатия (Opus уже сжат).
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

namespace voice_frag {

#if defined(USE_EINK)
constexpr size_t MAX_VOICE_PLAIN = 10240;   // Paper: ~10 KB (~10 сек), экономия heap
#else
constexpr size_t MAX_VOICE_PLAIN = 30720;   // ~30 KB (30 сек Opus 8 kbps)
#endif
constexpr size_t MAX_FRAGMENTS = 160;       // 30KB / 194

/** Отправка голоса (unicast). data = Opus, plainLen ≤ MAX_VOICE_PLAIN */
bool send(const uint8_t* to, const uint8_t* data, size_t dataLen);

/** Обработка входящего фрагмента VOICE_MSG. Возвращает true если собрано */
bool onFragment(const uint8_t* from, const uint8_t* to, const uint8_t* payload, size_t payloadLen,
                uint8_t* out, size_t outMaxLen, size_t* outLen);

void init();

}  // namespace voice_frag
