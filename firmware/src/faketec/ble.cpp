/**
 * FakeTech BLE — Nordic UART GATT (docs/API.md) через Adafruit Bluefruit52Lib + USB Serial.
 * BLEUart на heap после Serial — глобальный BLEUart мог мешать порядку инициализации USB.
 *
 * Диагностика пустого COM: соберите с -DRIFTLINK_SKIP_BLE (см. platformio.ini) — только Serial + mesh.
 */

#include "ble.h"
#include "node.h"
#include "region.h"
#include "neighbors.h"
#include "radio.h"
#include "crypto.h"
#include "group_owner_sign.h"
#include "x25519_keys.h"
#include "heap_metrics.h"
#include "protocol/packet.h"
#include "groups/groups.h"
#include "log.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "sodium/randombytes.h"
}

#ifndef RIFTLINK_SKIP_BLE
#include <bluefruit.h>
#endif

#define RIFTLINK_FAKETEC_VERSION "1.0.0-faketec"

static bool s_powersave = false;

static void (*s_onSend)(const uint8_t* to, const char* text, uint8_t ttlMinutes) = nullptr;
static void (*s_onLocation)(float lat, float lon, int16_t alt, uint16_t radiusM, uint32_t expiryEpochSec) = nullptr;

#ifndef RIFTLINK_SKIP_BLE
static BLEUart* s_bleuart = nullptr;
#endif
static String s_bleLine;
/** USB Serial: накопление строки без блокирующего readStringUntil (не голодать BLE NUS). */
static String s_serialLine;
static bool s_bleReady = false;
static uint32_t s_bleSyncSeq = 0;
#ifndef RIFTLINK_SKIP_BLE
static bool s_notifyWasOff = true;
#endif

/** Сопоставление cmd:ping (BLE) → evt:pong по cmdId (как rememberPingCmdId в src/ble/ble.cpp). */
struct PendingPingCmd {
  bool used;
  uint8_t to[protocol::NODE_ID_LEN];
  uint32_t cmdId;
  uint32_t expiresAtMs;
};
static PendingPingCmd s_pendingPingCmds[4];

namespace ble {
void notifyInfo(uint32_t cmdId);
void notifyError(const char* code, const char* msg, uint32_t cmdId);
void notifyInvite(uint32_t cmdId, int ttlSec);
void notifyGps(bool present, bool enabled, bool hasFix, uint32_t cmdId);
void notifySelftest(bool radioOk, bool displayOk, uint16_t batteryMv, uint32_t heapFree, uint32_t cmdId);
}

#ifndef RIFTLINK_SKIP_BLE

static void startAdv() {
  if (!s_bleuart) return;
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(*s_bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);
}

#endif

/**
 * NDJSON для Flutter: после каждого JSON — '\n'. Длинный payload режем на чанки (ATT по умолчанию ~20 B),
 * пауза между чанками как в src/ble/ble.cpp (notifyJsonToApp).
 */
#ifndef RIFTLINK_SKIP_BLE
static void sendBleNdjsonLine(const char* payload, size_t len) {
  if (!payload || len == 0) return;
  if (!s_bleReady || !s_bleuart || Bluefruit.connected() <= 0 || !s_bleuart->notifyEnabled()) return;
  const bool trace = (len > 200u);
  if (trace) RIFTLINK_DIAG("TRACE", "ble_ndjson_enter len=%u", (unsigned)len);
  const uint32_t t0 = trace ? millis() : 0u;
  /* Без обрезки len: evt:neighbors + passport >512 B — иначе JSON в NUS ломался и Flutter «зависал». */
  const size_t chunkMax = 20u;
  size_t off = 0;
  unsigned chunkIdx = 0;
  while (off < len) {
    size_t chunk = len - off;
    if (chunk > chunkMax) chunk = chunkMax;
    s_bleuart->write((const uint8_t*)payload + off, chunk);
    off += chunk;
    chunkIdx++;
    if (off < len) {
      if (trace && (chunkIdx % 20u) == 0u) {
        RIFTLINK_DIAG("TRACE", "ble_ndjson_chunk off=%u/%u chunks=%u", (unsigned)off, (unsigned)len, chunkIdx);
      }
      yield();
      delay(2);
    }
  }
  const uint8_t nl = '\n';
  s_bleuart->write(&nl, 1);
  if (trace) RIFTLINK_DIAG("TRACE", "ble_ndjson_leave ms=%lu total_chunks=%u", (unsigned long)(millis() - t0), chunkIdx);
}
#else
static void sendBleNdjsonLine(const char*, size_t) {}
#endif

static void sendEvt(const char* evt) {
  if (!evt) return;
  const size_t len = strlen(evt);
  const bool traceLong = (len > 200u);
  const bool serialOk = RIFTLINK_SERIAL_TX_HAS_SPACE();
  if (traceLong && serialOk) RIFTLINK_DIAG("TRACE", "sendEvt_begin len=%u", (unsigned)len);
  /* USB: полный println >~100 символов на CDC (Windows) может надолго блокировать передачу; полный JSON уходит в NUS ниже. */
  if (serialOk) {
    constexpr size_t kSerialEvtFullMax = 96;
    if (len <= kSerialEvtFullMax) {
      Serial.println(evt);
    } else {
      if (traceLong) RIFTLINK_DIAG("TRACE", "sendEvt_serial_preview_begin len=%u", (unsigned)len);
      Serial.printf("[RiftLink] evt len=%u preview: ", (unsigned)len);
      for (size_t i = 0; i < len && i < 120; i++) Serial.write((uint8_t)evt[i]);
      Serial.println(len > 120 ? "…" : "");
      if (traceLong) RIFTLINK_DIAG("TRACE", "sendEvt_serial_preview_done");
    }
  }
#ifndef RIFTLINK_SKIP_BLE
  sendBleNdjsonLine(evt, len);
#endif
  if (traceLong && serialOk) RIFTLINK_DIAG("TRACE", "sendEvt_end len=%u", (unsigned)len);
}

static uint32_t jsonCmdId(const JsonDocument& doc) {
  if (doc["cmdId"].isNull()) return 0;
  return (uint32_t)doc["cmdId"].as<unsigned long>();
}

static void applyCmdId(JsonDocument& doc, uint32_t cmdId) {
  if (cmdId != 0) doc["cmdId"] = cmdId;
}

static void rememberPingCmdId(const uint8_t to[protocol::NODE_ID_LEN], uint32_t cmdId) {
  if (!to || cmdId == 0) return;
  if (memcmp(to, protocol::BROADCAST_ID, protocol::NODE_ID_LEN) == 0) return;
  const uint32_t now = millis();
  int freeIdx = -1;
  int replaceIdx = 0;
  uint32_t oldest = 0xFFFFFFFFUL;
  for (int i = 0; i < 4; i++) {
    PendingPingCmd& e = s_pendingPingCmds[i];
    if (e.used && (int32_t)(now - e.expiresAtMs) >= 0) e.used = false;
    if (e.used && memcmp(e.to, to, protocol::NODE_ID_LEN) == 0) {
      e.cmdId = cmdId;
      e.expiresAtMs = now + 30000UL;
      return;
    }
    if (!e.used && freeIdx < 0) freeIdx = i;
    if (e.expiresAtMs < oldest) {
      oldest = e.expiresAtMs;
      replaceIdx = i;
    }
  }
  const int idx = (freeIdx >= 0) ? freeIdx : replaceIdx;
  PendingPingCmd& dst = s_pendingPingCmds[idx];
  dst.used = true;
  memcpy(dst.to, to, protocol::NODE_ID_LEN);
  dst.cmdId = cmdId;
  dst.expiresAtMs = now + 30000UL;
}

static uint32_t peekPingCmdIdForFrom(const uint8_t from[protocol::NODE_ID_LEN]) {
  if (!from) return 0;
  const uint32_t now = millis();
  for (int i = 0; i < 4; i++) {
    PendingPingCmd& e = s_pendingPingCmds[i];
    if (!e.used) continue;
    if ((int32_t)(now - e.expiresAtMs) >= 0) continue;
    if (memcmp(e.to, from, protocol::NODE_ID_LEN) == 0) return e.cmdId;
  }
  return 0;
}

