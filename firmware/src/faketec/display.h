/**
 * FakeTech Display — OLED автоопределение
 */

#pragma once

#include <cstdint>

namespace display {

bool init();   // auto-detect I2C OLED
bool isPresent();
void clear();
void setCursor(int c, int r);
void print(const char* s);
void print(int v);
void show();

}  // namespace display
