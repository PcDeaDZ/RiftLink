/**
 * BLE Layer — GATT сервис для Flutter
 * JSON: {"cmd":"send","text":"..."} / {"evt":"msg","from":"...","text":"..."}
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace ble {

bool init();
/** Полная деинициализация BLE: остановка advertising, NimBLE deinit, освобождение heap. */
void deinit();
void update();  // вызов из loop()

void setOnSend(void (*cb)(const uint8_t* to, const char* text, uint8_t ttlMinutes,
    bool critical, uint8_t triggerType, uint32_t triggerValueMs, bool isSos));
void setOnLocation(void (*cb)(float lat, float lon, int16_t alt, uint16_t radiusM, uint32_t expiryEpochSec));
void notifyMsg(const uint8_t* from, const char* text, uint32_t msgId = 0, int rssi = 0, uint8_t ttlMinutes = 0,
    const char* lane = "normal", const char* type = "text");
void requestMsgNotify(const uint8_t* from, const char* text, uint32_t msgId = 0, int rssi = 0, uint8_t ttlMinutes = 0,
    const char* lane = "normal", const char* type = "text");  // отложить — снизить стек в handlePacket
void notifyDelivered(const uint8_t* from, uint32_t msgId, int rssi = 0);
void notifyRead(const uint8_t* from, uint32_t msgId, int rssi = 0);
/** evt "sent" — unicast поставлен в очередь (to, msgId) */
void notifySent(const uint8_t* to, uint32_t msgId);
/** evt "waiting_key" — direct unicast отложен: ждём pairwise ключ с узлом */
void notifyWaitingKey(const uint8_t* to);
/** evt "undelivered" — ACK не получен после всех retry (to, msgId) */
void notifyUndelivered(const uint8_t* to, uint32_t msgId);
/** evt "broadcast_delivery" — delivered/total (msgId, delivered, total). При total>0 и delivered=0 — undelivered */
void notifyBroadcastDelivery(uint32_t msgId, int delivered, int total);
void notifyLocation(const uint8_t* from, float lat, float lon, int16_t alt, int rssi = 0);
void notifyTelemetry(const uint8_t* from, uint16_t batteryMv, uint16_t heapKb, int rssi = 0);
void notifyRelayProof(const uint8_t* relayedBy, const uint8_t* from, const uint8_t* to, uint16_t pktId, uint8_t opcode);
void notifyTimeCapsuleReleased(const uint8_t* to, uint32_t msgId, uint8_t triggerType);
void notifyInfo();
/** evt "invite" — id + pubKey (base64) для QR приглашения */
void notifyInvite();
void notifyNeighbors();  // evt "neighbors" — список соседей
void requestNeighborsNotify();  // отложить в update() — снизить стек в handlePacket
void notifyRoutes();    // evt "routes" — маршруты (dest, nextHop, hops, rssi) для mesh-визуализации
void notifyGroups();    // evt "groups" — список групп
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
/** Обработать JSON-команду (вызывается и из BLE onWrite, и из WebSocket). */
void processCommand(const uint8_t* data, size_t len);

/** 6-значный PIN для passkey pairing (отображается на экране). */
uint32_t getPasskey();
/** Перегенерировать PIN и сохранить в NVS. */
void regeneratePasskey();

}  // namespace ble