static void takePingCmdIdForFrom(const uint8_t from[protocol::NODE_ID_LEN]) {
  if (!from) return;
  const uint32_t now = millis();
  for (int i = 0; i < 4; i++) {
    PendingPingCmd& e = s_pendingPingCmds[i];
    if (!e.used) continue;
    if ((int32_t)(now - e.expiresAtMs) >= 0) {
      e.used = false;
      continue;
    }
    if (memcmp(e.to, from, protocol::NODE_ID_LEN) == 0) e.used = false;
  }
}

static bool parseFullNodeIdHex(const char* s, uint8_t out[protocol::NODE_ID_LEN]) {
  if (!s || strlen(s) < 16) return false;
  for (int i = 0; i < 8; i++) {
    char hex[3] = {s[i * 2], s[i * 2 + 1], 0};
    out[i] = (uint8_t)strtoul(hex, nullptr, 16);
  }
  return true;
}

static uint16_t s_diagPktIdCounter = 1;

static void appendFaketecNodePassport(JsonDocument& doc) {
  const uint8_t* id = node::getId();
  char idHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(idHex + i * 2, 3, "%02X", id[i]);
  doc["id"] = idHex;
  const char* nick = node::getNickname();
  if (nick && nick[0]) doc["nickname"] = nick;
  doc["region"] = region::getCode();
  doc["freq"] = region::getFreq();
  doc["power"] = region::getPower();
  if (region::getChannelCount() > 0) doc["channel"] = region::getChannel();
  doc["radioMode"] = "ble";
  /* На nRF52840 нет Wi‑Fi (в отличие от ESP32); приложение не должно ожидать STA/AP. */
  doc["wifiConnected"] = false;
  doc["version"] = RIFTLINK_FAKETEC_VERSION;
  doc["sf"] = radio::getSpreadingFactor();
  doc["bw"] = radio::getBandwidth();
  doc["cr"] = radio::getCodingRate();
  doc["modemPreset"] = radio::getModemPresetIndex();
  /* Поля как у Heltec V3 / evt:node в приложении (честные значения для nRF). */
  doc["gpsPresent"] = false;
  doc["gpsEnabled"] = false;
  doc["gpsFix"] = false;
  doc["powersave"] = s_powersave;
  doc["blePin"] = 0;
  doc["espNowAdaptive"] = false;
  doc["flashMb"] = 1;
}

static void appendFaketecSysMetrics(JsonDocument& doc) {
  const uint32_t heapFree = heap_metrics_free_bytes();
  doc["heapFree"] = heapFree;
  doc["heapTotal"] = heap_metrics_total_bytes();
  doc["heapMin"] = heap_metrics_min_free_ever_bytes();
  doc["cpuMhz"] = 64u;
}

static void emitFaketecNode(uint32_t seq, uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "node";
  doc["seq"] = seq;
  applyCmdId(doc, cmdId);
  appendFaketecNodePassport(doc);
  appendFaketecSysMetrics(doc);
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

static void emitFaketecNeighbors(uint32_t seq, uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "neighbors";
  doc["seq"] = seq;
  applyCmdId(doc, cmdId);
  JsonArray arr = doc["neighbors"].to<JsonArray>();
  JsonArray rssiArr = doc["rssi"].to<JsonArray>();
  JsonArray hasKeyArr = doc["hasKey"].to<JsonArray>();
  for (int i = 0; i < NEIGHBORS_MAX; i++) {
    uint8_t nid[protocol::NODE_ID_LEN];
    if (!neighbors::getId(i, nid)) continue;
    char hex[17] = {0};
    for (int j = 0; j < 8; j++) sprintf(hex + j * 2, "%02X", nid[j]);
    arr.add(hex);
    rssiArr.add(neighbors::getRssi(i));
    hasKeyArr.add(x25519_keys::hasKeyFor(nid));
  }
  /* Без appendFaketecNodePassport/appendFaketecSysMetrics: иначе ~450 B × NUS по 20 B → >1 с блокировки loop()
   * при каждом HELLO (ощущение «зависания» даже без подключения телефона — пока идёт preview в Serial).
   * Полный паспорт и heap — в evt:node (notifyInfo, cmd:info). Минимум по docs/API.md §3.10. */
  {
    const uint8_t* sid = node::getId();
    char idHex[17] = {0};
    for (int i = 0; i < 8; i++) snprintf(idHex + i * 2, 3, "%02X", sid[i]);
    doc["id"] = idHex;
  }
  String s;
  RIFTLINK_DIAG("TRACE", "emit_neighbors_serialize_begin seq=%u", (unsigned)seq);
  serializeJson(doc, s);
  RIFTLINK_DIAG("TRACE", "emit_neighbors_serialize_done json_len=%u", (unsigned)s.length());
  sendEvt(s.c_str());
}

static void emitFaketecRoutes(uint32_t seq, uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "routes";
  doc["seq"] = seq;
  applyCmdId(doc, cmdId);
  doc["routes"].to<JsonArray>();
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

static const char* groupRoleToStr(groups::GroupRole r) {
  using groups::GroupRole;
  switch (r) {
    case GroupRole::Owner:
      return "owner";
    case GroupRole::Admin:
      return "admin";
    case GroupRole::Member:
      return "member";
    default:
      return "none";
  }
}

static uint32_t parseJsonChannelId32(JsonVariant v) {
  if (v.isNull()) return 0;
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!s || !s[0]) return 0;
    char* end = nullptr;
    unsigned long ul = strtoul(s, &end, 10);
    if (end == s) return 0;
    if (ul > 4294967295UL) return 0;
    return static_cast<uint32_t>(ul);
  }
  const double d = v.as<double>();
  if (d < 2.0 || d > 4294967295.0) return 0;
  return static_cast<uint32_t>(static_cast<unsigned long long>(d + 0.5));
}

static groups::GroupRole parseGroupRole(const char* s) {
  if (!s || !s[0]) return groups::GroupRole::None;
  if (strcmp(s, "owner") == 0) return groups::GroupRole::Owner;
  if (strcmp(s, "admin") == 0) return groups::GroupRole::Admin;
  if (strcmp(s, "member") == 0) return groups::GroupRole::Member;
  return groups::GroupRole::None;
}

static bool isSelfNodeHex(const char* s) {
  uint8_t id[protocol::NODE_ID_LEN];
  return parseFullNodeIdHex(s, id) && memcmp(id, node::getId(), protocol::NODE_ID_LEN) == 0;
}

