/**
 * RiftLink — единый лог для захвата Serial (Heltec V3/V4/Paper и др.).
 * Формат: [DIAG][SCOPE] t=<ms> ... — удобно grep/filters как на «большой» прошивке.
 * RIFTLINK_LOG_* — короткие printf; на nRF52 Serial.printf есть в Print (Adafruit core).
 *
 * USB CDC (особенно Windows): если хост не успевает читать COM, write/printf может
 * блокировать надолго — «зависает» loop и перестают идти логи. Перед записью проверяем
 * Serial.availableForWrite(); при нехватке места строка пропускается (см. RIFTLINK_DIAG_USB_THROTTLE).
 * Важно: порог должен быть МЕНЬШЕ размера TX-буфера CDC (на nRF52 часто 256 B). 384 — ошибка: логи не печатались вообще.
 * По умолчанию throttle ВЫКЛ: иначе на части сборок/драйверов availableForWrite() ведёт себя непредсказуемо и Serial «молчит».
 * Включить: -DRIFTLINK_DIAG_USB_THROTTLE=1 (и при необходимости -DRIFTLINK_DIAG_USB_MIN_FREE=96).
 */

#pragma once

#ifndef RIFTLINK_DIAG_USB_THROTTLE
#define RIFTLINK_DIAG_USB_THROTTLE 0
#endif
/** Минимум свободных байт перед printf; типичная строка DIAG < 200 B; буфер CDC часто 256 B — не ставить > ~200. */
#ifndef RIFTLINK_DIAG_USB_MIN_FREE
#define RIFTLINK_DIAG_USB_MIN_FREE 96
#endif

#if RIFTLINK_DIAG_USB_THROTTLE
#define RIFTLINK_SERIAL_TX_HAS_SPACE() \
  ([]() -> bool { \
    int __n = Serial.availableForWrite(); \
    return __n < 0 || __n >= (int)RIFTLINK_DIAG_USB_MIN_FREE; \
  }())
#define RIFTLINK_LOG_ERR(...) \
  do { \
    if (RIFTLINK_SERIAL_TX_HAS_SPACE()) Serial.printf(__VA_ARGS__); \
  } while (0)
#define RIFTLINK_LOG_EVENT(...) \
  do { \
    if (RIFTLINK_SERIAL_TX_HAS_SPACE()) Serial.printf(__VA_ARGS__); \
  } while (0)
#else
#define RIFTLINK_SERIAL_TX_HAS_SPACE() (true)
#define RIFTLINK_LOG_ERR(...) Serial.printf(__VA_ARGS__)
#define RIFTLINK_LOG_EVENT(...) Serial.printf(__VA_ARGS__)
#endif

// Unified diagnostics stream for serial captures.
#ifndef RIFTLINK_DIAG_ON
#define RIFTLINK_DIAG_ON 1
#endif

#if RIFTLINK_DIAG_ON
#if RIFTLINK_DIAG_USB_THROTTLE
#define RIFTLINK_DIAG(scope, fmt, ...) \
  do { \
    if (RIFTLINK_SERIAL_TX_HAS_SPACE()) \
      Serial.printf("[DIAG][%s] t=%lu " fmt "\n", scope, (unsigned long)millis(), ##__VA_ARGS__); \
  } while (0)
#else
#define RIFTLINK_DIAG(scope, fmt, ...) \
  Serial.printf("[DIAG][%s] t=%lu " fmt "\n", scope, (unsigned long)millis(), ##__VA_ARGS__)
#endif
#else
#define RIFTLINK_DIAG(scope, fmt, ...) do { } while (0)
#endif
