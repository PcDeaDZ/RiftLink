/**
 * RiftLink BLE — GATT сервис для Flutter
 * Протокол: JSON {"cmd":"send","text":"..."} / {"evt":"msg","from":"...","text":"..."}
 */

#include "ble.h"
#include <string.h>
#include <string_view>
#include <new>
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
#include "telemetry/telemetry.h"
#include "selftest/selftest.h"
#include "ui/display.h"
#include "async_tasks.h"
#include "version.h"
#include "bls_n/bls_n.h"
#include "crypto/crypto.h"
#include "esp_now_slots/esp_now_slots.h"
#include "radio_mode/radio_mode.h"
#include "ws_server/ws_server.h"
#include "ble_ota/ble_ota.h"
#include <mbedtls/base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <nvs.h>
#include <esp_random.h>

#define NVS_BLE_NAMESPACE "riftlink"
#define NVS_KEY_BLE_PIN   "ble_pin"

static uint32_t s_passkey = 0;

static void loadOrGeneratePasskey() {
  nvs_handle_t h;
  if (nvs_open(NVS_BLE_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    uint32_t pin = 0;
    if (nvs_get_u32(h, NVS_KEY_BLE_PIN, &pin) == ESP_OK && pin >= 100000 && pin <= 999999) {
      s_passkey = pin;
    } else {
      s_passkey = esp_random() % 900000 + 100000;
      nvs_set_u32(h, NVS_KEY_BLE_PIN, s_passkey);
      nvs_commit(h);
    }
    nvs_close(h);
  } else {
    s_passkey = esp_random() % 900000 + 100000;
  }
}

#define SERVICE_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHAR_TX_UUID        "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // write from app
#define CHAR_RX_UUID        "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // notify to app
#define DEVICE_NAME         "RiftLink"
/**
 * Один JSON на запись в GATT TX / одно notify: ≤ BLE_ATT_MAX_JSON_BYTES.
 * После JSON в notify добавляется `\n` (NDJSON) — приложение режет поток по строкам; при len==512 байт `\n` не добавляется (редкий край).
 * NimBLE: BLE_ATT_ATTR_MAX_LEN и CONFIG_NIMBLE_CPP_ATT_VALUE_INIT_LENGTH — макс. 512 байт на значение атрибута.
 * Поэтому «512» — не произвольный порог, а потолок стека BLE.
 *
 * Оценка voice: {"cmd":"voice","to":"HH...","chunk":n,"total":t,"data":"<b64>"} — оболочка ~85–95 B,
 * на base64 остаётся ~417 B → сырой Opus ≤ ~312 B на чанк (см. BLE_VOICE_CHUNK_RAW_MAX).
 *
 * ArduinoJson 7: JsonDocument с кастомным Allocator — предпочтительно SPIRAM, иначе malloc (платы без PSRAM).
 * При переполнении — doc.overflowed().
 */
static constexpr size_t BLE_ATT_MAX_JSON_BYTES = 512;
/** Сырой Opus на один BLE-чанк voice (вход/выход), чтобы весь JSON уместился в 512 B. */
static constexpr size_t BLE_VOICE_CHUNK_RAW_MAX = 300;
static constexpr size_t BLE_VOICE_CHUNK_B64_BUF = 400;  // ceil(300/3)*4
static constexpr size_t kBleJsonProtocolMaxBytes = BLE_ATT_MAX_JSON_BYTES;
static_assert(kBleJsonProtocolMaxBytes == 512, "согласовано с BLE_ATT_ATTR_MAX_LEN");

namespace {
struct BleJsonAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
      void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
      if (p) return p;
    }
    return malloc(size);
  }
  void deallocate(void* pointer) override { free(pointer); }
  void* reallocate(void* pointer, size_t new_size) override {
    if (!pointer) return allocate(new_size);
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
      void* p = heap_caps_realloc(pointer, new_size, MALLOC_CAP_SPIRAM);
      if (p) return p;
    }
    return realloc(pointer, new_size);
  }
};
static BleJsonAllocator s_bleJsonAllocator;
}  // namespace

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

#ifndef BLE_LOG_TX_JSON
#define BLE_LOG_TX_JSON 1
#endif

