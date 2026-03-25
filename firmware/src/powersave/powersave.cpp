/**
 * Power Save — light sleep с пробуждением по DIO1 (LoRa) и таймеру
 */

#include "powersave.h"
#include "radio/radio.h"
#include "ble/ble.h"
#include "radio_mode/radio_mode.h"
#include "ui/display.h"
#include "locale/locale.h"
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>
#include <Arduino.h>

#define NVS_NAMESPACE "riftlink"
#define NVS_KEY_PSAVE "psave"

static bool s_enabled = true;
static volatile bool s_shutdownRequested = false;

namespace powersave {

void init() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
    uint8_t v = 0;  // default OFF — Serial не обрывается при отладке
    esp_err_t err = nvs_get_u8(h, NVS_KEY_PSAVE, &v);
    nvs_close(h);
    s_enabled = (err == ESP_OK && v != 0);
  }
}

bool isEnabled() { return s_enabled; }

void setEnabled(bool on) {
  s_enabled = on;
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, NVS_KEY_PSAVE, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
  }
}

bool canSleep() {
#if defined(USE_EINK)
  // E-Ink: light sleep может нарушать SPI/дисплей после пробуждения — отключаем.
  return false;
#else
  return s_enabled &&
      !ble::isConnected() &&
      !radio_mode::isSwitching() &&
      radio_mode::current() == radio_mode::BLE;
#endif
}

void lightSleepWake() {
  gpio_wakeup_enable((gpio_num_t)DIO1_GPIO, GPIO_INTR_HIGH_LEVEL);
  gpio_wakeup_enable((gpio_num_t)0, GPIO_INTR_LOW_LEVEL);  // USER_SW — active low
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(SLEEP_TIMEOUT_US);
  esp_light_sleep_start();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
}

void deepSleep() {
  displayClear();
  displaySetTextSize(1);
  displayText(20, 24, locale::getForDisplay("shutting_down"));
  displayShow();
  delay(1500);
  displaySleep();
  ble::deinit();
  // GPIO0 (USER_SW, active LOW) = wake source
  esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_0, ESP_EXT1_WAKEUP_ALL_LOW);
  esp_deep_sleep_start();
}

void requestShutdown() { s_shutdownRequested = true; }
bool isShutdownRequested() { return s_shutdownRequested; }

}  // namespace powersave
