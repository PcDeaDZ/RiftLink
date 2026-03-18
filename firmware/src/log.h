/**
 * RiftLink — лог. Только ошибки, обмен ключами, регистрация соседей.
 */

#pragma once

#define RIFTLINK_LOG_ERR(...) Serial.printf(__VA_ARGS__)
#define RIFTLINK_LOG_EVENT(...) Serial.printf(__VA_ARGS__)  // ключ, сосед