static void faketecNotifyGroupSecurityErrorV2(const char* groupUid, const char* code, const char* msg, uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "groupSecurityError";
  if (groupUid && groupUid[0]) doc["groupUid"] = groupUid;
  doc["code"] = code ? code : "group_v2_unknown";
  doc["msg"] = msg ? msg : "";
  applyCmdId(doc, cmdId);
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

static void faketecNotifyGroupStatusV2(const char* groupUid, bool inviteAcceptNoop, uint32_t cmdId) {
  if (!groupUid || !groupUid[0]) return;
  uint32_t channelId32 = 0;
  char groupTag[groups::GROUP_TAG_MAX_LEN + 1] = {0};
  char canonicalName[groups::GROUP_CANONICAL_NAME_MAX_LEN + 1] = {0};
  uint16_t keyVersion = 0;
  groups::GroupRole role = groups::GroupRole::None;
  uint32_t revocationEpoch = 0;
  bool ackApplied = false;
  if (!groups::getGroupV2(groupUid, &channelId32, groupTag, sizeof(groupTag), canonicalName, sizeof(canonicalName),
          &keyVersion, &role, &revocationEpoch, &ackApplied)) {
    return;
  }
  JsonDocument doc;
  doc["evt"] = "groupStatus";
  doc["groupUid"] = groupUid;
  doc["channelId32"] = channelId32;
  doc["groupTag"] = groupTag;
  doc["canonicalName"] = canonicalName;
  doc["myRole"] = groupRoleToStr(role);
  doc["keyVersion"] = keyVersion;
  doc["revocationEpoch"] = revocationEpoch;
  doc["ackApplied"] = ackApplied;
  doc["status"] = "ok";
  doc["rekeyRequired"] = !ackApplied;
  if (inviteAcceptNoop) doc["inviteNoop"] = true;
  applyCmdId(doc, cmdId);
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

static void faketecNotifyGroupRekeyProgressV2(const char* groupUid, const char* rekeyOpId, uint16_t keyVersion,
    uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "groupRekeyProgress";
  doc["groupUid"] = groupUid;
  doc["rekeyOpId"] = (rekeyOpId && rekeyOpId[0]) ? rekeyOpId : "local";
  doc["keyVersion"] = keyVersion;
  doc["pending"] = 0;
  doc["delivered"] = 0;
  doc["applied"] = 1;
  doc["failed"] = 0;
  applyCmdId(doc, cmdId);
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

static void faketecNotifyGroupMemberKeyStateV2(const char* groupUid, const char* memberId, const char* state,
    uint32_t ackAtUnix, uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "groupMemberKeyState";
  doc["groupUid"] = groupUid;
  doc["memberId"] = memberId;
  doc["status"] = state;
  if (ackAtUnix > 0) doc["ackAt"] = (int64_t)(int32_t)ackAtUnix;
  applyCmdId(doc, cmdId);
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

static void emitFaketecGroups(uint32_t seq, uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "groups";
  doc["seq"] = seq;
  applyCmdId(doc, cmdId);
  JsonArray arr = doc["groups"].to<JsonArray>();
  const int n = groups::getV2Count();
  for (int i = 0; i < n; i++) {
    char uid[groups::GROUP_UID_MAX_LEN + 1];
    uint32_t ch = 0;
    char tag[groups::GROUP_TAG_MAX_LEN + 1];
    char cn[groups::GROUP_CANONICAL_NAME_MAX_LEN + 1];
    uint16_t kv = 0;
    groups::GroupRole role = groups::GroupRole::None;
    uint32_t rev = 0;
    bool ack = false;
    if (!groups::getV2At(i, uid, sizeof(uid), &ch, tag, sizeof(tag), cn, sizeof(cn), &kv, &role, &rev, &ack)) continue;
    JsonObject o = arr.add<JsonObject>();
    o["groupUid"] = uid;
    o["channelId32"] = (int32_t)ch;
    if (tag[0]) o["groupTag"] = tag;
    if (cn[0]) o["canonicalName"] = cn;
    o["myRole"] = groupRoleToStr(role);
    o["keyVersion"] = kv;
    o["revocationEpoch"] = rev;
    o["ackApplied"] = ack;
  }
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

static void processJsonLine(const char* line);
static void emitRegionEvt(uint32_t cmdId);
static void emitBleOtaResult(bool ok, const char* reason);

static void applyModemPreset(int preset) {
  if (preset < 0 || preset > 3) return;
  radio::applyModemPreset((uint8_t)preset);
  radio::applyRegion(region::getFreq(), region::getPower());
}

static void emitRegionEvt(uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "region";
  doc["region"] = region::getCode();
  doc["freq"] = region::getFreq();
  doc["power"] = region::getPower();
  if (region::getChannelCount() > 0) doc["channel"] = region::getChannel();
  applyCmdId(doc, cmdId);
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

static void emitBleOtaResult(bool ok, const char* reason) {
  JsonDocument doc;
  doc["evt"] = "bleOtaResult";
  doc["ok"] = ok;
  if (reason && reason[0]) doc["reason"] = reason;
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

static void appendSelfNodeHex(char out[17]) {
  const uint8_t* id = node::getId();
  for (int i = 0; i < 8; i++) snprintf(out + i * 2, 3, "%02X", id[i]);
  out[16] = 0;
}

static void processJsonLine(const char* line) {
  if (!line) return;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) return;

  const char* cmd = doc["cmd"];
  if (!cmd) return;

  const uint32_t lineCmdId = jsonCmdId(doc);

  if (strcmp(cmd, "send") == 0 && s_onSend) {
    const char* text = doc["text"];
    const char* to = doc["to"];
    uint8_t ttl = doc["ttl"] | 0;
    if (text) {
      uint8_t toId[protocol::NODE_ID_LEN];
      if (to && strlen(to) >= 16) {
        for (int i = 0; i < 8; i++) {
          char hex[3] = {to[i * 2], to[i * 2 + 1], 0};
          toId[i] = (uint8_t)strtol(hex, nullptr, 16);
        }
        s_onSend(toId, text, ttl);
      } else {
        s_onSend(protocol::BROADCAST_ID, text, ttl);
      }
    }
    return;
  }

  if (strcmp(cmd, "info") == 0) {
    ble::notifyInfo(lineCmdId);
    return;
  }

  if (strcmp(cmd, "routes") == 0 || strcmp(cmd, "mesh") == 0) {
    emitFaketecRoutes(++s_bleSyncSeq, lineCmdId);
    return;
  }

  if (strcmp(cmd, "groups") == 0) {
    emitFaketecGroups(++s_bleSyncSeq, lineCmdId);
    return;
  }

  if (strcmp(cmd, "region") == 0) {
    const char* r = doc["region"];
    if (r && region::setRegion(r)) {
      radio::applyRegion(region::getFreq(), region::getPower());
      emitRegionEvt(lineCmdId);
    } else if (lineCmdId != 0) {
      ble::notifyError("region_bad", "unknown or invalid region", lineCmdId);
    }
    return;
  }

  if (strcmp(cmd, "channel") == 0) {
    int ch = doc["channel"] | -1;
    if (region::setChannel(ch)) {
      radio::applyRegion(region::getFreq(), region::getPower());
      emitRegionEvt(lineCmdId);
    } else if (lineCmdId != 0) {
      ble::notifyError("channel_bad", "invalid channel", lineCmdId);
    }
    return;
  }

  if (strcmp(cmd, "nickname") == 0) {
    const char* nick = doc["nickname"];
    if (nick) node::setNickname(nick);
    else node::setNickname("");
    ble::notifyInfo(lineCmdId);
    return;
  }

  if (strcmp(cmd, "ping") == 0) {
    const char* toStr = doc["to"];
    uint8_t to[protocol::NODE_ID_LEN];
    memset(to, 0xFF, sizeof(to));
    if (toStr && toStr[0] && !parseFullNodeIdHex(toStr, to)) {
      ble::notifyError("ping_to_bad", "to must be full 16 hex node id", lineCmdId);
      return;
    }
    if (lineCmdId != 0) rememberPingCmdId(to, lineCmdId);
    uint16_t pktId = (uint16_t)(++s_diagPktIdCounter);
    if (s_diagPktIdCounter >= 0xFFFE) s_diagPktIdCounter = 1;
    uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID];
    size_t len = protocol::buildPacket(pkt, sizeof(pkt),
        node::getId(), to, 31, protocol::OP_PING, nullptr, 0,
        false, false, false, protocol::CHANNEL_DEFAULT, pktId);
    if (len > 0) {
      (void)radio::send(pkt, len);
    }
    return;
  }

  if (strcmp(cmd, "read") == 0) {
    const char* fromStr = doc["from"];
    uint32_t msgId = (uint32_t)(doc["msgId"].as<unsigned long>());
    if (!fromStr || strlen(fromStr) < 16 || msgId == 0) {
      ble::notifyError("read_bad", "from and msgId required", lineCmdId);
      return;
    }
    uint8_t peer[protocol::NODE_ID_LEN];
    if (!parseFullNodeIdHex(fromStr, peer)) {
      ble::notifyError("read_bad", "from must be full node id", lineCmdId);
      return;
    }
    uint8_t pl[4];
    memcpy(pl, &msgId, 4);
    uint8_t pkt[protocol::SYNC_LEN + protocol::HEADER_LEN + protocol::MAX_PAYLOAD];
    size_t len = protocol::buildPacket(pkt, sizeof(pkt),
        node::getId(), peer, 31, protocol::OP_READ, pl, 4,
        false, false, false, protocol::CHANNEL_DEFAULT, 0);
    if (len > 0) (void)radio::send(pkt, len);
    return;
  }

  if (strcmp(cmd, "invite") == 0) {
    int ttlSec = doc["ttlSec"] | 600;
    ble::notifyInvite(lineCmdId, ttlSec);
    return;
  }

  if (strcmp(cmd, "acceptInvite") == 0) {
    ble::notifyError("not_supported", "acceptInvite: use ESP mesh or extend faketec crypto", lineCmdId);
    return;
  }

  if (strcmp(cmd, "radioMode") == 0) {
    const char* mode = doc["mode"];
    if (mode && strcmp(mode, "wifi") == 0) {
      ble::notifyError("not_supported", "WiFi on FakeTech: нет радиомодуля; только BLE + LoRa", lineCmdId);
      return;
    }
    /* mode: ble — уже в режиме BLE, ответ не обязателен для _sendCmd */
    return;
  }

  if (strcmp(cmd, "wifi") == 0) {
    ble::notifyError("not_supported", "WiFi on FakeTech: нет; используйте BLE", lineCmdId);
    return;
  }

  if (strcmp(cmd, "ota") == 0) {
    ble::notifyError("not_supported", "OTA: на FakeTech нет Wi‑Fi AP; прошивка через USB DFU (firmware.zip)", lineCmdId);
    return;
  }

  if (strcmp(cmd, "voice") == 0) {
    ble::notifyError("not_supported", "voice not implemented on faketec", lineCmdId);
    return;
  }

  if (strcmp(cmd, "location") == 0) {
    float lat = doc["lat"] | 0.0f;
    float lon = doc["lon"] | 0.0f;
    int16_t alt = (int16_t)(doc["alt"] | 0);
    uint16_t radiusM = (uint16_t)(doc["radiusM"] | 0);
    uint32_t expiryEpochSec = (uint32_t)(doc["expiryEpochSec"] | 0);
    if (s_onLocation) s_onLocation(lat, lon, alt, radiusM, expiryEpochSec);
    return;
  }

  if (strcmp(cmd, "sf") == 0 || strcmp(cmd, "loraSf") == 0) {
    int sf = doc["sf"] | 7;
    if (sf >= 7 && sf <= 12) radio::setSpreadingFactor((uint8_t)sf);
    ble::notifyInfo(lineCmdId);
    return;
  }

  if (strcmp(cmd, "modemPreset") == 0) {
    int p = doc["preset"] | -1;
    if (p >= 0 && p <= 3) {
      applyModemPreset(p);
      radio::applyRegion(region::getFreq(), region::getPower());
    }
    ble::notifyInfo(lineCmdId);
    return;
  }

  if (strcmp(cmd, "modemCustom") == 0) {
    int sf = doc["sf"] | 7;
    double bw = doc["bw"] | 125.0;
    int cr = doc["cr"] | 5;
    if (sf >= 7 && sf <= 12 && cr >= 5 && cr <= 8) {
      radio::applyModemConfig((uint8_t)sf, (float)bw, (uint8_t)cr);
      radio::applyRegion(region::getFreq(), region::getPower());
    }
    ble::notifyInfo(lineCmdId);
    return;
  }

  if (strcmp(cmd, "espnowChannel") == 0 || strcmp(cmd, "espnowAdaptive") == 0) {
    /* Нет ESP-NOW на nRF; ответ node завершает tracked-запрос как на V3. */
    ble::notifyInfo(lineCmdId);
    return;
  }

  if (strcmp(cmd, "traceroute") == 0) {
    emitFaketecRoutes(++s_bleSyncSeq, lineCmdId);
    return;
  }

  if (strcmp(cmd, "regeneratePin") == 0) {
    ble::notifyInfo(lineCmdId);
    return;
  }

  if (strcmp(cmd, "selftest") == 0) {
    ble::notifySelftest(true, true, 0, heap_metrics_free_bytes(), lineCmdId);
    return;
  }

  if (strcmp(cmd, "gps") == 0) {
    ble::notifyGps(false, false, false, lineCmdId);
    return;
  }

  if (strcmp(cmd, "powersave") == 0) {
    s_powersave = doc["enabled"] == true;
    emitFaketecNode(++s_bleSyncSeq, lineCmdId);
    return;
  }

  if (strcmp(cmd, "bleOtaStart") == 0) {
    emitBleOtaResult(false, "FakeTech: OTA через USB DFU (firmware.zip)");
    return;
  }
  if (strcmp(cmd, "bleOtaEnd") == 0 || strcmp(cmd, "bleOtaAbort") == 0) {
    return;
  }

  /* Groups V2 — паритет с firmware/src/ble/ble.cpp (ESP) и docs/API.md. */
  if (strcmp(cmd, "groupCreate") == 0) {
    const char* groupUid = doc["groupUid"];
    const char* groupTag = doc["groupTag"];
    const char* canonicalName = doc["displayName"];
    uint32_t channelId32 = parseJsonChannelId32(doc["channelId32"]);
    const char* keyB64 = doc["groupKey"];
    uint16_t keyVersion = (uint16_t)(doc["keyVersion"] | 1);
    const char* roleStr = doc["myRole"];
    uint32_t revEpoch = doc["revocationEpoch"] | 0;
    if (!groupUid || !groupUid[0] || !groupTag || !groupTag[0] || !canonicalName || !canonicalName[0] ||
        channelId32 <= groups::GROUP_ALL) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v3_bad", "Missing groupUid/groupTag/canonicalName/channelId32",
          lineCmdId);
      return;
    }
    if (strchr(canonicalName, '|') != nullptr) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v3_bad", "canonicalName contains invalid separator", lineCmdId);
      return;
    }
    uint8_t key[32];
    if (keyB64 && keyB64[0]) {
      size_t decLen = 0;
      if (!crypto::base64Decode(keyB64, strlen(keyB64), key, sizeof(key), &decLen) || decLen != 32) {
        faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_key_bad", "Bad groupKey", lineCmdId);
        return;
      }
    } else {
      randombytes_buf(key, sizeof(key));
    }
    groups::GroupRole role = parseGroupRole(roleStr);
    if (role == groups::GroupRole::None) role = groups::GroupRole::Owner;
    if (!groups::upsertGroupV2(groupUid, channelId32, groupTag, canonicalName, key, keyVersion, role, revEpoch)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v3_store_failed", "Failed to store V3 group", lineCmdId);
      return;
    }
    if (role == groups::GroupRole::Owner) {
      if (!group_owner_sign::ready() || !groups::setOwnerSignPubKeyV2(groupUid, group_owner_sign::publicKey32())) {
        faketecNotifyGroupSecurityErrorV2(groupUid, "group_v3_store_failed", "Failed to set owner signing key", lineCmdId);
        return;
      }
    }
    {
      const uint16_t appliedKv = keyVersion > 0 ? keyVersion : 1;
      (void)groups::ackKeyAppliedV2(groupUid, appliedKv);
    }
    faketecNotifyGroupStatusV2(groupUid, false, lineCmdId);
    return;
  }
  if (strcmp(cmd, "groupInviteCreate") == 0) {
    const char* groupUid = doc["groupUid"];
    const char* roleStr = doc["role"];
    uint32_t ttlSec = (uint32_t)(doc["ttlSec"] | 600);
    if (!groupUid || !groupUid[0]) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Missing groupUid", lineCmdId);
      return;
    }
    uint32_t channelId32 = 0;
    char groupTag[groups::GROUP_TAG_MAX_LEN + 1] = {0};
    char canonicalName[groups::GROUP_CANONICAL_NAME_MAX_LEN + 1] = {0};
    uint16_t keyVersion = 0;
    groups::GroupRole myRole = groups::GroupRole::None;
    uint8_t key[32];
    if (!groups::getGroupV2(groupUid, &channelId32, groupTag, sizeof(groupTag), canonicalName, sizeof(canonicalName),
            &keyVersion, &myRole, nullptr, nullptr) ||
        !groups::getGroupKeyV2(groupUid, key, &keyVersion)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Unknown groupUid", lineCmdId);
      return;
    }
    if (myRole != groups::GroupRole::Owner) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v31_invite_denied", "Only owner can issue signed invite",
          lineCmdId);
      return;
    }
    if (strchr(canonicalName, '|') != nullptr) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "canonicalName contains invalid separator",
          lineCmdId);
      return;
    }
    uint8_t ownerSignPubKey[groups::GROUP_OWNER_SIGN_PUBKEY_LEN] = {0};
    if (!groups::getOwnerSignPubKeyV2(groupUid, ownerSignPubKey)) {
      if (!group_owner_sign::ready() || !groups::setOwnerSignPubKeyV2(groupUid, group_owner_sign::publicKey32()) ||
          !groups::getOwnerSignPubKeyV2(groupUid, ownerSignPubKey)) {
        faketecNotifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Owner signing key is not set", lineCmdId);
        return;
      }
    }
    if (memcmp(ownerSignPubKey, group_owner_sign::publicKey32(), sizeof(ownerSignPubKey)) != 0) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v31_invite_denied", "Owner signing key mismatch", lineCmdId);
      return;
    }
    size_t keyB64Len = 0;
    char keyB64[80] = {0};
    if (!crypto::base64Encode(key, sizeof(key), keyB64, sizeof(keyB64), &keyB64Len)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Key encode failed", lineCmdId);
      return;
    }
    keyB64[keyB64Len] = '\0';
    size_t ownerPubB64Len = 0;
    char ownerPubB64[96] = {0};
    if (!crypto::base64Encode(ownerSignPubKey, sizeof(ownerSignPubKey), ownerPubB64, sizeof(ownerPubB64),
            &ownerPubB64Len)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Owner key encode failed", lineCmdId);
      return;
    }
    ownerPubB64[ownerPubB64Len] = '\0';
    if (!roleStr || !roleStr[0]) roleStr = "member";
    const uint32_t expiresAt = (uint32_t)(millis() / 1000) + ttlSec;
    char raw[420] = {0};
    int rawLen = snprintf(raw, sizeof(raw), "v3.1|%s|%lu|%s|%s|%u|%s|%s|%lu|%s", groupUid,
        (unsigned long)channelId32, groupTag, canonicalName, (unsigned)keyVersion, keyB64, roleStr,
        (unsigned long)expiresAt, ownerPubB64);
    if (rawLen <= 0 || (size_t)rawLen >= sizeof(raw)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Invite payload too large", lineCmdId);
      return;
    }
    unsigned char sig[group_owner_sign::SIGNATURE_LEN] = {0};
    if (!group_owner_sign::signDetached((const uint8_t*)raw, (size_t)rawLen, sig)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Invite sign failed", lineCmdId);
      return;
    }
    size_t sigB64Len = 0;
    char sigB64[140] = {0};
    if (!crypto::base64Encode(sig, sizeof(sig), sigB64, sizeof(sigB64), &sigB64Len)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Signature encode failed", lineCmdId);
      return;
    }
    sigB64[sigB64Len] = '\0';
    char inviteRaw[600] = {0};
    int inviteRawLen = snprintf(inviteRaw, sizeof(inviteRaw), "%s|%s", raw, sigB64);
    if (inviteRawLen <= 0 || (size_t)inviteRawLen >= sizeof(inviteRaw)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Invite payload too large", lineCmdId);
      return;
    }
    size_t inviteB64Len = 0;
    char inviteB64[640] = {0};
    if (!crypto::base64Encode((const uint8_t*)inviteRaw, (size_t)inviteRawLen, inviteB64, sizeof(inviteB64),
            &inviteB64Len)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Invite encode failed", lineCmdId);
      return;
    }
    inviteB64[inviteB64Len] = '\0';
    JsonDocument ev;
    ev["evt"] = "groupInvite";
    ev["groupUid"] = groupUid;
    ev["role"] = roleStr;
    ev["invite"] = inviteB64;
    ev["expiresAt"] = expiresAt;
    ev["channelId32"] = channelId32;
    ev["canonicalName"] = canonicalName;
    applyCmdId(ev, lineCmdId);
    char out[2048];
    const size_t n = serializeJson(ev, out, sizeof(out));
    if (n == 0 || n >= sizeof(out)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Invite JSON serialize failed", lineCmdId);
      return;
    }
    sendEvt(out);
    return;
  }
  if (strcmp(cmd, "groupInviteAccept") == 0) {
    const char* inviteB64 = doc["invite"];
    if (!inviteB64 || !inviteB64[0]) {
      faketecNotifyGroupSecurityErrorV2(nullptr, "group_v2_invite_bad", "Missing invite", lineCmdId);
      return;
    }
    uint8_t rawBuf[700] = {0};
    size_t rawLen = 0;
    if (!crypto::base64Decode(inviteB64, strlen(inviteB64), rawBuf, sizeof(rawBuf) - 1, &rawLen) || rawLen == 0) {
      faketecNotifyGroupSecurityErrorV2(nullptr, "group_v2_invite_bad", "Bad invite base64", lineCmdId);
      return;
    }
    rawBuf[rawLen] = 0;
    char* savePtr = nullptr;
    char* version = strtok_r((char*)rawBuf, "|", &savePtr);
    char* groupUid = strtok_r(nullptr, "|", &savePtr);
    char* channelStr = strtok_r(nullptr, "|", &savePtr);
    char* groupTag = strtok_r(nullptr, "|", &savePtr);
    char* canonicalName = strtok_r(nullptr, "|", &savePtr);
    char* keyVersionStr = strtok_r(nullptr, "|", &savePtr);
    char* keyB64 = strtok_r(nullptr, "|", &savePtr);
    char* roleStr = strtok_r(nullptr, "|", &savePtr);
    char* expiresStr = strtok_r(nullptr, "|", &savePtr);
    char* ownerPubB64 = strtok_r(nullptr, "|", &savePtr);
    char* sigB64 = strtok_r(nullptr, "|", &savePtr);
    if (!version || strcmp(version, "v3.1") != 0 || !groupUid || !channelStr || !groupTag || !canonicalName ||
        !canonicalName[0] || !keyVersionStr || !keyB64 || !roleStr || !expiresStr || !ownerPubB64 || !ownerPubB64[0] ||
        !sigB64 || !sigB64[0]) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Malformed invite", lineCmdId);
      return;
    }
    char signedRaw[420] = {0};
    int signedRawLen = snprintf(signedRaw, sizeof(signedRaw), "v3.1|%s|%s|%s|%s|%s|%s|%s|%s|%s", groupUid, channelStr,
        groupTag, canonicalName, keyVersionStr, keyB64, roleStr, expiresStr, ownerPubB64);
    if (signedRawLen <= 0 || (size_t)signedRawLen >= sizeof(signedRaw)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Malformed signed payload", lineCmdId);
      return;
    }
    uint8_t ownerSignPubKey[groups::GROUP_OWNER_SIGN_PUBKEY_LEN] = {0};
    size_t ownerSignPubKeyLen = 0;
    if (!crypto::base64Decode(ownerPubB64, strlen(ownerPubB64), ownerSignPubKey, sizeof(ownerSignPubKey),
            &ownerSignPubKeyLen) ||
        ownerSignPubKeyLen != sizeof(ownerSignPubKey)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Invalid owner signing key", lineCmdId);
      return;
    }
    unsigned char sig[group_owner_sign::SIGNATURE_LEN] = {0};
    size_t sigLen = 0;
    if (!crypto::base64Decode(sigB64, strlen(sigB64), sig, sizeof(sig), &sigLen) ||
        sigLen != group_owner_sign::SIGNATURE_LEN) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Invalid signature encoding", lineCmdId);
      return;
    }
    if (!group_owner_sign::verifyDetached(sig, (const uint8_t*)signedRaw, (size_t)signedRawLen, ownerSignPubKey)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Owner signature verification failed",
          lineCmdId);
      return;
    }
    const uint32_t nowSec = (uint32_t)(millis() / 1000);
    const uint32_t expiresAt = (uint32_t)strtoul(expiresStr, nullptr, 10);
    if (expiresAt == 0 || nowSec > expiresAt) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_invite_expired", "Invite expired", lineCmdId);
      return;
    }
    const uint32_t channelId32 = (uint32_t)strtoul(channelStr, nullptr, 10);
    const uint16_t keyVersion = (uint16_t)strtoul(keyVersionStr, nullptr, 10);
    uint8_t key[32];
    size_t keyDecLen = 0;
    if (channelId32 <= groups::GROUP_ALL ||
        !crypto::base64Decode(keyB64, strlen(keyB64), key, sizeof(key), &keyDecLen) || keyDecLen != 32) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_invite_bad", "Invalid key/channel", lineCmdId);
      return;
    }
    groups::GroupRole role = parseGroupRole(roleStr);
    if (role == groups::GroupRole::None) role = groups::GroupRole::Member;
    uint8_t pinnedOwnerSignPubKey[groups::GROUP_OWNER_SIGN_PUBKEY_LEN] = {0};
    if (groups::getOwnerSignPubKeyV2(groupUid, pinnedOwnerSignPubKey) &&
        memcmp(pinnedOwnerSignPubKey, ownerSignPubKey, sizeof(ownerSignPubKey)) != 0) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Owner signing key mismatch", lineCmdId);
      return;
    }
    if (groups::getGroupV2(groupUid, nullptr, nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, nullptr)) {
      faketecNotifyGroupStatusV2(groupUid, true, lineCmdId);
      return;
    }
    if (!groups::upsertGroupV2(groupUid, channelId32, groupTag, canonicalName, key, keyVersion > 0 ? keyVersion : 1,
            role, 0)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_store_failed", "Cannot store accepted invite", lineCmdId);
      return;
    }
    if (!groups::setOwnerSignPubKeyV2(groupUid, ownerSignPubKey)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v31_invite_bad", "Cannot persist owner signing key", lineCmdId);
      return;
    }
    {
      const uint16_t appliedKv = keyVersion > 0 ? keyVersion : 1;
      (void)groups::ackKeyAppliedV2(groupUid, appliedKv);
    }
    faketecNotifyGroupStatusV2(groupUid, false, lineCmdId);
    return;
  }
  if (strcmp(cmd, "groupGrantIssue") == 0) {
    const char* groupUid = doc["groupUid"];
    const char* subjectId = doc["subjectId"];
    const char* roleStr = doc["role"];
    if (!groupUid || !groupUid[0] || !subjectId || !subjectId[0] || !roleStr || !roleStr[0]) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_grant_bad", "Missing groupUid/subjectId/role", lineCmdId);
      return;
    }
    if (isSelfNodeHex(subjectId)) {
      groups::GroupRole role = parseGroupRole(roleStr);
      if (role == groups::GroupRole::None || !groups::setGroupRoleV2(groupUid, role)) {
        faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_grant_bad", "Invalid role or unknown group", lineCmdId);
        return;
      }
    }
    faketecNotifyGroupStatusV2(groupUid, false, lineCmdId);
    return;
  }
  if (strcmp(cmd, "groupRevoke") == 0) {
    const char* groupUid = doc["groupUid"];
    const char* subjectId = doc["subjectId"];
    uint32_t revEpoch = doc["revocationEpoch"] | 0;
    if (!groupUid || !groupUid[0] || !subjectId || !subjectId[0]) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_revoke_bad", "Missing groupUid/subjectId", lineCmdId);
      return;
    }
    if (isSelfNodeHex(subjectId)) {
      groups::setGroupRoleV2(groupUid, groups::GroupRole::None);
      uint32_t curEpoch = 0;
      groups::GroupRole curRole = groups::GroupRole::None;
      if (groups::getGroupV2(groupUid, nullptr, nullptr, 0, nullptr, 0, nullptr, &curRole, &curEpoch, nullptr)) {
        if (revEpoch <= curEpoch) revEpoch = curEpoch + 1;
      }
      groups::setRevocationEpochV2(groupUid, revEpoch);
    }
    faketecNotifyGroupStatusV2(groupUid, false, lineCmdId);
    return;
  }
  if (strcmp(cmd, "groupRekey") == 0) {
    const char* groupUid = doc["groupUid"];
    const char* keyB64 = doc["groupKey"];
    const char* rekeyOpId = doc["rekeyOpId"];
    uint16_t keyVersion = (uint16_t)(doc["keyVersion"] | 0);
    if (!groupUid || !groupUid[0]) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_rekey_bad", "Missing groupUid", lineCmdId);
      return;
    }
    if (keyVersion == 0) {
      uint16_t curVer = 0;
      if (groups::getGroupV2(groupUid, nullptr, nullptr, 0, nullptr, 0, &curVer, nullptr, nullptr, nullptr)) {
        uint32_t next = (uint32_t)curVer + 1u;
        if (next == 0 || next > 0xFFFFu) {
          keyVersion = 0xFFFFu;
        } else {
          keyVersion = (uint16_t)next;
        }
      } else {
        keyVersion = 1;
      }
    }
    uint8_t key[32];
    if (keyB64 && keyB64[0]) {
      size_t decLen = 0;
      if (!crypto::base64Decode(keyB64, strlen(keyB64), key, sizeof(key), &decLen) || decLen != 32) {
        faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_key_bad", "Bad groupKey", lineCmdId);
        return;
      }
    } else {
      randombytes_buf(key, sizeof(key));
    }
    if (!groups::updateGroupKeyV2(groupUid, key, keyVersion)) {
      faketecNotifyGroupSecurityErrorV2(groupUid, "group_v2_rekey_bad", "Unknown group", lineCmdId);
      return;
    }
    groups::GroupRole role = groups::GroupRole::None;
    uint16_t appliedVersion = 0;
    if (groups::getGroupV2(groupUid, nullptr, nullptr, 0, nullptr, 0, &appliedVersion, &role, nullptr, nullptr) &&
        role != groups::GroupRole::None) {
      groups::ackKeyAppliedV2(groupUid, appliedVersion);
    }
    faketecNotifyGroupRekeyProgressV2(groupUid, rekeyOpId, appliedVersion, lineCmdId);
    faketecNotifyGroupStatusV2(groupUid, false, lineCmdId);
    return;
  }
  if (strcmp(cmd, "groupAckKeyApplied") == 0) {
    const char* gu = doc["groupUid"];
    uint16_t kv = (uint16_t)(doc["keyVersion"] | 0);
    if (!gu || !gu[0] || kv == 0) {
      faketecNotifyGroupSecurityErrorV2(gu, "group_v2_ack_bad", "Missing groupUid/keyVersion", lineCmdId);
      return;
    }
    if (!groups::ackKeyAppliedV2(gu, kv)) {
      faketecNotifyGroupSecurityErrorV2(gu, "group_v2_ack_bad", "Ack failed", lineCmdId);
      return;
    }
    char memberHex[17];
    appendSelfNodeHex(memberHex);
    faketecNotifyGroupMemberKeyStateV2(gu, memberHex, "applied", (uint32_t)(millis() / 1000), lineCmdId);
    faketecNotifyGroupStatusV2(gu, false, lineCmdId);
    return;
  }
  if (strcmp(cmd, "groupStatus") == 0) {
    const char* gu = doc["groupUid"];
    if (!gu || !gu[0]) {
      faketecNotifyGroupSecurityErrorV2(gu, "group_v2_bad", "Missing groupUid", lineCmdId);
      return;
    }
    if (!groups::getGroupV2(gu, nullptr, nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, nullptr)) {
      faketecNotifyGroupSecurityErrorV2(gu, "group_v2_bad", "Unknown group", lineCmdId);
      return;
    }
    faketecNotifyGroupStatusV2(gu, false, lineCmdId);
    return;
  }
  if (strcmp(cmd, "groupCanonicalRename") == 0) {
    const char* gu = doc["groupUid"];
    const char* cn = doc["canonicalName"];
    if (!gu || !gu[0] || !cn || !cn[0]) {
      faketecNotifyGroupSecurityErrorV2(gu, "group_v3_name_bad", "Missing groupUid/canonicalName", lineCmdId);
      return;
    }
    groups::GroupRole role = groups::GroupRole::None;
    if (!groups::getGroupV2(gu, nullptr, nullptr, 0, nullptr, 0, nullptr, &role, nullptr, nullptr)) {
      faketecNotifyGroupSecurityErrorV2(gu, "group_v3_name_bad", "Unknown group", lineCmdId);
      return;
    }
    if (strchr(cn, '|') != nullptr) {
      faketecNotifyGroupSecurityErrorV2(gu, "group_v3_name_bad", "canonicalName contains invalid separator", lineCmdId);
      return;
    }
    if (role != groups::GroupRole::Owner) {
      faketecNotifyGroupSecurityErrorV2(gu, "group_v3_name_denied", "Only owner can rename canonicalName", lineCmdId);
      return;
    }
    if (!groups::setCanonicalNameV2(gu, cn)) {
      faketecNotifyGroupSecurityErrorV2(gu, "group_v3_name_bad", "Cannot set canonicalName", lineCmdId);
      return;
    }
    faketecNotifyGroupStatusV2(gu, false, lineCmdId);
    return;
  }
  if (strcmp(cmd, "groupSyncSnapshot") == 0) {
    JsonVariant groupsVar = doc["groups"];
    if (!groupsVar.is<JsonArray>()) {
      faketecNotifyGroupSecurityErrorV2(nullptr, "group_v2_snapshot_bad", "groups must be array", lineCmdId);
      return;
    }
    JsonArray arr = groupsVar.as<JsonArray>();
    for (JsonVariant v : arr) {
      if (!v.is<JsonObject>()) continue;
      JsonObject g = v.as<JsonObject>();
      const char* groupUid = g["groupUid"];
      const char* groupTag = g["groupTag"];
      const char* canonicalName = g["canonicalName"];
      const char* keyB64 = g["groupKey"];
      const char* roleStr = g["myRole"];
      uint32_t channelId32 = parseJsonChannelId32(g["channelId32"]);
      uint16_t keyVersion = (uint16_t)(g["keyVersion"] | 1);
      uint32_t revEpoch = g["revocationEpoch"] | 0;
      if (!groupUid || !groupUid[0] || !groupTag || !groupTag[0] || !canonicalName || !canonicalName[0] ||
          channelId32 <= groups::GROUP_ALL || !keyB64 || !keyB64[0]) {
        continue;
      }
      uint8_t key[32];
      size_t decLen = 0;
      if (!crypto::base64Decode(keyB64, strlen(keyB64), key, sizeof(key), &decLen) || decLen != 32) continue;
      groups::GroupRole role = parseGroupRole(roleStr);
      if (role == groups::GroupRole::None) role = groups::GroupRole::Member;
      if (!groups::upsertGroupV2(groupUid, channelId32, groupTag, canonicalName, key, keyVersion, role, revEpoch)) {
        continue;
      }
      if (g["ackApplied"] == true) groups::ackKeyAppliedV2(groupUid, keyVersion);
    }
    emitFaketecGroups(++s_bleSyncSeq, lineCmdId);
    return;
  }
  if (strcmp(cmd, "groupLeave") == 0) {
    const char* gu = doc["groupUid"];
    if (!gu || !gu[0]) {
      faketecNotifyGroupSecurityErrorV2(gu, "group_v2_leave_bad", "Missing groupUid", lineCmdId);
      return;
    }
    if (!groups::removeGroupV2(gu)) {
      faketecNotifyGroupSecurityErrorV2(gu, "group_v2_leave_bad", "Unknown group", lineCmdId);
      return;
    }
    emitFaketecGroups(++s_bleSyncSeq, lineCmdId);
    return;
  }

  if (strncmp(cmd, "group", 5) == 0 && strcmp(cmd, "groups") != 0) {
    if (lineCmdId != 0) ble::notifyError("group_unknown", "unknown group cmd on faketec", lineCmdId);
    return;
  }
}

