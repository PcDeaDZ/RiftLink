/**
 * RiftLink BLE — GATT сервис для Flutter
 * Протокол: JSON {"cmd":"send","text":"..."} / {"evt":"msg","from":"...","text":"..."}
 */

#include "ble.h"
#include <string.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include "protocol/packet.h"
#include "node/node.h"
#include "ota/ota.h"
#include "wifi/wifi.h"
#include "locale/locale.h"
#include "radio/radio.h"
#include "region/region.h"
#include "neighbors/neighbors.h"
#include "routing/routing.h"
#include "powersave/powersave.h"
#include "groups/groups.h"
#include "x25519_keys/x25519_keys.h"
#include "msg_queue/msg_queue.h"
#include "offline_queue/offline_queue.h"
#include "voice_frag/voice_frag.h"
#include "gps/gps.h"
#include "selftest/selftest.h"
#include "ui/display.h"
#include "async_tasks.h"
#include "version.h"
#include "bls_n/bls_n.h"
#include "crypto/crypto.h"
#include "esp_now_slots/esp_now_slots.h"
#include <mbedtls/base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define SERVICE_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHAR_TX_UUID        "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // write from app
#define CHAR_RX_UUID        "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // notify to app
#define DEVICE_NAME         "RiftLink"

static NimBLEServer* pServer = nullptr;
static NimBLECharacteristic* pRxChar = nullptr;
static bool s_connected = false;
// Отложенные ответы — тяжёлые notify вызываем из main loop, не из callback (Stack canary)
static volatile bool s_pendingInfo = false;
static volatile bool s_pendingGroups = false;
static volatile bool s_pendingRoutes = false;
static volatile bool s_pendingNeighbors = false;
static volatile bool s_pendingInvite = false;
static volatile bool s_pendingSelftest = false;
static volatile bool s_pendingGroupSend = false;
static uint32_t s_pendingGroupId = 0;
static char s_pendingGroupText[256] = {0};
static volatile bool s_pendingMsg = false;
static uint8_t s_pendingMsgFrom[8] = {0};
static char s_pendingMsgText[256] = {0};
static uint32_t s_pendingMsgId = 0;
static int s_pendingMsgRssi = 0;
static uint8_t s_pendingMsgTtl = 0;
static volatile bool s_pendingNickname = false;
static char s_pendingNicknameBuf[17] = {0};
static volatile bool s_pendingGps = false;
static bool s_pendingGpsHasEnabled = false;
static bool s_pendingGpsEnabled = false;
static bool s_pendingGpsHasPins = false;
static int s_pendingGpsRx = -1, s_pendingGpsTx = -1, s_pendingGpsEn = -1;
static void (*s_onSend)(const uint8_t* to, const char* text, uint8_t ttlMinutes) = nullptr;
static void (*s_onLocation)(float lat, float lon, int16_t alt) = nullptr;

#define VOICE_BUF_MAX (voice_frag::MAX_VOICE_PLAIN + 1024)
static uint8_t s_voiceBuf[VOICE_BUF_MAX];
static size_t s_voiceBufLen = 0;
static int s_voiceChunkTotal = -1;
static uint8_t s_voiceTo[protocol::NODE_ID_LEN];

// BLS-N: BLE scan для приёма RTS при подключённом телефоне
static bool s_blsScanActive = false;
static bool s_blsScanEnded = false;
static uint32_t s_blsScanLastStart = 0;
#define BLS_SCAN_DURATION_SEC 15
#define BLS_SCAN_RESTART_DELAY_MS 500

class BlsScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (!advertisedDevice->haveManufacturerData()) return;
    std::string mfr = advertisedDevice->getManufacturerData();
    if (mfr.size() < 19) return;
    const uint8_t* d = (const uint8_t*)mfr.data();
    if (d[0] != 0x4C || d[1] != 0x52) return;  // company ID 0x524C little-endian
    if (d[2] != 0x52 || d[3] != 0x54 || d[4] != 0x53) return;  // "RTS"
    uint8_t from4[4], to4[4];
    memcpy(from4, d + 5, 4);
    memcpy(to4, d + 9, 4);
    uint16_t len = (uint16_t)d[13] << 8 | d[14];
    uint32_t txAt = (uint32_t)d[15] << 24 | (uint32_t)d[16] << 16 | (uint32_t)d[17] << 8 | d[18];
    bls_n::addReceivedRts(from4, to4, len, txAt);
  }
  void onScanEnd(const NimBLEScanResults& scanResults, int reason) override {
    (void)scanResults;
    (void)reason;
    s_blsScanActive = false;
    s_blsScanEnded = true;
  }
};

