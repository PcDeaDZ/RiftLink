/**
 * BLE Layer — GATT сервис для Flutter
 * JSON: {"cmd":"send","text":"..."} / {"evt":"msg","from":"...","text":"..."}
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace ble {

bool init();
void update();  // вызов из loop()

void setOnSend(void (*cb)(const uint8_t* to, const char* text, uint8_t ttlMinutes));
void setOnLocation(void (*cb)(float lat, float lon, int16_t alt));
void notifyMsg(const uint8_t* from, const char* text, uint32_t msgId = 0, int rssi = 0, uint8_t ttlMinutes = 0);
void notifyDelivered(const uint8_t* from, uint32_t msgId, int rssi = 0);
void notifyRead(const uint8_t* from, uint32_t msgId, int rssi = 0);
/** evt "sent" — unicast поставлен в очередь (to, msgId) */
void notifySent(const uint8_t* to, uint32_t msgId);
void notifyLocation(const uint8_t* from, float lat, float lon, int16_t alt, int rssi = 0);
void notifyTelemetry(const uint8_t* from, uint16_t batteryMv, uint16_t heapKb, int rssi = 0);
void notifyInfo();
/** evt "invite" — id + pubKey (base64) для QR приглашения */
void notifyInvite();
void notifyNeighbors();  // evt "neighbors" — список соседей
void notifyRoutes();    // evt "routes" — маршруты (dest, nextHop, hops, rssi) для mesh-визуализации
void notifyGroups();    // evt "groups" — список групп
void notifyOta(const char* ip, const char* ssid, const char* password);
void notifyWifi(bool connected, const char* ssid, const char* ip);
void notifyRegion(const char* code, float freq, int power, int channel = -1);
void notifyGps(bool present, bool enabled, bool hasFix, int rx, int tx, int en);
void notifyPong(const uint8_t* from, int rssi = 0);
/** Результат самотестирования: radioOk, displayOk, batteryMv, heapFree */
void notifySelftest(bool radioOk, bool displayOk, uint16_t batteryMv, uint32_t heapFree);
/** Голосовое сообщение: from, data (Opus), dataLen. Отправляется чанками base64 */
void notifyVoice(const uint8_t* from, const uint8_t* data, size_t dataLen);
/** evt "error" — уведомить приложение о сбое (code, msg) */
void notifyError(const char* code, const char* msg);
bool isConnected();

}  // namespace ble
