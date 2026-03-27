/**
 * FakeTech Region — EU, RU, US
 */

#pragma once

namespace region {

void init();
bool isSet();
bool setRegion(const char* code);
const char* getCode();
float getFreq();
int getPower();
int getChannelCount();
int getChannel();
bool setChannel(int ch);
float getChannelMHz(int idx);
void switchChannelOnCongestion();
int getPresetCount();
const char* getPresetCode(int i);

}  // namespace region
