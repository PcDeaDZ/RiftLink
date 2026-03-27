/**
 * RiftLink Region — региональные пресеты (EU, RU, US)
 * Частота, мощность TX, соответствие регуляторике
 */

#pragma once

namespace region {

/** Инициализация (загрузка из NVS), вызывать до radio::init() */
void init();

/** Регион задан (в NVS). false при первом буте. */
bool isSet();

/** Установить регион (EU, UK, RU, US, AU), сохранить в NVS, применить к радио */
bool setRegion(const char* code);

/** Текущий код региона (например "EU") */
const char* getCode();

/** Частота в MHz */
float getFreq();

/** Макс. мощность TX в dBm */
int getPower();

/** Число каналов для EU/UK (3 LoRaWAN), 0 для остальных регионов */
int getChannelCount();

/** Текущий канал (0–2 для EU/UK), 0 для остальных */
int getChannel();

/** Установить канал (0–2). Только для EU/UK. Возвращает true при успехе */
bool setChannel(int ch);

/** Частота несущей для канала idx (0..getChannelCount()-1) при EU/UK; иначе 0 */
float getChannelMHz(int idx);

/** Channel Hopping: при congestion переключить на следующий канал (EU 868.1→868.3→868.5). Rate-limited. */
void switchChannelOnCongestion();

/** Количество регионов для выбора при первом буте */
int getPresetCount();

/** Код региона по индексу (0..getPresetCount()-1) */
const char* getPresetCode(int i);

}  // namespace region
