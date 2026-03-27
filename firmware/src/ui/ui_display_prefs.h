/**
 * Настройки отображения: переворот 180°, таймаут скрытия полоски вкладок (NVS).
 */
#pragma once

#include <cstdint>

namespace ui_display_prefs {

void init();

bool getFlip180();
/** Сохраняет в NVS. Вызывающий обязан вызвать displayApplyRotationFromPrefs() и перерисовку. */
bool setFlip180(bool on);

/** Секунды простоя до скрытия полоски вкладок: только 5, 10 или 15. */
uint8_t getTabBarHideIdleSeconds();
bool setTabBarHideIdleSeconds(uint8_t seconds);
/** Цикл: 5 с → 10 с → 15 с → 5 с. */
void cycleTabBarHideIdleSeconds();

}  // namespace ui_display_prefs
