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

/** Красивый бут-скрин: RiftLink + иконка связи */
void displayShowBootScreen();
/** Выбор языка при первом буте. Блокирует до выбора. Возвращает true если выбран. */
bool displayShowLanguagePicker();
/** Выбор страны/региона при первом буте. Вызывать после region::init() и radio::init(). */
bool displayShowRegionPicker();

/** Меню: 0=Main, 1=Info, 2=WiFi, 3=Sys, 4=Msg, 5=Lang, 6=GPS. Вызывать после init. */
void displayShowScreen(int screen);
/** Смена вкладки с принудительным full refresh (против ghosting на e-ink). */
void displayShowScreenForceFull(int screen);
/** Текущая вкладка (для опроса кнопки в loopTask) */
int displayGetCurrentScreen();
/** Следующая вкладка при переключении (учитывает gps::isPresent — скрывает GPS если нет модуля) */
int displayGetNextScreen(int current);
/** Обработка long press на вкладке (picker, selftest, gps toggle) */
void displayOnLongPress(int screen);
/** Показать последнее сообщение (экран 3). */
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
/** Вызвать при поднятии async infra (lazy: asyncInfraEnsure) — кнопка в displayTask */
void displaySetButtonPolledExternally(bool on);
