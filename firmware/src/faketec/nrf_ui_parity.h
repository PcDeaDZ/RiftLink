/**
 * Паритет локального UI с ESP при загрузке: язык, регион, предупреждения (см. main.cpp ESP runBootStateMachine).
 * T114 — кнопка (short/long как в menu_nrf); FakeTech — Serial s/l и цифры.
 */
#pragma once

/** Пока locale не сохранён в KV — выбор EN/RU (как displayShowLanguagePicker). */
void nrf_ui_run_language_until_done();

/** Пока region не сохранён в KV — выбор пресета и при EU/UK канала (как displayShowRegionPicker). После radio::init. */
void nrf_ui_run_region_until_done();
