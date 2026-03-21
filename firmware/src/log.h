/**
 * RiftLink — лог. Только ошибки, обмен ключами, регистрация соседей.
 */

#pragma once

#define RIFTLINK_LOG_ERR(...) Serial.printf(__VA_ARGS__)
#define RIFTLINK_LOG_EVENT(...) Serial.printf(__VA_ARGS__)  // ключ, сосед

// Unified diagnostics stream for serial captures.
#define RIFTLINK_DIAG_ON 1

#if RIFTLINK_DIAG_ON
#define RIFTLINK_DIAG(scope, fmt, ...) \
  Serial.printf("[DIAG][%s] t=%lu " fmt "\n", scope, (unsigned long)millis(), ##__VA_ARGS__)
#else
#define RIFTLINK_DIAG(scope, fmt, ...) do { } while (0)
#endif