#if !defined(RIFTLINK_DISABLE_BLS_N)
// BLS-N: BLE scan для приёма RTS при подключённом телефоне
static bool s_blsScanActive = false;
static bool s_blsScanEnded = false;
static uint32_t s_blsScanLastStart = 0;
#define BLS_SCAN_DURATION_SEC 15
#define BLS_SCAN_RESTART_DELAY_MS 500
/** NimBLE scan резервирует internal heap; при ~1KB после Wi‑Fi+async — BLE_INIT: Malloc failed */
#define BLS_SCAN_MIN_FREE_INTERNAL 5120
#define BLS_SCAN_MIN_LARGEST_INTERNAL 4096

static bool blsScanHeapOk() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= BLS_SCAN_MIN_FREE_INTERNAL &&
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= BLS_SCAN_MIN_LARGEST_INTERNAL;
}

/** Manufacturer Specific Data (AD type 0xFF), индекс как у NimBLE getManufacturerData(index) — без std::string. */
static bool bleAdvGetMfgData(const uint8_t* pl, size_t plLen, uint8_t index,
                             const uint8_t** outData, size_t* outLen) {
  constexpr uint8_t kMfgType = 0xFF;
  size_t off = 0;
  uint8_t seen = 0;
  while (plLen - off > 2) {
    const uint8_t L = pl[off];
    const size_t remaining = plLen - off;
    if (L >= remaining) return false;
    const uint8_t type = pl[off + 1];
    if (type == kMfgType) {
      if (seen == index) {
        if (L > 1) {
          *outData = pl + off + 2;
          *outLen = (size_t)L - 1;
          return true;
        }
        return false;
      }
      seen++;
    }
    off += 1 + L;
  }
  return false;
}
#endif /* !RIFTLINK_DISABLE_BLS_N */

static uint8_t s_notifyTxBuf[BLE_ATT_MAX_JSON_BYTES];

static bool hasActiveTransport() {
  if (radio_mode::current() == radio_mode::WIFI)
    return ws_server::hasClient();
  return s_connected && pRxChar;
}

/** Макс. payload одного BLE notify = negotiated MTU - 3 (ATT header). */
static size_t bleNotifyMtu() {
  if (!pServer || pServer->getConnectedCount() == 0) return BLE_ATT_MAX_JSON_BYTES;
  uint16_t mtu = pServer->getPeerMTU(pServer->getPeerInfo(0).getConnHandle());
  return (mtu > 3) ? (mtu - 3) : BLE_ATT_MAX_JSON_BYTES;
}

static void notifyJsonToApp(const char* payload, size_t len) {
  if (!payload || len == 0) return;

  // WiFi mode: route to WebSocket (no size limit)
  if (radio_mode::current() == radio_mode::WIFI) {
#if BLE_LOG_TX_JSON
    Serial.print("[WS->APP] ");
    Serial.write((const uint8_t*)payload, len);
    Serial.println();
#endif
    ws_server::sendEvent(payload, (int)len);
    return;
  }

  if (!pRxChar || !s_connected) return;

#if BLE_LOG_TX_JSON
  Serial.print("[BLE->APP] ");
  Serial.write((const uint8_t*)payload, len);
  Serial.println();
#endif

  const size_t chunkMax = bleNotifyMtu();
  size_t off = 0;
  while (off < len) {
    size_t remain = len - off;
    bool lastChunk = (remain <= chunkMax);
    size_t chunk = lastChunk ? remain : chunkMax;

    memcpy(s_notifyTxBuf, payload + off, chunk);
    size_t sendLen = chunk;
    // NDJSON newline delimiter only on the very last chunk
    if (lastChunk && chunk < sizeof(s_notifyTxBuf)) {
      s_notifyTxBuf[chunk] = '\n';
      sendLen = chunk + 1;
    }

    pRxChar->setValue(s_notifyTxBuf, sendLen);
    pRxChar->notify();
    off += chunk;

    if (!lastChunk) vTaskDelay(pdMS_TO_TICKS(8));
  }
}