static BlsScanCallbacks s_blsScanCallbacks;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    s_connected = true;
    displayWakeRequest();
    vTaskDelay(pdMS_TO_TICKS(5));   // краткая пауза для GATT
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    s_connected = false;
    s_blsScanEnded = true;
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
      pScan->stop();
      s_blsScanActive = false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));  // дать стеку освободить соединение
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (!pAdv->isAdvertising()) {
      pAdv->start();
      Serial.println("[BLE] Advertising restarted after disconnect");
    }
  }
};

class CharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string val = pCharacteristic->getValue();
    if (val.length() == 0) return;
    if (val.length() > 512) return;  // защита от переполнения, длинные — через фрагментацию

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, val.c_str());
    if (err) return;

    const char* cmd = doc["cmd"];
    if (!cmd) return;

    if (strcmp(cmd, "info") == 0) {
      s_pendingInfo = true;
      return;
    }

    if (strcmp(cmd, "invite") == 0) {
      s_pendingInvite = true;
      return;
    }

    if (strcmp(cmd, "channelKey") == 0) {
      const char* keyB64 = doc["key"];
      if (keyB64) {
        size_t decLen;
        uint8_t key[32];
        if (mbedtls_base64_decode(key, 32, &decLen, (const unsigned char*)keyB64, strlen(keyB64)) == 0 && decLen == 32) {
          if (crypto::setChannelKey(key)) {
            s_pendingInfo = true;
          }
        }
      }
      return;
    }

    if (strcmp(cmd, "acceptInvite") == 0) {
      const char* idStr = doc["id"];
      const char* pubKeyB64 = doc["pubKey"];
      const char* channelKeyB64 = doc["channelKey"];
      if (idStr && strlen(idStr) >= 8 && pubKeyB64) {
        if (channelKeyB64) {
          size_t decLen;
          uint8_t chKey[32];
          if (mbedtls_base64_decode(chKey, 32, &decLen, (const unsigned char*)channelKeyB64, strlen(channelKeyB64)) == 0 && decLen == 32) {
            crypto::setChannelKey(chKey);
          }
        }
        uint8_t nodeId[protocol::NODE_ID_LEN];
        memset(nodeId, 0xFF, protocol::NODE_ID_LEN);
        int n = (strlen(idStr) >= 16) ? 8 : 4;
        for (int i = 0; i < n; i++) {
          char hex[3] = { idStr[i*2], idStr[i*2+1], 0 };
          nodeId[i] = (uint8_t)strtoul(hex, nullptr, 16);
        }
        size_t decLen;
        uint8_t pubKey[32];
        if (mbedtls_base64_decode(pubKey, 32, &decLen, (const unsigned char*)pubKeyB64, strlen(pubKeyB64)) == 0 && decLen == 32) {
          x25519_keys::onKeyExchange(nodeId, pubKey);
          x25519_keys::sendKeyExchange(nodeId, true, false, "ble");  // forceSend — отправить наш ключ пиру для завершения обмена
          s_pendingInfo = true;
        }
      }
      return;
    }

    if (strcmp(cmd, "send") == 0) {
      const char* text = doc["text"];
      if (!text || strlen(text) == 0) return;  // пустые — не отправлять
      uint32_t groupId = doc["group"] | 0;
      if (groupId > 0) {
        if (strlen(text) == 0) return;  // пустые в группу — не отправлять
        strncpy(s_pendingGroupText, text, sizeof(s_pendingGroupText) - 1);
        s_pendingGroupText[sizeof(s_pendingGroupText) - 1] = '\0';
        s_pendingGroupId = groupId;
        __sync_synchronize();  // memory barrier — main loop должен видеть данные до флага
        s_pendingGroupSend = true;
      } else {
        uint8_t to[protocol::NODE_ID_LEN];
        memset(to, 0xFF, protocol::NODE_ID_LEN);
        const char* toStr = doc["to"];
        if (toStr && strlen(toStr) >= 8) {
          for (int i = 0; i < 4; i++) {
            char hex[3] = { toStr[i*2], toStr[i*2+1], 0 };
            to[i] = (uint8_t)strtoul(hex, nullptr, 16);
          }
        }
        uint8_t ttl = (doc["ttl"] | 0) & 0xFF;
        if (s_onSend) s_onSend(to, text, ttl);
      }
      return;
    }

    if (strcmp(cmd, "location") == 0) {
      float lat = doc["lat"] | 0.0f;
      float lon = doc["lon"] | 0.0f;
      int16_t alt = doc["alt"] | 0;
      if (s_onLocation) s_onLocation(lat, lon, alt);
      return;
    }

    if (strcmp(cmd, "ota") == 0) {
      ota::start();
      if (ota::isActive()) ble::notifyOta("192.168.4.1", "RiftLink-OTA", "riftlink123");
      return;
    }

    if (strcmp(cmd, "lang") == 0) {
      const char* lang = doc["lang"];
      if (lang) {
        if (strcmp(lang, "ru") == 0) locale::setLang(LANG_RU);
        else if (strcmp(lang, "en") == 0) locale::setLang(LANG_EN);
      }
      return;
    }

    if (strcmp(cmd, "wifi") == 0) {
      const char* ssid = doc["ssid"];
      const char* pass = doc["pass"].as<const char*>();
      if (!pass) pass = doc["password"].as<const char*>();
      if (ssid && wifi::setCredentials(ssid, pass)) {
        wifi::connect();
        char ip[16];
        wifi::getStatus(nullptr, 0, ip, sizeof(ip));
        ble::notifyWifi(wifi::isConnected(), ssid, ip);
      }
      return;
    }

    if (strcmp(cmd, "region") == 0) {
      const char* r = doc["region"];
      if (r && region::setRegion(r)) {
        ble::notifyRegion(region::getCode(), region::getFreq(), region::getPower(), region::getChannel());
      }
      return;
    }

    if (strcmp(cmd, "channel") == 0) {
      int ch = doc["channel"] | -1;
      if (ch >= 0 && ch <= 2 && region::setChannel(ch)) {
        ble::notifyRegion(region::getCode(), region::getFreq(), region::getPower(), region::getChannel());
      }
      return;
    }
    if (strcmp(cmd, "sf") == 0 || strcmp(cmd, "loraSf") == 0) {
      int sf = doc["sf"] | doc["value"] | -1;
      if (sf >= 7 && sf <= 12) {
        radio::requestSpreadingFactor((uint8_t)sf);
        s_pendingInfo = true;
      }
      return;
    }
    if (strcmp(cmd, "espnowChannel") == 0) {
      int ch = doc["channel"] | doc["espnowChannel"] | -1;
      if (ch >= 1 && ch <= 13 && esp_now_slots::setChannel((uint8_t)ch)) {
        esp_now_slots::setAdaptive(false);
        s_pendingInfo = true;
      }
      return;
    }
    if (strcmp(cmd, "espnowAdaptive") == 0) {
      bool on = doc["enabled"] | doc["adaptive"] | false;
      if (esp_now_slots::setAdaptive(on)) s_pendingInfo = true;
      return;
    }

    if (strcmp(cmd, "nickname") == 0) {
      const char* nick = doc["nickname"];
      if (nick && strnlen(nick, 18) <= 16) {
        strncpy(s_pendingNicknameBuf, nick, 16);
        s_pendingNicknameBuf[16] = '\0';
        __sync_synchronize();
        s_pendingNickname = true;
        s_pendingInfo = true;
        queueDisplayRequestInfoRedraw();
      }
      return;
    }

    if (strcmp(cmd, "groups") == 0) {
      s_pendingGroups = true;
      return;
    }
    if (strcmp(cmd, "routes") == 0 || strcmp(cmd, "mesh") == 0) {
      s_pendingRoutes = true;
      return;
    }
    if (strcmp(cmd, "powersave") == 0) {
      if (doc["enabled"].is<bool>()) {
        powersave::setEnabled(doc["enabled"].as<bool>());
      }
      return;
    }
    if (strcmp(cmd, "addGroup") == 0) {
      uint32_t gid = doc["group"] | 0;
      if (gid > 0 && groups::addGroup(gid)) {
        s_pendingGroups = true;
      }
      return;
    }
    if (strcmp(cmd, "removeGroup") == 0) {
      uint32_t gid = doc["group"] | 0;
      if (gid > 0) {
        groups::removeGroup(gid);
        s_pendingGroups = true;
      }
      return;
    }

    if (strcmp(cmd, "gps_sync") == 0) {
      int64_t utcMs = doc["utc_ms"] | 0;
      float lat = doc["lat"] | 0.0f;
      float lon = doc["lon"] | 0.0f;
      int16_t alt = doc["alt"] | 0;
      if (utcMs != 0) gps::setPhoneSync(utcMs, lat, lon, alt);
      return;
    }

    if (strcmp(cmd, "gps") == 0) {
      s_pendingGpsHasEnabled = doc["enabled"].is<bool>();
      if (s_pendingGpsHasEnabled) s_pendingGpsEnabled = doc["enabled"].as<bool>();
      s_pendingGpsHasPins = doc["rx"].is<int>() || doc["rx"].is<int64_t>() ||
          doc["tx"].is<int>() || doc["tx"].is<int64_t>() ||
          doc["en"].is<int>() || doc["en"].is<int64_t>();
      if (s_pendingGpsHasPins) {
        s_pendingGpsRx = doc["rx"] | -1;
        s_pendingGpsTx = doc["tx"] | -1;
        s_pendingGpsEn = doc["en"] | -1;
      }
      __sync_synchronize();
      s_pendingGps = true;
      return;
    }

    if (strcmp(cmd, "selftest") == 0 || strcmp(cmd, "test") == 0) {
      s_pendingSelftest = true;
      return;
    }

    if (strcmp(cmd, "read") == 0) {
      const char* fromStr = doc["from"];
      uint32_t msgId = doc["msgId"] | 0u;
      if (fromStr && strlen(fromStr) >= 8 && msgId != 0) {
        uint8_t to[protocol::NODE_ID_LEN];
        memset(to, 0xFF, protocol::NODE_ID_LEN);
        for (int i = 0; i < 4; i++) {
          char hex[3] = { fromStr[i*2], fromStr[i*2+1], 0 };
          to[i] = (uint8_t)strtoul(hex, nullptr, 16);
        }
        uint8_t payload[4];
        memcpy(payload, &msgId, 4);
        uint8_t pkt[protocol::PAYLOAD_OFFSET + 4];
        size_t pktLen = protocol::buildPacket(pkt, sizeof(pkt),
            node::getId(), to, 31, protocol::OP_READ, payload, 4, false, false);
        if (pktLen > 0) radio::send(pkt, pktLen, neighbors::rssiToSf(neighbors::getRssiFor(to)));
      }
      return;
    }

    if (strcmp(cmd, "ping") == 0) {
      const char* toStr = doc["to"];
      uint8_t to[protocol::NODE_ID_LEN];
      memset(to, 0xFF, protocol::NODE_ID_LEN);
      if (toStr && strlen(toStr) >= 8) {
        int n = (strlen(toStr) >= 16) ? 8 : 4;
        for (int i = 0; i < n; i++) {
          char hex[3] = { toStr[i*2], toStr[i*2+1], 0 };
          to[i] = (uint8_t)strtoul(hex, nullptr, 16);
        }
      }
      uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN];
      size_t len = protocol::buildPacket(pkt, sizeof(pkt),
          node::getId(), to, 31, protocol::OP_PING, nullptr, 0);
      if (len > 0) radio::send(pkt, len, neighbors::rssiToSf(neighbors::getRssiFor(to)));
      return;
    }

    if (strcmp(cmd, "voice") == 0) {
      const char* toStr = doc["to"];
      const char* dataStr = doc["data"];
      int chunk = doc["chunk"] | -1;
      int total = doc["total"] | -1;
      if (!toStr || strlen(toStr) < 8 || !dataStr || chunk < 0 || total <= 0) return;

      memset(s_voiceTo, 0, protocol::NODE_ID_LEN);
      int n = (strlen(toStr) >= 16) ? 8 : 4;
      for (int i = 0; i < n; i++) {
        char hex[3] = { toStr[i*2], toStr[i*2+1], 0 };
        s_voiceTo[i] = (uint8_t)strtoul(hex, nullptr, 16);
      }

      if (chunk == 0) {
        s_voiceBufLen = 0;
        s_voiceChunkTotal = total;
      }
      if (s_voiceChunkTotal != total) return;

      size_t b64Len = strlen(dataStr);
      size_t maxDec = VOICE_BUF_MAX - s_voiceBufLen;
      size_t olen;
      int r = mbedtls_base64_decode(s_voiceBuf + s_voiceBufLen, maxDec, &olen,
          (const unsigned char*)dataStr, b64Len);
      if (r == 0) {
        s_voiceBufLen += olen;
      }

      if (chunk == total - 1 && s_voiceBufLen > 0 && s_voiceBufLen <= voice_frag::MAX_VOICE_PLAIN) {
        voice_frag::send(s_voiceTo, s_voiceBuf, s_voiceBufLen);
        s_voiceBufLen = 0;
        s_voiceChunkTotal = -1;
      }
      return;
    }
  }
};

