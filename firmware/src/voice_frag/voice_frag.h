/**
 * RiftLink — голосовые сообщения (VOICE_MSG)
 * Формат как MSG_FRAG: msgId, part, total, data. Без сжатия (Opus уже сжат).
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

namespace voice_frag {

/** Макс. размер сжатого Opus в одном VOICE_MSG (байт). Синхронизировать с приложением. */
constexpr size_t MAX_VOICE_PLAIN = 8192;
/** ceil(max_cipher / FRAG_DATA_MAX), FRAG_DATA_MAX=194, max_cipher≈12+plain+16. */
constexpr size_t MAX_FRAGMENTS = 43;
constexpr uint8_t VOICE_PROFILE_FAST = 1;
constexpr uint8_t VOICE_PROFILE_BALANCED = 2;
constexpr uint8_t VOICE_PROFILE_RESILIENT = 3;

/** Отправка голоса (unicast). data = Opus, plainLen ≤ MAX_VOICE_PLAIN.
 *  Вызывать только при удерживаемом voice_buffers mutex (путь BLE после voice_buffers_acquire).
 *  outMsgId — если не null, записывает назначенный msgId (для evt:sent). */
bool send(const uint8_t* to, const uint8_t* data, size_t dataLen, uint32_t* outMsgId = nullptr);

/**
 * Обработка входящего фрагмента VOICE_MSG. Возвращает true если собрано и расшифровано.
 * Расшифровка в voice_buffers_plain(); при успехе вызывающий обязан вызвать voice_buffers_release().
 * out может быть nullptr — plaintext всегда в voice_buffers_plain().
 * outMsgId — если не null, записывает msgId собранного голоса (для ACK).
 */
bool onFragment(const uint8_t* from, const uint8_t* to, const uint8_t* payload, size_t payloadLen,
                uint8_t* out, size_t outMaxLen, size_t* outLen, uint32_t* outMsgId = nullptr);

/** Проверить, является ли msgId ожидающим голосовым ACK.
 *  При совпадении удаляет из pending и возвращает true + заполняет peerOut. */
bool matchAck(const uint8_t* from, uint32_t msgId);

/** Явный ранний init (необязательно): иначе поднимется при первом send/onFragment */
void init();
/** Освободить незавершённые reassembly-буферы и сбросить состояние (после deinit снова lazy-init) */
void deinit();

}  // namespace voice_frag