#if !defined(RIFTLINK_DISABLE_BLS_N)
class BlsScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (!advertisedDevice->haveManufacturerData()) return;
    const auto& pl = advertisedDevice->getPayload();
    const uint8_t* d = nullptr;
    size_t mfrLen = 0;
    if (!bleAdvGetMfgData(pl.data(), pl.size(), 0, &d, &mfrLen) || mfrLen < 19) return;
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
#endif /* !RIFTLINK_DISABLE_BLS_N */

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    s_connected = true;
    displayWakeRequest();
    vTaskDelay(pdMS_TO_TICKS(5));   // краткая пауза для GATT
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    s_connected = false;
#if !defined(RIFTLINK_DISABLE_BLS_N)
    s_blsScanEnded = true;
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
      pScan->stop();
      s_blsScanActive = false;
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(50));  // дать стеку освободить соединение
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (!pAdv->isAdvertising()) {
      pAdv->start();
      Serial.println("[BLE] Advertising restarted after disconnect");
    }
  }
};

static void bleHandleTxJson(const uint8_t* val, uint16_t len);

/**
 * TX GATT: NimBLE вызывает writeEvent с буфером записи — парсим JSON по указателю,
 * без getValue()/std::string и без копии NimBLEAttValue (deepCopy/realloc в библиотеке).
 */
class RiftTxCharacteristic : public NimBLECharacteristic {
 public:
  RiftTxCharacteristic(const NimBLEUUID& uuid, uint32_t properties, uint16_t maxLen, NimBLEService* pService)
      : NimBLECharacteristic(uuid, static_cast<uint16_t>(properties), maxLen, pService) {}

 protected:
  void writeEvent(const uint8_t* val, uint16_t len, NimBLEConnInfo& connInfo) override {
    (void)connInfo;
    setValue(val, len);
    bleHandleTxJson(val, len);
  }
};

/** Большой evt info — раньше String::reserve(1200); теперь BSS, без heap. */
static constexpr size_t NOTIFY_INFO_JSON_CAP = 1280;
static char s_notifyInfoPayload[NOTIFY_INFO_JSON_CAP];

alignas(RiftTxCharacteristic) static uint8_t s_riftTxCharMem[sizeof(RiftTxCharacteristic)];
static bool s_riftTxCharConstructed = false;

