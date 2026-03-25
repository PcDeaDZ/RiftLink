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
#include <cstdint>
#include <cerrno>
#include <vector>

#include "lwip/sockets.h"

#include "node/node.h"

static httpd_handle_t s_server = nullptr;
static int s_wsFd = -1;

static void (*s_onCmd)(const char* json, size_t len) = nullptr;

/** Склейка фрагментированного TEXT (RFC6455): esp_http_server вызывает handler на каждый кадр.
 *  Иначе первый фрагмент (например 1 байт «{») уходит в JSON-парсер без остальных → cmd:info не обрабатывается. */
static std::vector<uint8_t> s_wsTextAccum;
static constexpr size_t kWsJsonAccumMax = 8192;

// Совпадает с esp_http_server/src/httpd_ws.c (без httpd_sess_get / httpd_queue_work).
namespace {
constexpr uint8_t kWsFin = 0x80U;
constexpr uint8_t kWsOpText = 0x1U;
constexpr uint8_t kWsMaskBit = 0x80U;

bool sendAll(int fd, const void* data, size_t len) {
  const auto* p = static_cast<const uint8_t*>(data);
  size_t off = 0;
  while (off < len) {
    const int n = send(fd, p + off, len - off, 0);
    if (n < 0) return false;
    if (n == 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

/** TEXT-кадр WebSocket (сервер без маски). Из loop() — без очереди httpd: иначе httpd_ws_send_data
 *  ждёт httpd, а httpd может быть в recv() на этом же fd → взаимная блокировка и закрытие сокета на телефоне. */
bool sendWsTextFrameDirect(int fd, const char* payload, size_t payloadLen) {
  uint8_t header[10];
  uint8_t txLen = 0;
  header[0] = static_cast<uint8_t>(kWsFin | kWsOpText);
  if (payloadLen <= 125) {
    header[1] = static_cast<uint8_t>(payloadLen & 0x7fU);
    txLen = 2;
  } else if (payloadLen < 65536U) {
    header[1] = 126;
    header[2] = static_cast<uint8_t>((payloadLen >> 8U) & 0xffU);
    header[3] = static_cast<uint8_t>(payloadLen & 0xffU);
    txLen = 4;
  } else {
    header[1] = 127;
    uint64_t len64 = payloadLen;
    uint8_t shift_idx = sizeof(uint64_t) - 1;
    for (int8_t idx = 2; idx <= 9; idx++) {
      header[idx] = static_cast<uint8_t>((len64 >> (shift_idx * 8)) & 0xffU);
      shift_idx--;
    }
    txLen = 10;
  }
  header[1] = static_cast<uint8_t>(header[1] & static_cast<uint8_t>(~kWsMaskBit));
  if (!sendAll(fd, header, txLen)) return false;
  if (payloadLen > 0 && payload) {
    if (!sendAll(fd, payload, payloadLen)) return false;
  }
  return true;
}
}  // namespace

static esp_err_t wsHandler(httpd_req_t* req) {
  if (!s_server) {
    return ESP_FAIL;
  }
  const int sockfd = httpd_req_to_sockfd(req);

  // После ответа 101 Switching Protocols esp_http_server вызывает этот же handler с method==GET
  // (см. httpd_uri.c: handshake → uri->handler). Официальный ws_echo_server на ЛЮБОЙ GET сразу
  // return ESP_OK и НЕ вызывает httpd_ws_recv_frame: иначе get_frame_type ещё не прочитал 1-й байт
  // кадра (он вызывается из httpd_req_new для следующих сообщений) — recv ломает разбор и даёт
  // ESP_ERR_INVALID_STATE / «не замаскирован» / мусорный opcode. Данные WS приходят с req->method==0.
  if (req->method == HTTP_GET) {
    s_wsFd = sockfd;
    Serial.printf("[BLE_CHAIN] stage=fw_ws action=client_connected fd=%d\n", s_wsFd);
    Serial.printf("[WS] Client connected, fd=%d\n", s_wsFd);
    return ESP_OK;
  }

  // Исходящие notify и hasClient() завязаны на s_wsFd — обновляем до recv.
  s_wsFd = sockfd;

  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof(frame));

  esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
  if (ret != ESP_OK) {
    Serial.printf("[BLE_CHAIN] stage=fw_ws action=recv_hdr_fail reason=%s\n", esp_err_to_name(ret));
    return ret;
  }
  if (frame.len == 0) return ESP_OK;

  uint8_t* buf = (uint8_t*)heap_caps_malloc(frame.len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t*)malloc(frame.len + 1);
  if (!buf) return ESP_ERR_NO_MEM;

  frame.payload = buf;
  ret = httpd_ws_recv_frame(req, &frame, frame.len);
  if (ret != ESP_OK) {
    Serial.printf("[BLE_CHAIN] stage=fw_ws action=recv_payload_fail reason=%s len=%u\n",
        esp_err_to_name(ret), (unsigned)frame.len);
    free(buf);
    return ret;
  }

  buf[frame.len] = '\0';

  // TEXT или BINARY — первый фрагмент сообщения; CONTINUE — продолжение (RFC6455 §5.2).
  // Некоторые клиенты шлют JSON в BINARY; раньше отбрасывали → cmd не доходил до парсера.
  const bool isTextOrBin =
      (frame.type == HTTPD_WS_TYPE_TEXT || frame.type == HTTPD_WS_TYPE_BINARY);
  const bool isContinue = (frame.type == HTTPD_WS_TYPE_CONTINUE);
  if (!isTextOrBin && !isContinue) {
    Serial.printf("[BLE_CHAIN] stage=fw_ws action=cmd_rx_skip opcode=%u len=%u\n",
        (unsigned)frame.type, (unsigned)frame.len);
    free(buf);
    return ESP_OK;
  }
  if (isTextOrBin) {
    s_wsTextAccum.clear();
  } else if (isContinue && s_wsTextAccum.empty()) {
    free(buf);
    return ESP_OK;
  }
  if (s_wsTextAccum.size() + frame.len > kWsJsonAccumMax) {
    s_wsTextAccum.clear();
    free(buf);
    Serial.println("[BLE_CHAIN] stage=fw_ws action=cmd_rx_drop reason=accum_overflow");
    return ESP_OK;
  }
  s_wsTextAccum.insert(s_wsTextAccum.end(), buf, buf + frame.len);
  free(buf);

  Serial.printf("[BLE_CHAIN] stage=fw_ws action=cmd_rx frag_len=%u final=%d accum=%u\n",
      (unsigned)frame.len, (int)frame.final, (unsigned)s_wsTextAccum.size());

  if (!frame.final) {
    return ESP_OK;
  }

  if (s_onCmd) {
    s_onCmd(reinterpret_cast<const char*>(s_wsTextAccum.data()), s_wsTextAccum.size());
  }
  s_wsTextAccum.clear();
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
  snprintf(hostname, sizeof(hostname), "riftlink-%02x%02x%02x%02x%02x%02x%02x%02x",
      id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
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
  s_wsTextAccum.clear();
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

bool sendEvent(const char* json, int len) {
  if (!s_server || s_wsFd < 0 || !json || len <= 0) {
    Serial.printf("[BLE_CHAIN] stage=fw_ws action=send_skip reason=no_client fd=%d len=%d\n", s_wsFd, len);
    return false;
  }

  if (httpd_ws_get_fd_info(s_server, s_wsFd) != HTTPD_WS_CLIENT_WEBSOCKET) {
    Serial.printf("[BLE_CHAIN] stage=fw_ws action=send_fail reason=fd_not_ws fd=%d len=%d\n", s_wsFd, len);
    s_wsFd = -1;
    return false;
  }

  if (!sendWsTextFrameDirect(s_wsFd, json, static_cast<size_t>(len))) {
    Serial.printf("[BLE_CHAIN] stage=fw_ws action=send_fail reason=send errno=%d fd=%d len=%d\n",
        errno, s_wsFd, len);
    Serial.printf("[WS] Send failed (direct WS frame), closing fd=%d\n", s_wsFd);
    s_wsFd = -1;
    return false;
  }
  Serial.printf("[BLE_CHAIN] stage=fw_ws action=send_ok fd=%d len=%d\n", s_wsFd, len);
  return true;
}

}  // namespace ws_server
