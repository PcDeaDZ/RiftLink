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
#define NEIGHBOR_TIMEOUT_MS 120000  // 2 мин — узел считается офлайн (HELLO каждые 10с)

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
