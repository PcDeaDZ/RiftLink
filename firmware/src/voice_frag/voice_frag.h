/**
 * RiftLink — голосовые сообщения (VOICE_MSG)
 * Формат как MSG_FRAG: msgId, part, total, data. Без сжатия (Opus уже сжат).
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

namespace voice_frag {

constexpr size_t MAX_VOICE_PLAIN = 15360;   // ~15 KB (~15 сек Opus/AAC ~1 KB/s)
constexpr size_t MAX_FRAGMENTS = 85;        // ceil(16412 / 194)
constexpr uint8_t VOICE_PROFILE_FAST = 1;
constexpr uint8_t VOICE_PROFILE_BALANCED = 2;
constexpr uint8_t VOICE_PROFILE_RESILIENT = 3;

/** Отправка голоса (unicast). data = Opus, plainLen ≤ MAX_VOICE_PLAIN */
bool send(const uint8_t* to, const uint8_t* data, size_t dataLen);

/**
 * Обработка входящего фрагмента VOICE_MSG. Возвращает true если собрано.
 * Сборка: статический пул буферов на слот (BSS), без malloc на фрагмент.
 */
bool onFragment(const uint8_t* from, const uint8_t* to, const uint8_t* payload, size_t payloadLen,
                uint8_t* out, size_t outMaxLen, size_t* outLen);

/** Явный ранний init (необязательно): иначе поднимется при первом send/onFragment */
void init();
/** Освободить незавершённые reassembly-буферы и сбросить состояние (после deinit снова lazy-init) */
void deinit();

}  // namespace voice_frag
