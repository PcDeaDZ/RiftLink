/**
 * RiftLink OTA — WiFi AP + ArduinoOTA
 * AP SSID/pass берутся из wifi::getApSsid() / wifi::getApPassword()
 * IP: 192.168.4.1
 */

#include "ota.h"
#include "wifi/wifi.h"
#include <WiFi.h>
#include <ArduinoOTA.h>

static bool s_active = false;

namespace ota {

void start() {
  if (s_active) return;
  if (!wifi::isAvailable()) {
    Serial.println("[OTA] WiFi unavailable — use USB");
    return;
  }
  const char* apSsid = wifi::getApSsid();
  const char* apPass = wifi::getApPassword();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid, apPass);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[OTA] AP started: %s / %s @ %s\n", apSsid, apPass, ip.toString().c_str());

  ArduinoOTA.setHostname("RiftLink");
  ArduinoOTA.setPassword(apPass);

  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] End");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] Progress: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t err) {
    Serial.printf("[OTA] Error %u: ", err);
    if (err == OTA_AUTH_ERROR) Serial.println("Auth failed");
    else if (err == OTA_BEGIN_ERROR) Serial.println("Begin failed");
    else if (err == OTA_CONNECT_ERROR) Serial.println("Connect failed");
    else if (err == OTA_RECEIVE_ERROR) Serial.println("Receive failed");
    else if (err == OTA_END_ERROR) Serial.println("End failed");
  });

  ArduinoOTA.begin();
  s_active = true;
}

void stop() {
  if (!s_active) return;
  ArduinoOTA.end();
  WiFi.softAPdisconnect(true);
  s_active = false;
  Serial.println("[OTA] Stopped");
}

void update() {
  if (s_active) {
    ArduinoOTA.handle();
  }
}

bool isActive() {
  return s_active;
}

}  // namespace ota
