/**
 * Единый пул plain+cipher для голосового конвейера (BLE → mesh).
 * Mutex защищает регион от гонки BLE packetTask vs обработка VOICE_MSG.
 *
 * Контракт:
 * - voice_frag::send() вызывать только при уже удержанном mutex (путь BLE).
 * - После успешного voice_frag::onFragment(...): вызвать voice_buffers_release()
 *   после notifyVoice (plaintext в voice_buffers_plain()).
 */

#pragma once

#include <cstddef>
#include <cstdint>

bool voice_buffers_init();
void voice_buffers_deinit();

/** Захват mutex и ленивое выделение пула. false при OOM. */
bool voice_buffers_acquire();

/** Освободить mutex (после использования plain/cipher регионов). */
void voice_buffers_release();

size_t voice_buffers_plain_cap();
size_t voice_buffers_cipher_cap();

uint8_t* voice_buffers_plain();
uint8_t* voice_buffers_cipher();
