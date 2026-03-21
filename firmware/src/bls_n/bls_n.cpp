/**
 * BLS-N — BLE-LoRa Slot Negotiation
 * RTS через manufacturer data в BLE advertising (когда не подключён телефон).
 * Приём RTS — через BLE scan (когда подключён телефон, advertising остановлен).
 */

#include "bls_n.h"

#if !defined(RIFTLINK_DISABLE_BLS_N)

#include "ble/ble.h"
#include "node/node.h"
#include <NimBLEDevice.h>
#include <Arduino.h>
#include <string.h>

#define BLS_RTS_COMPANY_ID 0x524C  // "RL" — RiftLink
#define BLS_RTS_CACHE_MAX 4
#define BLS_RTS_TTL_MS 3000       // RTS действителен 3 с
#define BLS_RTS_DEFER_OVERLAP_MS 500  // не передавать если RTS в пределах 500 ms

namespace bls_n {

struct RtsEntry {
  uint8_t from[protocol::NODE_ID_LEN];
  uint8_t to[protocol::NODE_ID_LEN];
  uint16_t len;
  uint32_t txAt;
  uint32_t receivedAt;
};

static RtsEntry s_rtsCache[BLS_RTS_CACHE_MAX];
static uint8_t s_rtsCount = 0;

void init() {
  memset(s_rtsCache, 0, sizeof(s_rtsCache));
  s_rtsCount = 0;
}

/** Добавить RTS в кэш (вызывается из BLE scan callback) */
void addReceivedRts(const uint8_t* from, const uint8_t* to, uint16_t len, uint32_t txAt) {
  if (s_rtsCount >= BLS_RTS_CACHE_MAX) {
    // Вытеснить самый старый
    memmove(&s_rtsCache[0], &s_rtsCache[1], (BLS_RTS_CACHE_MAX - 1) * sizeof(RtsEntry));
    s_rtsCount = BLS_RTS_CACHE_MAX - 1;
  }
  RtsEntry* e = &s_rtsCache[s_rtsCount];
  memcpy(e->from, from, protocol::NODE_ID_LEN);
  memcpy(e->to, to, protocol::NODE_ID_LEN);
  e->len = len;
  e->txAt = txAt;
  e->receivedAt = millis();
  s_rtsCount++;
}

/** Очистить устаревшие RTS */
static void pruneRtsCache() {
  uint32_t now = millis();
  uint8_t w = 0;
  for (uint8_t r = 0; r < s_rtsCount; r++) {
    if (now - s_rtsCache[r].receivedAt < BLS_RTS_TTL_MS) {
      if (w != r) s_rtsCache[w] = s_rtsCache[r];
      w++;
    }
  }
  s_rtsCount = w;
}

bool sendRtsBeforeLora(const uint8_t* to, size_t payloadLen) {
  if (!to || ble::isConnected()) return false;  // При подключённом телефоне не рекламируем

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  if (!pAdv || !pAdv->isAdvertising()) return false;

  const uint8_t* from = node::getId();
  uint32_t txAt = millis() + 60;  // LoRa TX через ~60 ms

  uint8_t mfrData[2 + 3 + protocol::NODE_ID_LEN + protocol::NODE_ID_LEN + 2 + 4];
  mfrData[0] = BLS_RTS_COMPANY_ID & 0xFF;
  mfrData[1] = (BLS_RTS_COMPANY_ID >> 8) & 0xFF;
  mfrData[2] = 0x52;  // 'R' — маркер RTS
  mfrData[3] = 0x54;  // 'T'
  mfrData[4] = 0x53;  // 'S'
  memcpy(mfrData + 5, from, protocol::NODE_ID_LEN);
  memcpy(mfrData + 5 + protocol::NODE_ID_LEN, to, protocol::NODE_ID_LEN);
  size_t lenOff = 5 + protocol::NODE_ID_LEN + protocol::NODE_ID_LEN;
  mfrData[lenOff + 0] = (payloadLen >> 8) & 0xFF;
  mfrData[lenOff + 1] = payloadLen & 0xFF;
  mfrData[lenOff + 2] = (txAt >> 24) & 0xFF;
  mfrData[lenOff + 3] = (txAt >> 16) & 0xFF;
  mfrData[lenOff + 4] = (txAt >> 8) & 0xFF;
  mfrData[lenOff + 5] = txAt & 0xFF;

  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN);
  advData.addServiceUUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
  if (!advData.setManufacturerData(mfrData, sizeof(mfrData))) return false;

  NimBLEAdvertisementData scanData;
  char advName[20];
  snprintf(advName, sizeof(advName), "RL-%02X%02X%02X%02X%02X%02X%02X%02X",
      from[0], from[1], from[2], from[3], from[4], from[5], from[6], from[7]);
  scanData.setName(advName);

  pAdv->setAdvertisementData(advData);
  pAdv->setScanResponseData(scanData);
  return true;
}

bool shouldDeferTx(const uint8_t* to) {
  pruneRtsCache();
  uint32_t now = millis();
  for (uint8_t i = 0; i < s_rtsCount; i++) {
    const RtsEntry* e = &s_rtsCache[i];
    if (now - e->receivedAt > BLS_RTS_TTL_MS) continue;
    // Кто-то передаёт в [txAt, txAt+overlap]. Не передавать в этот интервал.
    if (now >= (uint32_t)(e->txAt - 50) && now <= e->txAt + BLS_RTS_DEFER_OVERLAP_MS)
      return true;
  }
  return false;
}

}  // namespace bls_n

#endif /* !RIFTLINK_DISABLE_BLS_N */
