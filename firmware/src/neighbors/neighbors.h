/**
 * RiftLink Neighbors — список узлов, видимых по HELLO
 * Онлайн-статус: до 8 соседей
 * Thread-safe: mutex защищает доступ из packetTask, loopTask, displayTask
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

#define NEIGHBORS_MAX 16  // 10+ узлов — без вытеснения
// HELLO у 1 соседа после «прогрева» может быть до 30 с + jitter; мало окно RX — запас против ложного OFF.
#define NEIGHBOR_TIMEOUT_MS 180000  // 3 мин без пакетов от узла — офлайн

namespace neighbors {

void init();
/** Добавить/обновить соседа по HELLO. rssi в dBm (0 = неизвестно). Возвращает true если список изменился */
bool onHello(const uint8_t* nodeId, int rssi = 0);
/** Обновить RSSI для узла (если уже в списке) */
void updateRssi(const uint8_t* nodeId, int rssi);
/** Получить число активных соседей */
int getCount();
/** RSSI последнего пакета от соседа i (dBm), 0 если неизвестно */
int getRssi(int i);
/** RSSI для конкретного узла (dBm). -128 если не найден */
int getRssiFor(const uint8_t* nodeId);
/** Минимальный RSSI среди активных соседей (dBm). 0 если нет данных */
int getMinRssi();
/** Средний RSSI активных соседей (dBm). -90 если нет данных */
int getAverageRssi();
/** Записать ID соседа i (0..getCount()-1) в out. Возвращает false если i неверный */
bool getId(int i, uint8_t* out);
/** Записать ID в hex-строку (17 байт с \0) */
void getIdHex(int i, char* out);
/** Узел считается online, если есть активный слот и timeout не истёк. */
bool isOnline(const uint8_t* nodeId);
/** Возраст последнего пакета от узла в мс; UINT32_MAX если узел неизвестен/offline. */
uint32_t getFreshnessMs(const uint8_t* nodeId);
/** Обновить батарею узла по входящей telemetry (mV). */
void updateBattery(const uint8_t* nodeId, uint16_t batteryMv);
/** Батарея узла (mV), 0 если неизвестно. */
int getBatteryMv(const uint8_t* nodeId);
/** Учёт попыток/успешных ACK для trust score. */
void recordAckSent(const uint8_t* nodeId);
void recordAckReceived(const uint8_t* nodeId);
/** ACK-rate * 1000 (0..1000), -1 если данных нет. */
int getAckRatePermille(const uint8_t* nodeId);

/** RSSI → SF 7–12 для per-neighbor TX. -128/0 → 0 (default) */
inline uint8_t rssiToSf(int rssi) {
  if (rssi <= -128 || rssi > 0) return 0;
  if (rssi >= -75) return 7;
  if (rssi >= -85) return 9;
  if (rssi >= -95) return 10;
  if (rssi >= -105) return 11;
  return 12;
}

/** SF-orthogonality: разные SF соседям для параллельной передачи. 2+ соседей → hash(nodeId)%6. */
uint8_t rssiToSfOrthogonal(const uint8_t* nodeId);

}  // namespace neighbors