namespace ble {

bool init() {
  Serial.println("[BLE] Init...");
  if (!NimBLEDevice::init(DEVICE_NAME)) {
    Serial.println("[BLE] NimBLEDevice::init FAILED — устройство не будет видно в скане BLE");
    return false;
  }
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);  // P3 вместо P9 — меньше ток, стабильнее при connect

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pService = pServer->createService(SERVICE_UUID);

  NimBLECharacteristic* pTxChar = pService->createCharacteristic(CHAR_TX_UUID, NIMBLE_PROPERTY::WRITE_NR);
  pTxChar->setCallbacks(new CharCallbacks());

  pRxChar = pService->createCharacteristic(CHAR_RX_UUID, NIMBLE_PROPERTY::NOTIFY);
  pService->start();

  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  // UUID в main packet, имя RL-12AB34CD в scan response (лимит 31 байт на пакет)
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN);  // general discoverable — иначе Android не видит
  advData.addServiceUUID(SERVICE_UUID);
  NimBLEAdvertisementData scanData;
  char advName[12];
  const uint8_t* id = node::getId();
  snprintf(advName, sizeof(advName), "RL-%02X%02X%02X%02X", id[0], id[1], id[2], id[3]);
  scanData.setName(advName);
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->setScanResponseData(scanData);
  pAdvertising->enableScanResponse(true);  // Android иначе видит "unknown"
  if (!pAdvertising->start()) {
    Serial.println("[BLE] Advertising start FAILED — перезагрузите устройство");
    return false;
  }
  Serial.printf("[BLE] Advertising as '%s'\n", advName);
  return true;
}

