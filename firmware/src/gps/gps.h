/**
 * RiftLink GPS — включение/выключение, пины, NMEA
 * По образцу Meshtastic: PIN_GPS_EN, GPS_RX, GPS_TX
 * Heltec V3: EN=46, RX=48, TX=47
 * Heltec V4: EN=45, RX=44, TX=43 (46 занят LoRa FEM)
 */

#pragma once

#include <cstdint>

namespace gps {

void init();
void update();  // вызов из loop — парсинг NMEA

bool isPresent();   // пины настроены (не -1)
bool isEnabled();   // питание включено
void setEnabled(bool on);
void toggle();      // переключить питание

bool hasFix();
float getLat();
float getLon();
int16_t getAlt();
uint32_t getSatellites();   // 0 если нет данных
float getCourseDeg();       // 0..360, -1 если нет (направление движения)
const char* getCourseCardinal();  // "N","NE","E" и т.д., "" если нет

/** Пины: rx=наш RX (принимаем от GPS TX), tx=наш TX, en=power (-1=не используется) */
void setPins(int rx, int tx, int en);
void getPins(int* rx, int* tx, int* en);

/** Сохранить конфиг в NVS */
void saveConfig();

/** Beacon-sync + GPS: данные от телефона по BLE (gps_sync). UTC ms, lat, lon, alt */
void setPhoneSync(int64_t utcMs, float lat, float lon, int16_t alt);

/** Есть ли данные от телефона (для beacon с timestamp) */
bool hasPhoneSync();

}  // namespace gps
