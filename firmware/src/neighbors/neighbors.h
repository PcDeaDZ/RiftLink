/**
 * RiftLink Neighbors — список узлов, видимых по HELLO
 * Онлайн-статус: до 8 соседей
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

#define NEIGHBORS_MAX 8
#define NEIGHBOR_TIMEOUT_MS 60000  // 1 мин — узел считается офлайн

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
/** Средний RSSI активных соседей (dBm). -90 если нет данных */
int getAverageRssi();
/** Записать ID соседа i (0..getCount()-1) в out. Возвращает false если i неверный */
bool getId(int i, uint8_t* out);
/** Записать ID в hex-строку (17 байт с \0) */
void getIdHex(int i, char* out);

}  // namespace neighbors