void setOnSend(void (*cb)(const uint8_t* to, const char* text, uint8_t ttlMinutes)) {
  s_onSend = cb;
}

void setOnLocation(void (*cb)(float lat, float lon, int16_t alt)) {
  s_onLocation = cb;
}

void requestMsgNotify(const uint8_t* from, const char* text, uint32_t msgId, int rssi, uint8_t ttlMinutes) {
  if (!from || !text) return;
  memcpy(s_pendingMsgFrom, from, 8);
  strncpy(s_pendingMsgText, text, 255);
  s_pendingMsgText[255] = '\0';
  s_pendingMsgId = msgId;
  s_pendingMsgRssi = rssi;
  s_pendingMsgTtl = ttlMinutes;
  s_pendingMsg = true;
}

void notifyMsg(const uint8_t* from, const char* text, uint32_t msgId, int rssi, uint8_t ttlMinutes) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "msg";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["text"] = text;
  if (msgId != 0) doc["msgId"] = msgId;
  if (rssi != 0) doc["rssi"] = rssi;
  if (ttlMinutes != 0) doc["ttl"] = ttlMinutes;

  char buf[400];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyDelivered(const uint8_t* from, uint32_t msgId, int rssi) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "delivered";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["msgId"] = msgId;
  if (rssi != 0) doc["rssi"] = rssi;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyRead(const uint8_t* from, uint32_t msgId, int rssi) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "read";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["msgId"] = msgId;
  if (rssi != 0) doc["rssi"] = rssi;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifySent(const uint8_t* to, uint32_t msgId) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "sent";
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(toHex + i*2, 3, "%02X", to[i]);
  doc["to"] = toHex;
  doc["msgId"] = msgId;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyUndelivered(const uint8_t* to, uint32_t msgId) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "undelivered";
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(toHex + i*2, 3, "%02X", to[i]);
  doc["to"] = toHex;
  doc["msgId"] = msgId;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyBroadcastDelivery(uint32_t msgId, int delivered, int total) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  if (total > 0 && delivered == 0) {
    doc["evt"] = "undelivered";
  } else {
    doc["evt"] = "broadcast_delivery";
  }
  doc["msgId"] = msgId;
  doc["delivered"] = delivered;
  doc["total"] = total;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyLocation(const uint8_t* from, float lat, float lon, int16_t alt, int rssi) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "location";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["lat"] = lat;
  doc["lon"] = lon;
  doc["alt"] = alt;
  if (rssi != 0) doc["rssi"] = rssi;

  char buf[200];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyTelemetry(const uint8_t* from, uint16_t batteryMv, uint16_t heapKb, int rssi) {
  (void)from; (void)batteryMv; (void)heapKb; (void)rssi;
  // Телеметрия не отправляется в приложение — системная инфо, не для чатов
}

