/**
 * Полноценное меню nRF (структура как display_tabs на Heltec V3/V4): главный список,
 * экраны разделов, дашборд по короткому нажатию на T114.
 */
#pragma once

#include <cstdint>

void nrf_render_dashboard(uint8_t page);

void menu_nrf_init();

/** T114: таймер скрытия полоски вкладок (как displayUpdate + ui_tab_bar_idle на ESP). */
void menu_nrf_tab_idle_tick();

/** T114: вызывать из loop с текущим состоянием кнопки (LOW = нажата). */
void menu_nrf_poll_t114_button(bool pressed, uint32_t now_ms);

/** Serial: открыть главное меню (FakeTech без физической кнопки). */
void menu_nrf_open_menu();

/** Serial: следующая страница дашборда (0..2). */
void menu_nrf_dashboard_next_page();

uint8_t menu_nrf_dashboard_page();

void menu_nrf_set_dashboard_page(uint8_t page);

/** Serial status/dash: показать дашборд и сбросить состояние меню (кнопка T114 соответствует экрану). */
void menu_nrf_goto_dashboard(uint8_t page);

/** После locale::setLang (Serial/BLE): перерисовать текущий экран меню/дашборда. */
void menu_nrf_redraw_after_locale();
