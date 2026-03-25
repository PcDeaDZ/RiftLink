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

/** Единая очередь команд радио (актор): TX + отложенное применение региона/SF/modem. */
enum class RadioCmdType : uint8_t { Tx = 0, ApplyRegion = 1, ApplySf = 2, ApplyModem = 3 };

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
    struct {
      uint8_t preset;   // radio::ModemPreset
      uint8_t sf;       // для CUSTOM
      uint16_t bw10;    // BW * 10 (1250 = 125.0 kHz)
      uint8_t cr;       // 5–8
    } modem;
  } u;
};

enum class TxRequestClass : uint8_t {
  critical = 0,
  control = 1,
  data = 2,
  voice = 3,
};

struct TxRequest {
  uint8_t buf[PACKET_BUF_SIZE];
  uint16_t len = 0;
  uint8_t txSf = 0;
  bool priority = false;
  TxRequestClass klass = TxRequestClass::data;
  uint32_t enqueueMs = 0;
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

extern QueueHandle_t packetQueue;      // PacketQueueItem* (pointer-based)
extern QueueHandle_t radioCmdQueue;    // RadioCmd: TX + ApplyRegion + ApplySf (copy-based, small items OK)
extern QueueHandle_t displayQueue;

#include "ptr_pool.h"

inline constexpr size_t PACKET_POOL_SIZE = 16;
inline constexpr size_t TX_REQUEST_POOL_SIZE = 48;

extern PtrPool<PacketQueueItem, PACKET_POOL_SIZE> packetPool;
extern PtrPool<TxRequest, TX_REQUEST_POOL_SIZE> txRequestPool;

bool asyncQueuesInit();
