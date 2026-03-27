/**
 * Единое выполнение действий подменю SYS / NET / GPS (без отрисовки).
 */
#pragma once

struct UiDisplayHooks {
  void (*show_modem_picker)();
  void (*run_modem_scan)();
  void (*show_region_picker)();
  void (*show_language_picker)();
  void (*run_selftest)();
};

namespace ui_menu_exec {

/** sel: 0..4 главное меню SYS; язык — в подменю «Дисплей». */
void exec_sys_menu(int sel, const UiDisplayHooks& hooks);
/** NET: 0 = переключить WiFi/BLE. */
void exec_net_menu(int sel);
/** GPS: 0 = toggle при наличии модуля. */
void exec_gps_menu(int sel);

}  // namespace ui_menu_exec
