/**
 * T114: выключение в System OFF (как Meshtastic deep sleep / power off на nRF).
 */

#include "t114_power.h"

#if defined(RIFTLINK_BOARD_HELTEC_T114)

#include "board_pins.h"
#include "display_nrf.h"
#include "ble/ble.h"
#include "gps/gps.h"
#include "radio/radio.h"

#include <Arduino.h>
#include <nrf.h>
#include <nrf_gpio.h>

extern "C" {
uint32_t sd_power_system_off(void);
}

void t114_power_enter_system_off() {
  display_nrf::t114_set_backlight_power(false);
  pinMode(TFT_VTFT_CTRL, OUTPUT);
  digitalWrite(TFT_VTFT_CTRL, TFT_VTFT_PWR_OFF);

  if (gps::isPresent()) gps::setEnabled(false);

  if (radio::isReady() && radio::takeMutex(pdMS_TO_TICKS(2000))) {
    radio::standbyChipUnderMutex();
    radio::releaseMutex();
  }

  ble::deinit();

  /* Пробуждение от нажатия кнопки (активный LOW): sense LOW на P1.10 — см. variant T114. */
  nrf_gpio_cfg_sense_input((uint32_t)T114_BUTTON_PIN, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);

  (void)sd_power_system_off();
  NRF_POWER->SYSTEMOFF = 1;
  __DSB();
  for (;;) {
    __WFE();
  }
}

#endif
