/**
 * FreeRTOS: ESP-IDF использует префикс freertos/; Adafruit nRF52 — стандартные имена.
 */
#pragma once

#if defined(RIFTLINK_NRF52)
#include <FreeRTOS.h>
#include <semphr.h>
#include <queue.h>
#include <task.h>
#else
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#endif
