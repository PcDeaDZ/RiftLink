#include "ui_menu_exec.h"
#include "gps/gps.h"
#include "powersave/powersave.h"
#include "radio_mode/radio_mode.h"
#include "selftest/selftest.h"

void ui_menu_exec::exec_sys_menu(int sel, const UiDisplayHooks& hooks) {
  if (sel < 0 || sel > 4) return;
  switch (sel) {
    case 0: hooks.show_modem_picker(); break;
    case 1: hooks.run_modem_scan(); break;
    case 2: powersave::setEnabled(!powersave::isEnabled()); break;
    case 3: hooks.show_region_picker(); break;
    case 4: hooks.run_selftest(); break;
    default: break;
  }
}

void ui_menu_exec::exec_net_menu(int sel) {
  if (sel != 0) return;
  bool isBle = (radio_mode::current() == radio_mode::BLE);
  radio_mode::switchTo(isBle ? radio_mode::WIFI : radio_mode::BLE);
}

void ui_menu_exec::exec_gps_menu(int sel) {
  if (sel == 0 && gps::isPresent()) gps::toggle();
}
