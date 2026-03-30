/** Меню nRF: паритет UI с `display.cpp` / `display_tabs` (Heltec V3/V4). */
#pragma once

#include <cstdint>

void nrf_render_dashboard(uint8_t page);

void menu_nrf_init();

/**
 * После init: как ESP drawScreen(0) — в режиме списка CT_HOME (главное меню с иконками),
 * во вкладках — первая вкладка (MAIN и т.д.). Не путать с Serial status/dash (трёхстраничный телеметрийный экран).
 */
void menu_nrf_show_boot_screen();

/** T114: таймер скрытия полоски вкладок (как displayUpdate + ui_tab_bar_idle на ESP). */
void menu_nrf_tab_idle_tick();

/** T114: если изменились данные для текущего экрана (дайджест), перерисовать; без таймера «вхолостую». */
void menu_nrf_periodic_refresh(uint32_t now_ms);

/** T114: вызывать из loop с текущим состоянием кнопки (LOW = нажата). */
void menu_nrf_poll_t114_button(bool pressed, uint32_t now_ms);

/** Serial: открыть главное меню (FakeTech без физической кнопки). */
void menu_nrf_open_menu();

/** Serial: следующая страница дашборда (0..2). */
void menu_nrf_dashboard_next_page();

uint8_t menu_nrf_dashboard_page();

void menu_nrf_set_dashboard_page(uint8_t page);

/** Serial status/dash: три страницы телеметрии (0 RF / 1 mesh / 2 bat) — на ESP такого экрана нет; только nRF Serial. */
void menu_nrf_goto_dashboard(uint8_t page);

/** После locale::setLang (Serial/BLE): перерисовать текущий экран меню/дашборда. */
void menu_nrf_redraw_after_locale();
