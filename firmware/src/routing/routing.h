/**
 * RiftLink Routing — ROUTE_REQ/REPLY, таблица маршрутов
 * AODV-подобный проактивный поиск маршрута
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "protocol/packet.h"

#define ROUTING_MAX_ROUTES 16
#define ROUTING_MAX_REVERSE 8
#define ROUTING_MAX_SEEN 16
#define ROUTING_ROUTE_TTL_MS 120000   // 2 мин — маршрут устаревает
#define ROUTING_REVERSE_TTL_MS 30000  // 30 сек — обратный маршрут
#define ROUTING_SEEN_TTL_MS 10000     // 10 сек — дубликаты ROUTE_REQ

namespace routing {

void init();

/** Получить next_hop для dest. Возвращает true если маршрут есть */
bool getNextHop(const uint8_t* dest, uint8_t* nextHopOut);

/** Получить маршрут: nextHop, hops, rssi (dBm). Возвращает true если маршрут есть */
bool getRoute(const uint8_t* dest, uint8_t* nextHopOut, uint8_t* hopsOut, int8_t* rssiOut);

/** Число активных маршрутов */
int getRouteCount();

/** Записать маршрут i (0..getRouteCount()-1): dest, nextHop, hops, rssi. Возвращает false если i неверный */
bool getRouteAt(int i, uint8_t* destOut, uint8_t* nextHopOut, uint8_t* hopsOut, int8_t* rssiOut);

/** Запросить маршрут до target (отправить ROUTE_REQ) */
void requestRoute(const uint8_t* target);

/** Обработка входящего ROUTE_REQ. Возвращает true если пакет обработан */
bool onRouteReq(const uint8_t* from, const uint8_t* payload, size_t payloadLen);

/** Обработка входящего ROUTE_REPLY. Возвращает true если пакет обработан */
bool onRouteReply(const uint8_t* from, const uint8_t* to, const uint8_t* payload, size_t payloadLen);

/** Обновление: истечение устаревших записей */
void update();

}  // namespace routing