void notifyInfo() {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "info";
  const uint8_t* id = node::getId();
  char idHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(idHex + i*2, 3, "%02X", id[i]);
  doc["id"] = idHex;
  char nick[17];
  node::getNickname(nick, sizeof(nick));
  if (nick[0]) doc["nickname"] = nick;
  doc["region"] = region::getCode();
  doc["freq"] = region::getFreq();
  doc["power"] = region::getPower();
  if (region::getChannelCount() > 0) {
    doc["channel"] = region::getChannel();
  }
  doc["espnowChannel"] = esp_now_slots::getChannel();
  doc["espnowAdaptive"] = esp_now_slots::isAdaptive();
  doc["version"] = RIFTLINK_VERSION;
  doc["sf"] = radio::getSpreadingFactor();
  int offlinePending = offline_queue::getPendingCount();
  if (offlinePending > 0) doc["offlinePending"] = offlinePending;
  doc["gpsPresent"] = gps::isPresent();
  doc["gpsEnabled"] = gps::isEnabled();
  doc["gpsFix"] = gps::hasFix();
  doc["powersave"] = powersave::isEnabled();

  JsonArray grpArr = doc["groups"].to<JsonArray>();
  int ng = groups::getCount();
  for (int i = 0; i < ng; i++) {
    grpArr.add((uint32_t)groups::getId(i));
  }

  JsonArray arr = doc["neighbors"].to<JsonArray>();
  JsonArray rssiArr = doc["neighborsRssi"].to<JsonArray>();
  JsonArray hasKeyArr = doc["neighborsHasKey"].to<JsonArray>();
  int n = neighbors::getCount();
  char hex[17];
  uint8_t peerId[protocol::NODE_ID_LEN];
  for (int i = 0; i < n; i++) {
    neighbors::getIdHex(i, hex);
    arr.add(hex);
    rssiArr.add(neighbors::getRssi(i));
    if (neighbors::getId(i, peerId)) hasKeyArr.add(x25519_keys::hasKeyFor(peerId));
    else hasKeyArr.add(false);
  }

  JsonArray routesArr = doc["routes"].to<JsonArray>();
  int nr = routing::getRouteCount();
  uint8_t dest[8], nextHop[8];
  uint8_t hops;
  int8_t rssi;
  for (int i = 0; i < nr; i++) {
    if (!routing::getRouteAt(i, dest, nextHop, &hops, &rssi)) continue;
    JsonObject ro = routesArr.add<JsonObject>();
    char d[17], nh[17];
    for (int j = 0; j < 8; j++) { snprintf(d + j*2, 3, "%02X", dest[j]); snprintf(nh + j*2, 3, "%02X", nextHop[j]); }
    d[16] = nh[16] = '\0';
    ro["dest"] = d;
    ro["nextHop"] = nh;
    ro["hops"] = hops;
    ro["rssi"] = (int)rssi;
  }

  char buf[600];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyInvite() {
  if (!pRxChar || !s_connected) return;

  uint8_t pubKey[32];
  if (!x25519_keys::getOurPublicKey(pubKey)) return;

  size_t b64Len;
  mbedtls_base64_encode(nullptr, 0, &b64Len, pubKey, 32);
  char pubKeyB64[64];
  if (mbedtls_base64_encode((unsigned char*)pubKeyB64, sizeof(pubKeyB64), &b64Len, pubKey, 32) != 0) return;
  pubKeyB64[b64Len] = '\0';

  JsonDocument doc;
  doc["evt"] = "invite";
  const uint8_t* id = node::getId();
  char idHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(idHex + i*2, 3, "%02X", id[i]);
  doc["id"] = idHex;
  doc["pubKey"] = pubKeyB64;

  uint8_t chKey[32];
  if (crypto::getChannelKey(chKey)) {
    size_t chB64Len;
    mbedtls_base64_encode(nullptr, 0, &chB64Len, chKey, 32);
    char chKeyB64[64];
    if (mbedtls_base64_encode((unsigned char*)chKeyB64, sizeof(chKeyB64), &chB64Len, chKey, 32) == 0) {
      chKeyB64[chB64Len] = '\0';
      doc["channelKey"] = chKeyB64;
    }
  }

  char buf[200];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyRoutes() {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "routes";
  JsonArray arr = doc["routes"].to<JsonArray>();
  int n = routing::getRouteCount();
  uint8_t dest[8], nextHop[8];
  uint8_t hops;
  int8_t rssi;
  char d[17], nh[17];
  for (int i = 0; i < n; i++) {
    if (!routing::getRouteAt(i, dest, nextHop, &hops, &rssi)) continue;
    JsonObject ro = arr.add<JsonObject>();
    for (int j = 0; j < 8; j++) { snprintf(d + j*2, 3, "%02X", dest[j]); snprintf(nh + j*2, 3, "%02X", nextHop[j]); }
    d[16] = nh[16] = '\0';
    ro["dest"] = d;
    ro["nextHop"] = nh;
    ro["hops"] = hops;
    ro["rssi"] = (int)rssi;
  }
  char buf[400];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyGroups() {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "groups";
  JsonArray arr = doc["groups"].to<JsonArray>();
  int n = groups::getCount();
  for (int i = 0; i < n; i++) {
    arr.add((uint32_t)groups::getId(i));
  }

  char buf[120];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void requestNeighborsNotify() {
  s_pendingNeighbors = true;
}

void notifyNeighbors() {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "neighbors";
  JsonArray arr = doc["neighbors"].to<JsonArray>();
  JsonArray rssiArr = doc["rssi"].to<JsonArray>();
  JsonArray hasKeyArr = doc["hasKey"].to<JsonArray>();
  int n = neighbors::getCount();
  char hex[17];
  uint8_t peerId[protocol::NODE_ID_LEN];
  for (int i = 0; i < n; i++) {
    neighbors::getIdHex(i, hex);
    arr.add(hex);
    rssiArr.add(neighbors::getRssi(i) != 0 ? neighbors::getRssi(i) : (int)0);
    if (neighbors::getId(i, peerId)) hasKeyArr.add(x25519_keys::hasKeyFor(peerId));
    else hasKeyArr.add(false);
  }

  char buf[320];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyOta(const char* ip, const char* ssid, const char* password) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "ota";
  doc["ip"] = ip;
  doc["ssid"] = ssid;
  doc["password"] = password;

  char buf[200];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyWifi(bool connected, const char* ssid, const char* ip) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "wifi";
  doc["connected"] = connected;
  if (ssid) doc["ssid"] = ssid;
  if (ip) doc["ip"] = ip;

  char buf[150];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyRegion(const char* code, float freq, int power, int channel) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "region";
  doc["region"] = code;
  doc["freq"] = freq;
  doc["power"] = power;
  if (channel >= 0) doc["channel"] = channel;

  char buf[120];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyGps(bool present, bool enabled, bool hasFix, int rx, int tx, int en) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "gps";
  doc["present"] = present;
  doc["enabled"] = enabled;
  doc["hasFix"] = hasFix;
  if (rx >= 0) doc["rx"] = rx;
  if (tx >= 0) doc["tx"] = tx;
  if (en >= 0) doc["en"] = en;

  char buf[120];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyError(const char* code, const char* msg) {
  if (!pRxChar || !s_connected || !code || !msg) return;

  JsonDocument doc;
  doc["evt"] = "error";
  doc["code"] = code;
  doc["msg"] = msg;

  char buf[200];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyPong(const uint8_t* from, int rssi) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "pong";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  if (rssi != 0) doc["rssi"] = rssi;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifySelftest(bool radioOk, bool displayOk, uint16_t batteryMv, uint32_t heapFree) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc;
  doc["evt"] = "selftest";
  doc["radioOk"] = radioOk;
  doc["displayOk"] = displayOk;
  doc["batteryMv"] = batteryMv;
  doc["heapFree"] = heapFree;

  char buf[120];
  size_t len = serializeJson(doc, buf);
  pRxChar->setValue((uint8_t*)buf, len);
  pRxChar->notify();
}

void notifyVoice(const uint8_t* from, const uint8_t* data, size_t dataLen) {
  if (!pRxChar || !s_connected || !data) return;

  const size_t CHUNK_RAW = 384;
  const size_t CHUNK_B64 = 512;
  size_t totalChunks = (dataLen + CHUNK_RAW - 1) / CHUNK_RAW;

  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);

  for (size_t i = 0; i < totalChunks; i++) {
    size_t off = i * CHUNK_RAW;
    size_t chunkLen = dataLen - off;
    if (chunkLen > CHUNK_RAW) chunkLen = CHUNK_RAW;

    unsigned char b64[CHUNK_B64];
    size_t olen;
    if (mbedtls_base64_encode(b64, sizeof(b64), &olen, data + off, chunkLen) != 0) break;

    JsonDocument doc;
    doc["evt"] = "voice";
    doc["from"] = fromHex;
    doc["chunk"] = (int)i;
    doc["total"] = (int)totalChunks;
    doc["data"] = (const char*)b64;

    char buf[600];
    size_t len = serializeJson(doc, buf);
    if (len < sizeof(buf)) {
      pRxChar->setValue((uint8_t*)buf, len);
      pRxChar->notify();
    }
  }
}

void update() {
  // GPS: применить в main loop (thread-safe: setPins удаляет s_serial, main читает в gps::update)
  if (s_pendingGps) {
    s_pendingGps = false;
    if (s_pendingGpsHasEnabled) gps::setEnabled(s_pendingGpsEnabled);
    if (s_pendingGpsHasPins) {
      gps::setPins(s_pendingGpsRx, s_pendingGpsTx, s_pendingGpsEn);
      gps::saveConfig();
    }
    if (s_connected && pRxChar) {
      int rx, tx, en;
      gps::getPins(&rx, &tx, &en);
      notifyGps(gps::isPresent(), gps::isEnabled(), gps::hasFix(), rx, tx, en);
    }
    return;
  }
  if (s_connected && pRxChar) {
    // Сначала применить nickname из BLE (thread-safe: main loop пишет в node)
    if (s_pendingNickname) {
      s_pendingNickname = false;
      node::setNickname(s_pendingNicknameBuf);
    }
    // По одному notify за вызов — не перегружать BLE stack
    if (s_pendingInfo) {
      s_pendingInfo = false;
      notifyInfo();
      vTaskDelay(pdMS_TO_TICKS(2));  // дать BLE отправить большой payload
      return;
    }
    if (s_pendingMsg) {
      s_pendingMsg = false;
      notifyMsg(s_pendingMsgFrom, s_pendingMsgText, s_pendingMsgId, s_pendingMsgRssi, s_pendingMsgTtl);
      return;
    }
    if (s_pendingGroups) { s_pendingGroups = false; notifyGroups(); return; }
    if (s_pendingRoutes) { s_pendingRoutes = false; notifyRoutes(); return; }
    if (s_pendingNeighbors) { s_pendingNeighbors = false; notifyNeighbors(); return; }
    if (s_pendingInvite) { s_pendingInvite = false; notifyInvite(); return; }
    if (s_pendingSelftest) {
      s_pendingSelftest = false;
      selftest::Result r;
      selftest::run(&r);
      notifySelftest(r.radioOk, r.displayOk, r.batteryMv, r.heapFree);
    }
    if (s_pendingGroupSend) {
      s_pendingGroupSend = false;
      bool ok = msg_queue::enqueueGroup(s_pendingGroupId, s_pendingGroupText);
      if (!ok) notifyError("group_send", "Сообщение слишком длинное или ошибка шифрования");
    }
  }
  if (pServer && !s_connected && pServer->getConnectedCount() == 0) {
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (!pAdv->isAdvertising()) {
      pAdv->start();
    }
  }

  // BLS-N: при подключённом телефоне — BLE scan для приёма RTS от соседей
  // Paper: отключено — BLE scan вызывает Malloc failed при heap ~10KB (много advertisers)
#if !defined(USE_EINK)
  if (s_connected) {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && !pScan->isScanning() && !s_blsScanActive) {
      if (s_blsScanEnded && (millis() - s_blsScanLastStart) < BLS_SCAN_RESTART_DELAY_MS)
        ;  // ждём перед повторной попыткой
      else {
        if (s_blsScanEnded) s_blsScanEnded = false;
        pScan->setScanCallbacks(&s_blsScanCallbacks, true);
        if (pScan->start(BLS_SCAN_DURATION_SEC, false, false)) {
          s_blsScanActive = true;
          s_blsScanLastStart = millis();
        } else {
          s_blsScanEnded = true;
          s_blsScanLastStart = millis();  // throttle при конфликте scan+GATT
        }
      }
    }
  }
#endif  // !USE_EINK
}

bool isConnected() { return s_connected; }

}  // namespace ble
