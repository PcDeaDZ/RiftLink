/**
 * WebSocket server — WiFi-режим коммуникации с телефоном.
 * HTTP + WebSocket на порту 80, mDNS для discovery.
 * JSON-протокол идентичен BLE GATT (cmd/evt).
 */

#include "ws_server.h"
#include <esp_http_server.h>
#include <mdns.h>
#include <esp_heap_caps.h>
#include <Arduino.h>
#include <cstring>

#include "node/node.h"

static httpd_handle_t s_server = nullptr;
static int s_wsFd = -1;

static void (*s_onCmd)(const char* json, size_t len) = nullptr;

static esp_err_t wsHandler(httpd_req_t* req) {
  if (req->method == HTTP_GET) {
    // WebSocket handshake — accept
    s_wsFd = httpd_req_to_sockfd(req);
    Serial.printf("[BLE_CHAIN] stage=fw_ws action=client_connected fd=%d\n", s_wsFd);
    Serial.printf("[WS] Client connected, fd=%d\n", s_wsFd);
    return ESP_OK;
  }

  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof(frame));
  frame.type = HTTPD_WS_TYPE_TEXT;

  esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
  if (ret != ESP_OK) return ret;
  if (frame.len == 0) return ESP_OK;

  uint8_t* buf = (uint8_t*)heap_caps_malloc(frame.len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t*)malloc(frame.len + 1);
  if (!buf) return ESP_ERR_NO_MEM;

  frame.payload = buf;
  ret = httpd_ws_recv_frame(req, &frame, frame.len);
  if (ret != ESP_OK) { free(buf); return ret; }

  buf[frame.len] = '\0';

  if (s_onCmd) {
    s_onCmd((const char*)buf, frame.len);
  }

  free(buf);
  return ESP_OK;
}

namespace ws_server {

void setOnCommand(void (*cb)(const char* json, size_t len)) {
  s_onCmd = cb;
}

void start() {
  if (s_server) return;

  // mDNS
  mdns_init();
  char hostname[32];
  const uint8_t* id = node::getId();
  snprintf(hostname, sizeof(hostname), "riftlink-%02x%02x%02x%02x", id[0], id[1], id[2], id[3]);
  mdns_hostname_set(hostname);
  mdns_instance_name_set("RiftLink Node");
  mdns_service_add("RiftLink", "_riftlink", "_tcp", 80, nullptr, 0);
  Serial.printf("[WS] mDNS: %s.local\n", hostname);

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 4;
  config.stack_size = 4096;

  if (httpd_start(&s_server, &config) != ESP_OK) {
    Serial.println("[WS] Server start FAILED");
    return;
  }

  httpd_uri_t wsUri = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = wsHandler,
    .user_ctx = nullptr,
    .is_websocket = true
  };
  httpd_register_uri_handler(s_server, &wsUri);

  Serial.printf("[WS] Server started on :80/ws, heap=%u\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void stop() {
  if (!s_server) return;
  httpd_stop(s_server);
  s_server = nullptr;
  s_wsFd = -1;
  mdns_service_remove_all();
  mdns_free();
  Serial.println("[WS] Server stopped");
}

void update() {
  // esp_http_server handles requests asynchronously
}

bool hasClient() {
  return s_server && s_wsFd >= 0;
}

void sendEvent(const char* json, int len) {
  if (!s_server || s_wsFd < 0 || !json || len <= 0) {
    Serial.printf("[BLE_CHAIN] stage=fw_ws action=send_skip reason=no_client fd=%d len=%d\n", s_wsFd, len);
    return;
  }

  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof(frame));
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = (uint8_t*)json;
  frame.len = (size_t)len;

  esp_err_t ret = httpd_ws_send_frame_async(s_server, s_wsFd, &frame);
  if (ret != ESP_OK) {
    Serial.printf("[BLE_CHAIN] stage=fw_ws action=send_fail reason=%s fd=%d len=%d\n",
        esp_err_to_name(ret), s_wsFd, len);
    Serial.printf("[WS] Send failed: %s, closing fd=%d\n", esp_err_to_name(ret), s_wsFd);
    s_wsFd = -1;
  } else {
    Serial.printf("[BLE_CHAIN] stage=fw_ws action=send_ok fd=%d len=%d\n", s_wsFd, len);
  }
}

}  // namespace ws_server
