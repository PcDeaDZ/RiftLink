/**
 * BLE OTA — chunked firmware receive over BLE.
 * Uses esp_ota_* API. Buffer in PSRAM, flash writes in 4K blocks.
 */

#include "ble_ota.h"
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <mbedtls/md5.h>
#include <Arduino.h>
#include <cstring>

#define OTA_BUF_SIZE 4096

namespace ble_ota {

static bool s_active = false;
static esp_ota_handle_t s_handle = 0;
static const esp_partition_t* s_partition = nullptr;
static uint32_t s_totalSize = 0;
static uint32_t s_written = 0;
static mbedtls_md5_context s_md5Ctx;
static char s_expectedMd5[33] = {0};

static uint8_t* s_buf = nullptr;
static size_t s_bufLen = 0;

bool isActive() { return s_active; }
uint32_t bytesWritten() { return s_written; }

bool begin(uint32_t size, const char* md5Hex) {
  if (s_active) abort();

  s_partition = esp_ota_get_next_update_partition(nullptr);
  if (!s_partition) {
    Serial.println("[BLE-OTA] No OTA partition found");
    return false;
  }

  esp_err_t err = esp_ota_begin(s_partition, size, &s_handle);
  if (err != ESP_OK) {
    Serial.printf("[BLE-OTA] esp_ota_begin failed: %s\n", esp_err_to_name(err));
    return false;
  }

  s_buf = (uint8_t*)heap_caps_malloc(OTA_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_buf) s_buf = (uint8_t*)malloc(OTA_BUF_SIZE);
  if (!s_buf) {
    esp_ota_abort(s_handle);
    Serial.println("[BLE-OTA] Buffer alloc failed");
    return false;
  }
  s_bufLen = 0;

  s_totalSize = size;
  s_written = 0;

  mbedtls_md5_init(&s_md5Ctx);
  mbedtls_md5_starts(&s_md5Ctx);

  if (md5Hex && strlen(md5Hex) == 32) {
    strncpy(s_expectedMd5, md5Hex, 32);
    s_expectedMd5[32] = '\0';
  } else {
    s_expectedMd5[0] = '\0';
  }

  s_active = true;
  Serial.printf("[BLE-OTA] Begin: size=%u, partition=%s\n",
      (unsigned)size, s_partition->label);
  return true;
}

static bool flushBuffer() {
  if (s_bufLen == 0) return true;
  esp_err_t err = esp_ota_write(s_handle, s_buf, s_bufLen);
  if (err != ESP_OK) {
    Serial.printf("[BLE-OTA] Write failed: %s\n", esp_err_to_name(err));
    return false;
  }
  s_written += s_bufLen;
  s_bufLen = 0;
  return true;
}

bool writeChunk(const uint8_t* data, size_t len) {
  if (!s_active || !data || len == 0) return false;

  mbedtls_md5_update(&s_md5Ctx, data, len);

  size_t off = 0;
  while (off < len) {
    size_t space = OTA_BUF_SIZE - s_bufLen;
    size_t chunk = (len - off < space) ? (len - off) : space;
    memcpy(s_buf + s_bufLen, data + off, chunk);
    s_bufLen += chunk;
    off += chunk;

    if (s_bufLen >= OTA_BUF_SIZE) {
      if (!flushBuffer()) {
        abort();
        return false;
      }
    }
  }
  return true;
}

bool end() {
  if (!s_active) return false;

  // Flush remaining buffer
  if (!flushBuffer()) {
    abort();
    return false;
  }

  // Verify MD5
  if (s_expectedMd5[0]) {
    uint8_t digest[16];
    mbedtls_md5_finish(&s_md5Ctx, digest);
    mbedtls_md5_free(&s_md5Ctx);

    char computed[33];
    for (int i = 0; i < 16; i++) snprintf(computed + i * 2, 3, "%02x", digest[i]);
    computed[32] = '\0';

    if (strcasecmp(computed, s_expectedMd5) != 0) {
      Serial.printf("[BLE-OTA] MD5 mismatch: expected=%s computed=%s\n", s_expectedMd5, computed);
      esp_ota_abort(s_handle);
      free(s_buf); s_buf = nullptr;
      s_active = false;
      return false;
    }
    Serial.println("[BLE-OTA] MD5 verified OK");
  } else {
    mbedtls_md5_free(&s_md5Ctx);
  }

  esp_err_t err = esp_ota_end(s_handle);
  if (err != ESP_OK) {
    Serial.printf("[BLE-OTA] esp_ota_end failed: %s\n", esp_err_to_name(err));
    free(s_buf); s_buf = nullptr;
    s_active = false;
    return false;
  }

  err = esp_ota_set_boot_partition(s_partition);
  if (err != ESP_OK) {
    Serial.printf("[BLE-OTA] Set boot partition failed: %s\n", esp_err_to_name(err));
    free(s_buf); s_buf = nullptr;
    s_active = false;
    return false;
  }

  free(s_buf); s_buf = nullptr;
  s_active = false;
  Serial.printf("[BLE-OTA] Success: %u bytes written, rebooting...\n", (unsigned)s_written);
  return true;
}

void abort() {
  if (!s_active) return;
  esp_ota_abort(s_handle);
  mbedtls_md5_free(&s_md5Ctx);
  free(s_buf); s_buf = nullptr;
  s_bufLen = 0;
  s_active = false;
  s_written = 0;
  Serial.println("[BLE-OTA] Aborted");
}

}  // namespace ble_ota
