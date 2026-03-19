/**
 * FakeTech Node — Node ID, никнейм
 */

#pragma once

#include "protocol/packet.h"

namespace node {

void init();
const uint8_t* getId();
void getIdCopy(uint8_t* out);
bool isForMe(const uint8_t* to);
bool isBroadcast(const uint8_t* to);
void setNickname(const char* nick);
const char* getNickname();

}  // namespace node
