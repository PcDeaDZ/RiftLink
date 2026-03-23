/**
 * FakeTech BLE — заглушка (Serial для первой версии)
 * TODO: Bluefruit52Lib или SoftDevice GATT
 */

#include "ble.h"
#include "node.h"
#include "protocol/packet.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

static void (*s_onSend)(const uint8_t* to, const char* text, uint8_t ttlMinutes) = nullptr;

namespace ble {

bool init() {
  Serial.println("[BLE] Stub (use Serial)");
  return true;
}

void update() {
  if (Serial.available() <= 0) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0 || line.length() > 512) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) return;

  const char* cmd = doc["cmd"];
  if (!cmd) return;

  if (strcmp(cmd, "send") == 0 && s_onSend) {
    const char* text = doc["text"];
    const char* to = doc["to"];
    uint8_t ttl = doc["ttl"] | 0;
    if (text) {
      uint8_t toId[protocol::NODE_ID_LEN];
      if (to && strlen(to) >= 16) {
        for (int i = 0; i < 8; i++) {
          char hex[3] = {to[i*2], to[i*2+1], 0};
          toId[i] = (uint8_t)strtol(hex, nullptr, 16);
        }
        s_onSend(toId, text, ttl);
      } else {
        s_onSend(protocol::BROADCAST_ID, text, ttl);
      }
    }
  }
  if (strcmp(cmd, "info") == 0) notifyInfo();
}

void setOnSend(void (*cb)(const uint8_t* to, const char* text, uint8_t ttlMinutes)) {
  s_onSend = cb;
}

static void sendEvt(const char* evt) {
  Serial.println(evt);
}

void notifyMsg(const uint8_t* from, const char* text, uint32_t msgId, int rssi, uint8_t ttlMinutes) {
  JsonDocument doc;
  doc["evt"] = "msg";
  char fromHex[17] = {0};
  for (int i = 0; i < 8; i++) sprintf(fromHex + i*2, "%02X", from[i]);
  doc["from"] = fromHex;
  doc["text"] = text;
  doc["msgId"] = msgId;
  doc["rssi"] = rssi;
  doc["ttl"] = ttlMinutes;
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifyInfo() {
  JsonDocument doc;
  doc["evt"] = "node";
  doc["seq"] = 1;
  char idHex[17] = {0};
  const uint8_t* id = node::getId();
  for (int i = 0; i < 8; i++) sprintf(idHex + i*2, "%02X", id[i]);
  doc["nodeId"] = idHex;
  doc["nickname"] = node::getNickname();
  doc["platform"] = "faketec_v5";
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifyNeighbors() {
  JsonDocument doc;
  doc["evt"] = "neighbors";
  doc["count"] = 0;
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

void notifyError(const char* code, const char* msg) {
  JsonDocument doc;
  doc["evt"] = "error";
  doc["code"] = code;
  doc["msg"] = msg;
  String s;
  serializeJson(doc, s);
  sendEvt(s.c_str());
}

bool isConnected() {
  return true;
}

}  // namespace ble
