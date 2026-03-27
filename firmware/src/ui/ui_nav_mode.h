/**
 * Режим навигации UI: вертикальный список (HOME) или полоска вкладок + переключение разделов.
 * Хранение в NVS (riftlink / ui_nav).
 */
#pragma once

namespace ui_nav_mode {

void init();
bool isTabMode();
/** true = сохранено в NVS */
bool setTabMode(bool tabs);

}  // namespace ui_nav_mode
