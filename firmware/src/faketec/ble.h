/**
 * FakeTech BLE — Nordic UART GATT (Bluefruit52Lib BLEUart, UUID как в docs/API.md)
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace ble {

bool init();
void update();

void setOnSend(void (*cb)(const uint8_t* to, const char* text, uint8_t ttlMinutes));
/** Как ble.h (Heltec): координаты с телефона → шифрованный OP_LOCATION в эфир (GNSS на плате нет). */
void setOnLocation(void (*cb)(float lat, float lon, int16_t alt, uint16_t radiusM, uint32_t expiryEpochSec));
void notifyMsg(const uint8_t* from, const char* text, uint32_t msgId = 0, int rssi = 0, uint8_t ttlMinutes = 0);
void notifyTelemetry(const uint8_t* from, uint16_t batteryMv, uint16_t heapKb, int rssi = 0);
void notifyLocation(const uint8_t* from, float lat, float lon, int16_t alt, int rssi = 0);
void notifySent(const uint8_t* to, uint32_t msgId);
void notifyUndelivered(const uint8_t* to, uint32_t msgId);
void notifyBroadcastDelivery(uint32_t msgId, int delivered, int total);
void notifyTimeCapsuleReleased(const uint8_t* to, uint32_t msgId, uint8_t triggerType);
void notifyDelivered(const uint8_t* from, uint32_t msgId, int rssi = 0);
void notifyRead(const uint8_t* from, uint32_t msgId, int rssi = 0);
void notifyPong(const uint8_t* from, int rssi = 0, uint16_t pingPktId = 0);
void clearPingRetryForPeer(const uint8_t* from);
/** Волна node→neighbors→routes→groups; [cmdId] повторяется из cmd:info для TransportResponseRouter. */
void notifyInfo(uint32_t cmdId = 0);
void notifyNeighbors(uint32_t cmdId = 0);
void notifyRoutes(uint32_t cmdId = 0);
void notifyGroups(uint32_t cmdId = 0);
/** createInvite: ответ evt:invite с тем же cmdId; ttlSec из cmd (по умолчанию 600). */
void notifyInvite(uint32_t cmdId = 0, int ttlSec = 600);
/** setGps: ответ evt:gps с cmdId (на FakeTech нет модуля GPS). */
void notifyGps(bool present, bool enabled, bool hasFix, uint32_t cmdId = 0);
void notifySelftest(bool radioOk, bool displayOk, uint16_t batteryMv, uint32_t heapFree, uint32_t cmdId = 0);
void notifyError(const char* code, const char* msg, uint32_t cmdId = 0);
void notifyRelayProof(const uint8_t* relayedBy, const uint8_t* from, const uint8_t* to, uint16_t pktId, uint8_t opcode);
bool isConnected();

}  // namespace ble
