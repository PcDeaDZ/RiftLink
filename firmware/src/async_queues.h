/**
 * Async Queues — очереди для межзадачного обмена
 * Packet, Send, Display
 */

#pragma once

#include <Arduino.h>
#include "protocol/packet.h"
#include "crypto/crypto.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Размер буфера пакета: sync + header + payload + crypto overhead
constexpr size_t PACKET_BUF_SIZE = protocol::SYNC_LEN + protocol::HEADER_LEN + protocol::MAX_PAYLOAD + crypto::OVERHEAD;

struct PacketQueueItem {
  uint8_t buf[PACKET_BUF_SIZE];
  uint16_t len;
  int8_t rssi;  // RSSI на момент приёма — radio может быть перезаписан до обработки
  uint8_t sf;   // SF на момент приёма (7–12)
};

struct SendQueueItem {
  uint8_t buf[PACKET_BUF_SIZE];
  size_t len;
  uint8_t txSf;  // 0 = текущий baseSf, 7–12 = принудительный SF (per-neighbor)
};

/** Единая очередь команд радио (актор): TX + отложенное применение региона/SF. */
enum class RadioCmdType : uint8_t { Tx = 0, ApplyRegion = 1, ApplySf = 2 };

struct RadioCmd {
  RadioCmdType type;
  bool priority;  // только для Tx (очередь в начало)
  union {
    struct {
      uint8_t buf[PACKET_BUF_SIZE];
      uint16_t len;
      uint8_t txSf;
    } tx;
    struct {
      uint32_t freqHz;
      int32_t power;
    } region;
    struct {
      uint8_t sf;
    } spread;
  } u;
};

// Команды отображения
constexpr uint8_t CMD_REDRAW_SCREEN = 1;
constexpr uint8_t CMD_SET_LAST_MSG = 2;
constexpr uint8_t CMD_REQUEST_INFO_REDRAW = 3;
constexpr uint8_t CMD_LONG_PRESS = 4;
constexpr uint8_t CMD_WAKE = 5;
constexpr uint8_t CMD_BLINK_LED = 6;

struct DisplayQueueItem {
  uint8_t cmd;
  uint8_t screen;
  char fromHex[17];
  char text[64];
};

extern QueueHandle_t packetQueue;
extern QueueHandle_t radioCmdQueue;  // RadioCmd: TX + ApplyRegion + ApplySf
extern QueueHandle_t displayQueue;

bool asyncQueuesInit();
