/**
 * Таб-режим: автоскрытие полоски вкладок по неактивности; первое короткое нажатие только возвращает вкладки.
 */
#pragma once

namespace ui_tab_bar_idle {

void init();

/** Вызов из displayUpdate: таймер скрытия; при drill вкладки не скрываются. */
void tick(bool tabDrillIn);

/** Любая осмысленная активность (кнопка short/long, энкодер). */
void onInput();

/** Полоска вкладок (иконки) видна и сдвигает контент. */
bool tabStripVisible();

/** Первое короткое при скрытой полоске: только показать вкладки. Возвращает true, если событие поглощено. */
bool tryRevealFirstShortOnly();

}  // namespace ui_tab_bar_idle