namespace ble {

bool init() {
#ifdef RIFTLINK_SKIP_BLE
  RIFTLINK_DIAG("BLE", "event=STACK skip=1 reason=RIFTLINK_SKIP_BLE");
  yield();
  s_bleReady = false;
  return true;
#else
  static bool s_initAttempted = false;
  if (s_initAttempted) {
    return true;
  }
  s_initAttempted = true;

  RIFTLINK_DIAG("BLE", "event=INIT stage=bluefruit_cfg");
  yield();

  s_bleuart = new BLEUart(2048);
  if (!s_bleuart) {
    RIFTLINK_DIAG("BLE", "event=INIT ok=0 cause=oom_bleuart");
    s_bleReady = false;
    return true;
  }

  Bluefruit.autoConnLed(false);
  Bluefruit.configPrphBandwidth(BANDWIDTH_HIGH);

  RIFTLINK_DIAG("BLE", "event=INIT stage=softdevice_begin");
  yield();

  Bluefruit.begin();

  /* Как src/ble/ble.cpp (NimBLE): имя в эфире RL-<полный node id hex>, не RiftLink- + 2 байта. */
  const uint8_t* nid = node::getId();
  char advName[20];
  snprintf(advName, sizeof(advName), "RL-%02X%02X%02X%02X%02X%02X%02X%02X",
      nid[0], nid[1], nid[2], nid[3], nid[4], nid[5], nid[6], nid[7]);
  Bluefruit.setName(advName);
  (void)Bluefruit.setTxPower(4);

  if (s_bleuart->begin() != ERROR_NONE) {
    RIFTLINK_DIAG("BLE", "event=BLEUART_BEGIN ok=0");
    s_bleReady = false;
    return true;
  }

  s_bleReady = true;
  startAdv();
  RIFTLINK_DIAG("BLE", "event=ADVERTISING ok=1 name=%s tx_dbm=4 service=NUS", advName);
  yield();
  return true;
#endif
}

void update() {
#ifndef RIFTLINK_SKIP_BLE
  /* Сначала NUS: команды с телефона не должны ждать USB Serial и не отбрасываться из‑за defer init. */
  if (s_bleReady && s_bleuart) {
    int bleDrain = 0;
    while (s_bleuart->available() > 0) {
      int c = s_bleuart->read();
      if (c < 0) break;
      if (c == '\n' || c == '\r') {
        s_bleLine.trim();
        if (s_bleLine.length() > 0 && s_bleLine.length() <= 512) {
          processJsonLine(s_bleLine.c_str());
        }
        s_bleLine = "";
        continue;
      }
      if (s_bleLine.length() < 512) s_bleLine += (char)c;
      if (++bleDrain >= 48) {
        bleDrain = 0;
        yield();
      }
    }

    if (Bluefruit.connected() == 0) {
      s_notifyWasOff = true;
    } else if (s_bleuart->notifyEnabled()) {
      if (s_notifyWasOff) {
        s_notifyWasOff = false;
        notifyInfo(0);
      }
    } else {
      s_notifyWasOff = true;
    }
  }
#endif

  {
    int serialDrain = 0;
    while (Serial.available() > 0) {
      int c = Serial.read();
      if (c < 0) break;
      if (c == '\n' || c == '\r') {
        s_serialLine.trim();
        if (s_serialLine.length() > 0 && s_serialLine.length() <= 512) processJsonLine(s_serialLine.c_str());
        s_serialLine = "";
        continue;
      }
      if (s_serialLine.length() < 512) s_serialLine += (char)c;
      if (++serialDrain >= 48) {
        serialDrain = 0;
        yield();
      }
    }
  }

  yield();
}

void setOnSend(void (*cb)(const uint8_t* to, const char* text, uint8_t ttlMinutes)) {
  s_onSend = cb;
}

void setOnLocation(void (*cb)(float lat, float lon, int16_t alt, uint16_t radiusM, uint32_t expiryEpochSec)) {
  s_onLocation = cb;
}

void notifyMsg(const uint8_t* from, const char* text, uint32_t msgId, int rssi, uint8_t ttlMinutes) {
  JsonDocument doc;
  doc["evt"] = "msg";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) sprintf(fromHex + i * 2, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["text"] = text;
  doc["msgId"] = msgId;
  doc["rssi"] = rssi;
  doc["ttl"] = ttlMinutes;
  doc["type"] = "text";
  doc["lane"] = "normal";
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifyTelemetry(const uint8_t* from, uint16_t batteryMv, uint16_t heapKb, int rssi) {
  if (!from) return;
  JsonDocument doc;
  doc["evt"] = "telemetry";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) sprintf(fromHex + i * 2, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["battery"] = batteryMv;
  doc["heapKb"] = heapKb;
  if (rssi != 0) doc["rssi"] = rssi;
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifyLocation(const uint8_t* from, float lat, float lon, int16_t alt, int rssi) {
  if (!from) return;
  JsonDocument doc;
  doc["evt"] = "location";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) sprintf(fromHex + i * 2, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["lat"] = lat;
  doc["lon"] = lon;
  doc["alt"] = alt;
  if (rssi != 0) doc["rssi"] = rssi;
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifySent(const uint8_t* to, uint32_t msgId) {
  JsonDocument doc;
  doc["evt"] = "sent";
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) sprintf(toHex + i * 2, "%02X", to[i]);
  doc["to"] = toHex;
  doc["msgId"] = msgId;
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifyUndelivered(const uint8_t* to, uint32_t msgId) {
  JsonDocument doc;
  doc["evt"] = "undelivered";
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) sprintf(toHex + i * 2, "%02X", to[i]);
  doc["to"] = toHex;
  doc["msgId"] = msgId;
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifyBroadcastDelivery(uint32_t msgId, int delivered, int total) {
  JsonDocument doc;
  doc["evt"] = "broadcast_delivery";
  doc["msgId"] = msgId;
  doc["delivered"] = delivered;
  doc["total"] = total;
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifyTimeCapsuleReleased(const uint8_t* to, uint32_t msgId, uint8_t triggerType) {
  JsonDocument doc;
  doc["evt"] = "time_capsule_released";
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) sprintf(toHex + i * 2, "%02X", to[i]);
  doc["to"] = toHex;
  doc["msgId"] = msgId;
  doc["triggerType"] = triggerType;
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifyDelivered(const uint8_t* from, uint32_t msgId, int rssi) {
  JsonDocument doc;
  doc["evt"] = "delivered";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) sprintf(fromHex + i * 2, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["msgId"] = msgId;
  if (rssi != 0) doc["rssi"] = rssi;
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifyRead(const uint8_t* from, uint32_t msgId, int rssi) {
  JsonDocument doc;
  doc["evt"] = "read";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) sprintf(fromHex + i * 2, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["msgId"] = msgId;
  if (rssi != 0) doc["rssi"] = rssi;
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifyPong(const uint8_t* from, int rssi, uint16_t pingPktId) {
  if (!from) return;
  JsonDocument doc;
  doc["evt"] = "pong";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) sprintf(fromHex + i * 2, "%02X", from[i]);
  doc["from"] = fromHex;
  if (rssi != 0) doc["rssi"] = rssi;
  if (pingPktId != 0) doc["pingPktId"] = pingPktId;
  const uint32_t cid = peekPingCmdIdForFrom(from);
  applyCmdId(doc, cid);
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
  takePingCmdIdForFrom(from);
}

void clearPingRetryForPeer(const uint8_t* from) {
  (void)from;
}

void notifyInfo(uint32_t cmdId) {
  const uint32_t seq = ++s_bleSyncSeq;
  emitFaketecNode(seq, cmdId);
  yield();
  delay(3);
  emitFaketecNeighbors(seq, cmdId);
  yield();
  delay(3);
  emitFaketecRoutes(seq, cmdId);
  yield();
  delay(2);
  emitFaketecGroups(seq, cmdId);
}

void notifyNeighbors(uint32_t cmdId) {
  RIFTLINK_DIAG("TRACE", "notifyNeighbors_begin cmdId=%u", (unsigned)cmdId);
  emitFaketecNeighbors(++s_bleSyncSeq, cmdId);
  RIFTLINK_DIAG("TRACE", "notifyNeighbors_end");
  yield();
}

void notifyRoutes(uint32_t cmdId) {
  emitFaketecRoutes(++s_bleSyncSeq, cmdId);
}

void notifyGroups(uint32_t cmdId) {
  emitFaketecGroups(++s_bleSyncSeq, cmdId);
}

void notifyRelayProof(const uint8_t* relayedBy, const uint8_t* from, const uint8_t* to, uint16_t pktId, uint8_t opcode) {
#ifndef RIFTLINK_SKIP_BLE
  if (!relayedBy || !from || !to) return;
  JsonDocument doc;
  doc["evt"] = "relayProof";
  char byHex[17] = {0};
  char fromHex[17] = {0};
  char toHex[17] = {0};
  for (int i = 0; i < 8; i++) {
    snprintf(byHex + i * 2, 3, "%02X", relayedBy[i]);
    snprintf(fromHex + i * 2, 3, "%02X", from[i]);
    snprintf(toHex + i * 2, 3, "%02X", to[i]);
  }
  doc["relayedBy"] = byHex;
  doc["from"] = fromHex;
  doc["to"] = toHex;
  doc["pktId"] = pktId;
  doc["opcode"] = opcode;
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
#else
  (void)relayedBy;
  (void)from;
  (void)to;
  (void)pktId;
  (void)opcode;
#endif
}

void notifyError(const char* code, const char* msg, uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "error";
  doc["code"] = code;
  doc["msg"] = msg;
  applyCmdId(doc, cmdId);
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifyInvite(uint32_t cmdId, int ttlSec) {
  JsonDocument doc;
  doc["evt"] = "invite";
  const uint8_t* id = node::getId();
  char idHex[17] = {0};
  for (int i = 0; i < 8; i++) snprintf(idHex + i * 2, 3, "%02X", id[i]);
  doc["id"] = idHex;
  char b64[48];
  if (crypto::getInvitePublicKeyBase64(b64, sizeof(b64))) {
    doc["pubKey"] = b64;
  } else {
    doc["pubKey"] = "";
  }
  if (ttlSec > 0) doc["inviteTtlMs"] = (uint32_t)ttlSec * 1000u;
  applyCmdId(doc, cmdId);
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifyGps(bool present, bool enabled, bool hasFix, uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "gps";
  doc["present"] = present;
  doc["enabled"] = enabled;
  doc["hasFix"] = hasFix;
  applyCmdId(doc, cmdId);
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifySelftest(bool radioOk, bool displayOk, uint16_t batteryMv, uint32_t heapFree, uint32_t cmdId) {
  JsonDocument doc;
  doc["evt"] = "selftest";
  doc["radioOk"] = radioOk;
  doc["displayOk"] = displayOk;
  doc["antennaOk"] = true;
  doc["batteryMv"] = batteryMv;
  doc["heapFree"] = heapFree;
  applyCmdId(doc, cmdId);
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

bool isConnected() {
#ifndef RIFTLINK_SKIP_BLE
  return s_bleReady && Bluefruit.connected() > 0;
#else
  return false;
#endif
}

}  // namespace ble