static void bleHandleTxJson(const uint8_t* val, uint16_t len) {
  if (len == 0) return;

  // BLE OTA: binary chunk mode — data goes directly to ble_ota, not JSON
  if (ble_ota::isActive()) {
    // Check if it's a JSON control message (starts with '{')
    if (val[0] == '{') {
      // Parse as JSON — could be bleOtaEnd or bleOtaAbort
      JsonDocument doc(&s_bleJsonAllocator);
      DeserializationError err = deserializeJson(doc, std::string_view((const char*)val, len));
      if (!err) {
        const char* cmd = doc["cmd"];
        if (cmd && strcmp(cmd, "bleOtaEnd") == 0) {
          bool ok = ble_ota::end();
          char resp[64];
          snprintf(resp, sizeof(resp), "{\"evt\":\"bleOtaResult\",\"ok\":%s}", ok ? "true" : "false");
          notifyJsonToApp(resp, strlen(resp));
          if (ok) {
            delay(500);
            esp_restart();
          }
          return;
        }
        if (cmd && strcmp(cmd, "bleOtaAbort") == 0) {
          ble_ota::abort();
          notifyJsonToApp("{\"evt\":\"bleOtaResult\",\"ok\":false,\"reason\":\"aborted\"}", 52);
          return;
        }
      }
      // Not a control message, treat as data
    }
    // Raw binary chunk
    if (!ble_ota::writeChunk(val, len)) {
      ble_ota::abort();
      notifyJsonToApp("{\"evt\":\"bleOtaResult\",\"ok\":false,\"reason\":\"write_failed\"}", 56);
      return;
    }
    // Progress notification every 16K
    uint32_t w = ble_ota::bytesWritten();
    if (w > 0 && (w % 16384) < len) {
      char prog[80];
      int n = snprintf(prog, sizeof(prog), "{\"evt\":\"bleOtaProgress\",\"written\":%u}", (unsigned)w);
      notifyJsonToApp(prog, n);
    }
    return;
  }

  if (len > BLE_ATT_MAX_JSON_BYTES) return;

  JsonDocument doc(&s_bleJsonAllocator);
  const std::string_view jsonSv(reinterpret_cast<const char*>(val), len);
  DeserializationError err = deserializeJson(doc, jsonSv);
  if (err) return;

    const char* cmd = doc["cmd"];
    if (!cmd) return;

    if (strcmp(cmd, "bleOtaStart") == 0) {
      uint32_t size = doc["size"] | 0;
      const char* md5 = doc["md5"];
      if (size == 0) {
        notifyJsonToApp("{\"evt\":\"bleOtaResult\",\"ok\":false,\"reason\":\"no_size\"}", 51);
        return;
      }
      if (ble_ota::begin(size, md5)) {
        char resp[64];
        int n = snprintf(resp, sizeof(resp), "{\"evt\":\"bleOtaReady\",\"chunkSize\":%u}", (unsigned)(BLE_ATT_MAX_JSON_BYTES - 3));
        notifyJsonToApp(resp, n);
      } else {
        notifyJsonToApp("{\"evt\":\"bleOtaResult\",\"ok\":false,\"reason\":\"begin_failed\"}", 56);
      }
      return;
    }

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
      // OTA через WiFi AP — переключаем в WiFi режим
      radio_mode::switchTo(radio_mode::WIFI, radio_mode::AP);
      return;
    }

    if (strcmp(cmd, "radioMode") == 0) {
      const char* mode = doc["mode"];
      if (!mode) return;
      if (strcmp(mode, "wifi") == 0) {
        const char* variant = doc["variant"] | "ap";
        const char* ssid = doc["ssid"];
        const char* pass = doc["pass"].as<const char*>();
        if (!pass) pass = doc["password"].as<const char*>();
        auto wv = (strcmp(variant, "sta") == 0) ? radio_mode::STA : radio_mode::AP;
        radio_mode::switchTo(radio_mode::WIFI, wv, ssid, pass);
      } else if (strcmp(mode, "ble") == 0) {
        radio_mode::switchTo(radio_mode::BLE);
      }
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
      if (ssid) {
        radio_mode::switchTo(radio_mode::WIFI, radio_mode::STA, ssid, pass);
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
    if (strcmp(cmd, "modemPreset") == 0) {
      int p = doc["preset"] | doc["value"] | -1;
      if (p >= 0 && p < 4) {
        radio::requestModemPreset((radio::ModemPreset)p);
        s_pendingInfo = true;
      }
      return;
    }
    if (strcmp(cmd, "modemCustom") == 0) {
      int sf = doc["sf"] | -1;
      float bw = doc["bw"] | -1.0f;
      int cr = doc["cr"] | -1;
      if (sf >= 7 && sf <= 12 && bw > 0 && cr >= 5 && cr <= 8) {
        radio::requestCustomModem((uint8_t)sf, bw, (uint8_t)cr);
        s_pendingInfo = true;
      }
      return;
    }
    if (strcmp(cmd, "espnowChannel") == 0) {
      if (radio_mode::current() != radio_mode::WIFI) {
        ble::notifyError("espnow", "ESP-NOW доступен только в WiFi-режиме");
        return;
      }
      int ch = doc["channel"] | doc["espnowChannel"] | -1;
      if (ch >= 1 && ch <= 13 && esp_now_slots::setChannel((uint8_t)ch)) {
        esp_now_slots::setAdaptive(false);
        s_pendingInfo = true;
      }
      return;
    }
    if (strcmp(cmd, "espnowAdaptive") == 0) {
      if (radio_mode::current() != radio_mode::WIFI) {
        ble::notifyError("espnow", "ESP-NOW доступен только в WiFi-режиме");
        return;
      }
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
    if (strcmp(cmd, "regeneratePin") == 0) {
      ble::regeneratePasskey();
      s_pendingInfo = true;
      return;
    }
    if (strcmp(cmd, "regenerateApPass") == 0) {
      wifi::regenerateApPassword();
      s_pendingInfo = true;
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

    if (strcmp(cmd, "shutdown") == 0 || strcmp(cmd, "poweroff") == 0) {
      powersave::requestShutdown();
      return;
    }

    if (strcmp(cmd, "signalTest") == 0) {
      // Ping all neighbors and report RSSI
      int n = neighbors::getCount();
      for (int i = 0; i < n && i < 8; i++) {
        uint8_t peerId[protocol::NODE_ID_LEN];
        if (!neighbors::getId(i, peerId)) continue;
        uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN];
        size_t len = protocol::buildPacket(pkt, sizeof(pkt),
            node::getId(), peerId, 31, protocol::OP_PING, nullptr, 0);
        if (len > 0) radio::send(pkt, len, neighbors::rssiToSf(neighbors::getRssiFor(peerId)));
      }
      return;
    }

    if (strcmp(cmd, "traceroute") == 0) {
      const char* toStr = doc["to"];
      if (toStr && strlen(toStr) >= 8) {
        uint8_t target[protocol::NODE_ID_LEN];
        memset(target, 0xFF, protocol::NODE_ID_LEN);
        int n = (strlen(toStr) >= 16) ? 8 : 4;
        for (int i = 0; i < n; i++) {
          char hex[3] = { toStr[i*2], toStr[i*2+1], 0 };
          target[i] = (uint8_t)strtoul(hex, nullptr, 16);
        }
        routing::requestRoute(target);
        s_pendingRoutes = true;
      }
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
      if (strlen(dataStr) > BLE_VOICE_CHUNK_B64_BUF) return;  // иначе не влезает в 512 B одной записи

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

static ServerCallbacks s_bleServerCallbacks;

namespace ble {

void processCommand(const uint8_t* data, size_t len) {
  bleHandleTxJson(data, (uint16_t)(len > 0xFFFF ? 0xFFFF : len));
}

bool init() {
  Serial.println("[BLE] Init...");
  if (!NimBLEDevice::init(DEVICE_NAME)) {
    Serial.println("[BLE] NimBLEDevice::init FAILED — устройство не будет видно в скане BLE");
    return false;
  }
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);
  NimBLEDevice::setMTU(517);

  loadOrGeneratePasskey();
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(s_passkey);
  Serial.printf("[BLE] Passkey: %06u\n", (unsigned)s_passkey);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&s_bleServerCallbacks);

  NimBLEService* pService = pServer->createService(SERVICE_UUID);

  if (!s_riftTxCharConstructed) {
    new (s_riftTxCharMem) RiftTxCharacteristic(NimBLEUUID(CHAR_TX_UUID), NIMBLE_PROPERTY::WRITE_NR, BLE_ATT_ATTR_MAX_LEN, pService);
    s_riftTxCharConstructed = true;
  }
  pService->addCharacteristic(reinterpret_cast<RiftTxCharacteristic*>(s_riftTxCharMem));

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

void deinit() {
  Serial.println("[BLE] Deinit...");
#if !defined(RIFTLINK_DISABLE_BLS_N)
  if (s_blsScanActive) {
    NimBLEDevice::getScan()->stop();
    s_blsScanActive = false;
  }
#endif
  s_connected = false;
  pRxChar = nullptr;
  pServer = nullptr;
  NimBLEDevice::deinit(true);
  Serial.printf("[BLE] Deinit done, heap free=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
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

  JsonDocument doc(&s_bleJsonAllocator);
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
  notifyJsonToApp(buf, len);
}

void notifyDelivered(const uint8_t* from, uint32_t msgId, int rssi) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "delivered";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["msgId"] = msgId;
  if (rssi != 0) doc["rssi"] = rssi;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyRead(const uint8_t* from, uint32_t msgId, int rssi) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "read";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["msgId"] = msgId;
  if (rssi != 0) doc["rssi"] = rssi;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifySent(const uint8_t* to, uint32_t msgId) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "sent";
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(toHex + i*2, 3, "%02X", to[i]);
  doc["to"] = toHex;
  doc["msgId"] = msgId;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyUndelivered(const uint8_t* to, uint32_t msgId) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "undelivered";
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(toHex + i*2, 3, "%02X", to[i]);
  doc["to"] = toHex;
  doc["msgId"] = msgId;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyBroadcastDelivery(uint32_t msgId, int delivered, int total) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
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
  notifyJsonToApp(buf, len);
}

void notifyLocation(const uint8_t* from, float lat, float lon, int16_t alt, int rssi) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
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
  notifyJsonToApp(buf, len);
}

void notifyTelemetry(const uint8_t* from, uint16_t batteryMv, uint16_t heapKb, int rssi) {
  (void)from; (void)batteryMv; (void)heapKb; (void)rssi;
  // Телеметрия не отправляется в приложение — системная инфо, не для чатов
}

void notifyInfo() {
  if (!hasActiveTransport()) return;

  JsonDocument doc(&s_bleJsonAllocator);
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
  doc["radioMode"] = (radio_mode::current() == radio_mode::BLE) ? "ble" : "wifi";
  doc["espnowChannel"] = esp_now_slots::getChannel();
  doc["espnowAdaptive"] = esp_now_slots::isAdaptive();
  doc["version"] = RIFTLINK_VERSION;
  doc["sf"] = radio::getSpreadingFactor();
  doc["bw"] = radio::getBandwidth();
  doc["cr"] = radio::getCodingRate();
  doc["modemPreset"] = (int)radio::getModemPreset();
  uint16_t batteryMv = telemetry::readBatteryMv();
  if (batteryMv > 0) doc["batteryMv"] = batteryMv;
  int batPct = telemetry::batteryPercent();
  if (batPct >= 0) doc["batteryPercent"] = batPct;
  doc["charging"] = telemetry::isCharging();
  if (gps::hasTime()) {
    doc["timeHour"] = gps::getHour();
    doc["timeMinute"] = gps::getMinute();
  }
  int offlinePending = offline_queue::getPendingCount();
  if (offlinePending > 0) doc["offlinePending"] = offlinePending;
  doc["gpsPresent"] = gps::isPresent();
  doc["gpsEnabled"] = gps::isEnabled();
  doc["gpsFix"] = gps::hasFix();
  doc["powersave"] = powersave::isEnabled();
  doc["blePin"] = s_passkey;
  doc["apSsid"] = wifi::getApSsid();
  doc["apPassword"] = wifi::getApPassword();

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

  // Буфер 600 обрезал большой `info` (соседи/маршруты) → невалидный JSON в приложении.
  size_t plen = serializeJson(doc, s_notifyInfoPayload, sizeof(s_notifyInfoPayload));
  if (plen == 0) return;
  notifyJsonToApp(s_notifyInfoPayload, plen);
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

  JsonDocument doc(&s_bleJsonAllocator);
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
  notifyJsonToApp(buf, len);
}

void notifyRoutes() {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
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
  notifyJsonToApp(buf, len);
}

void notifyGroups() {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "groups";
  JsonArray arr = doc["groups"].to<JsonArray>();
  int n = groups::getCount();
  for (int i = 0; i < n; i++) {
    arr.add((uint32_t)groups::getId(i));
  }

  char buf[120];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void requestNeighborsNotify() {
  s_pendingNeighbors = true;
}

void notifyNeighbors() {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
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
  notifyJsonToApp(buf, len);
}

void notifyOta(const char* ip, const char* ssid, const char* password) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "ota";
  doc["ip"] = ip;
  doc["ssid"] = ssid;
  doc["password"] = password;

  char buf[200];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyWifi(bool connected, const char* ssid, const char* ip) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "wifi";
  doc["connected"] = connected;
  if (ssid) doc["ssid"] = ssid;
  if (ip) doc["ip"] = ip;

  char buf[150];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyRegion(const char* code, float freq, int power, int channel) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "region";
  doc["region"] = code;
  doc["freq"] = freq;
  doc["power"] = power;
  if (channel >= 0) doc["channel"] = channel;

  char buf[120];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyGps(bool present, bool enabled, bool hasFix, int rx, int tx, int en) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "gps";
  doc["present"] = present;
  doc["enabled"] = enabled;
  doc["hasFix"] = hasFix;
  if (rx >= 0) doc["rx"] = rx;
  if (tx >= 0) doc["tx"] = tx;
  if (en >= 0) doc["en"] = en;

  char buf[120];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyError(const char* code, const char* msg) {
  if (!pRxChar || !s_connected || !code || !msg) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "error";
  doc["code"] = code;
  doc["msg"] = msg;

  char buf[200];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyPong(const uint8_t* from, int rssi) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "pong";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);
  doc["from"] = fromHex;
  if (rssi != 0) doc["rssi"] = rssi;

  char buf[80];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifySelftest(bool radioOk, bool displayOk, uint16_t batteryMv, uint32_t heapFree) {
  if (!pRxChar || !s_connected) return;

  JsonDocument doc(&s_bleJsonAllocator);
  doc["evt"] = "selftest";
  doc["radioOk"] = radioOk;
  doc["displayOk"] = displayOk;
  doc["antennaOk"] = radioOk;
  doc["batteryMv"] = batteryMv;
  int batPct = telemetry::batteryPercent();
  if (batPct >= 0) doc["batteryPercent"] = batPct;
  doc["charging"] = telemetry::isCharging();
  doc["heapFree"] = heapFree;

  char buf[160];
  size_t len = serializeJson(doc, buf);
  notifyJsonToApp(buf, len);
}

void notifyVoice(const uint8_t* from, const uint8_t* data, size_t dataLen) {
  if (!pRxChar || !s_connected || !data) return;

  const size_t CHUNK_RAW = BLE_VOICE_CHUNK_RAW_MAX;
  size_t totalChunks = (dataLen + CHUNK_RAW - 1) / CHUNK_RAW;

  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(fromHex + i*2, 3, "%02X", from[i]);

  for (size_t i = 0; i < totalChunks; i++) {
    size_t off = i * CHUNK_RAW;
    size_t chunkLen = dataLen - off;
    if (chunkLen > CHUNK_RAW) chunkLen = CHUNK_RAW;

    unsigned char b64[BLE_VOICE_CHUNK_B64_BUF + 1];
    size_t olen;
    if (mbedtls_base64_encode(b64, sizeof(b64) - 1, &olen, data + off, chunkLen) != 0) break;
    if (olen < sizeof(b64)) b64[olen] = '\0';

    JsonDocument doc(&s_bleJsonAllocator);
    doc["evt"] = "voice";
    doc["from"] = fromHex;
    doc["chunk"] = (int)i;
    doc["total"] = (int)totalChunks;
    doc["data"] = (const char*)b64;

    char buf[BLE_ATT_MAX_JSON_BYTES + 1];
    size_t len = serializeJson(doc, buf);
    if (len > 0 && len <= BLE_ATT_MAX_JSON_BYTES) {
      notifyJsonToApp(buf, len);
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
    if (hasActiveTransport()) {
      int rx, tx, en;
      gps::getPins(&rx, &tx, &en);
      notifyGps(gps::isPresent(), gps::isEnabled(), gps::hasFix(), rx, tx, en);
    }
    return;
  }
  if (hasActiveTransport()) {
    // Сначала применить nickname (thread-safe: main loop пишет в node)
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
      return;
    }
    if (s_pendingGroupSend) {
      s_pendingGroupSend = false;
      bool ok = msg_queue::enqueueGroup(s_pendingGroupId, s_pendingGroupText);
      if (!ok) notifyError("group_send", "Сообщение слишком длинное или ошибка шифрования");
    }
  }
  if (radio_mode::current() == radio_mode::BLE &&
      pServer && !s_connected && pServer->getConnectedCount() == 0) {
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (!pAdv->isAdvertising()) {
      pAdv->start();
    }
  }

  // BLS-N: при подключённом телефоне — BLE scan для приёма RTS от соседей
  // Paper: отключено — BLE scan вызывает Malloc failed при heap ~10KB (много advertisers)
  // OLED + Wi‑Fi: после esp_wifi_init остаётся мало internal — не стартуем scan без порога (иначе NimBLE BLE_INIT).
#if !defined(USE_EINK) && !defined(RIFTLINK_DISABLE_BLS_N)
  if (s_connected) {
    static uint32_t s_blsHeapSkipLogMs = 0;
    if (!blsScanHeapOk()) {
      if (millis() - s_blsHeapSkipLogMs > 60000) {
        s_blsHeapSkipLogMs = millis();
        Serial.printf("[RiftLink] BLS: BLE scan пропущен (internal free=%u largest=%u; нужно >=%u / >=%u)\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned)BLS_SCAN_MIN_FREE_INTERNAL, (unsigned)BLS_SCAN_MIN_LARGEST_INTERNAL);
      }
    } else {
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
  }
#endif  // !USE_EINK && !RIFTLINK_DISABLE_BLS_N
}

bool isConnected() { return s_connected; }

uint32_t getPasskey() { return s_passkey; }

void regeneratePasskey() {
  s_passkey = esp_random() % 900000 + 100000;
  nvs_handle_t h;
  if (nvs_open(NVS_BLE_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u32(h, NVS_KEY_BLE_PIN, s_passkey);
    nvs_commit(h);
    nvs_close(h);
  }
  NimBLEDevice::setSecurityPasskey(s_passkey);
  Serial.printf("[BLE] New passkey: %06u\n", (unsigned)s_passkey);
}

}  // namespace ble
