/**
 * RiftLink Display — OLED SSD1306
 * Heltec V3: SDA=17, SCL=18, RST=21
 */

#pragma once

#include <cstdint>

void displayInit();
void displayClear();
void displayText(int x, int y, const char* text);
void displayShow();
void displaySetTextSize(uint8_t s);

/** Цепочка загрузки при первом старте: doneCount заполненных шагов из totalSteps, строка статуса. */
void displayShowInitProgress(int doneCount, int totalSteps, const char* statusLine);

/** Красивый бут-скрин: RiftLink + иконка связи */
void displayShowBootScreen();
/** Выбор языка при первом буте. Блокирует до выбора. Возвращает true если выбран. */
bool displayShowLanguagePicker();
/** Выбор страны/региона при первом буте. Вызывать после region::init() и radio::init(). */
bool displayShowRegionPicker();

/** Меню: 0=Home, 1=Main, 2=Msg, 3=Peers, [GPS], Net, Sys(last). Вызывать после init. */
void displayShowScreen(int screen);
/** Смена экрана с принудительным full refresh (против ghosting на e-ink). */
void displayShowScreenForceFull(int screen);
/** Текущий экран (для опроса кнопки в loopTask) */
int displayGetCurrentScreen();
/** Короткое нажатие: на главном — следующий пункт меню; иначе — на главный экран. Возвращает индекс экрана (обычно 0). */
/** Короткое нажатие: возвращает индекс экрана после обработки (для queueDisplayRedraw в main). */
int displayHandleShortPress();
/** Обработка long press на экране (picker, selftest, gps toggle) */
void displayOnLongPress(int screen);
/** Установить последнее сообщение — обновит экран если Msg активен. */
void displaySetLastMsg(const char* fromHex, const char* text);
/** Обновить экран (периодически в loop). Возвращает true если кнопка нажата */
bool displayUpdate();
/** Запросить перерисовку вкладки Info (никнейм и т.п.) — после смены ника по BLE */
void displayRequestInfoRedraw();

/** Слип-мод: выключить дисплей до события или нажатия PRG. Вызывать при неактивности. */
void displaySleep();
/** Разбудить дисплей (включить и перерисовать). */
void displayWake();
/** Запросить пробуждение (вызывать при BLE connect, входящем сообщении и т.п.). */
void displayWakeRequest();
/** Дисплей в слипе? */
bool displayIsSleeping();
/** Popup-меню активно? pollButtonAndQueue() должна игнорировать кнопку. */
bool displayIsMenuActive();
/** Вызвать при поднятии async infra (lazy: asyncInfraEnsure) — кнопка в displayTask */
void displaySetButtonPolledExternally(bool on);
/** Показать предупреждение на экране на duration_ms (блокирующий). */
void displayShowWarning(const char* line1, const char* line2, uint32_t durationMs);
/** Результаты selftest в том же стиле, что подменю (chrome + заголовок). */
void displayShowSelftestSummary(bool radioOk, bool antennaOk, uint16_t batteryMv, uint32_t heapFree);

/** Таб-режим: первое короткое при скрытой полоске вкладок только показывает её (true = событие поглощено). */
bool displayTryRevealTabBarRowOnly();
/** Сброс таймера скрытия полоски вкладок (кнопка, энкодер). */
void displayNotifyTabChromeActivity();
/** Применить переворот экрана из NVS (после init и смены в настройках). Реализация в бэкенде дисплея. */
void displayApplyRotationFromPrefs();
