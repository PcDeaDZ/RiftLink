/**
 * Node — Node ID, генерация, хранение в NVS
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

namespace node {

void init();
const uint8_t* getId();
void getIdCopy(uint8_t* out);
bool isForMe(const uint8_t* to);
bool isBroadcast(const uint8_t* to);

/** Никнейм (до 16 символов), пустая строка = не задан */
void getNickname(char* out, size_t maxLen);
bool setNickname(const char* name);

}  // namespace node
