/**
 * RiftLink — телеметрия
 * Heltec V3/V4 (OLED): GPIO1 = ADC1_CH0 для батареи, GPIO37 = ADC_CTRL (HIGH = вкл. делитель)
 *   Делитель 390k+100k → коэффициент 4.9.  Meshtastic variant.h: BATTERY_PIN=1, ADC_CTRL=37
 * Heltec V3 Paper: GPIO7 = E-Ink BUSY. Батарея на GPIO19/20 (ADC2, divider ~50%).
 */

#include "telemetry.h"
#if defined(ARDUINO_LILYGO_T_LORA_PAGER)
#include "board/bq27220_tpager.h"
#endif
#if defined(ARDUINO_LILYGO_T_BEAM)
#include "board/lilygo_tbeam.h"
#endif
#include "neighbors/neighbors.h"
#include "node/node.h"
#include "radio/radio.h"
#include "async_tasks.h"
#include "log.h"
#include "crypto/crypto.h"
#include "protocol/packet.h"
#include <Arduino.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#if !defined(USE_EINK) && !defined(ARDUINO_LILYGO_T_BEAM)
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#endif

#define BAT_ADC_PIN      1      // GPIO1 (ADC1_CH0) — Heltec V3/V4 OLED
#define BAT_ADC_CTRL     37     // GPIO37: HIGH = включить делитель батареи
#define BAT_DIVIDER      4.9f   // резисторный делитель 390k / 100k

#if defined(USE_EINK)
#define PAPER_ADC_CTRL  19  // GPIO19: LOW = включить делитель батареи
#define PAPER_ADC_IN    20  // GPIO20: ADC2, напряжение после делителя ~50%
#endif

static bool s_inited = false;
static uint32_t s_lastBatteryRawLogMs = 0;
static SemaphoreHandle_t s_adcMutex = nullptr;
static uint16_t s_lastBatteryMv = 0;
static uint32_t s_lastBatteryReadMs = 0;
static constexpr uint32_t BATTERY_CACHE_MS = 900;
// ESP32-S3 occasionally aborts inside adc_oneshot HAL under runtime load.
// Keep legacy Arduino ADC path as default to avoid hard crashes in displayTask.
static constexpr bool kUseEspIdfOneshotAdc = false;
#if !defined(USE_EINK) && !defined(ARDUINO_LILYGO_T_BEAM)
static adc_oneshot_unit_handle_t s_batAdcUnit = nullptr;
static adc_cali_handle_t s_batAdcCali = nullptr;
static bool s_batAdcCalibrated = false;
static bool s_batAdcReady = false;
static constexpr adc_channel_t BAT_ADC_CHANNEL = ADC_CHANNEL_0;  // GPIO1
static uint8_t s_adcConsecutiveReadFails = 0;
static uint8_t s_adcConsecutiveCalFails = 0;
static uint32_t s_lastAdcDiagLogMs = 0;
static constexpr uint8_t ADC_REINIT_FAIL_THRESHOLD = 5;
static constexpr uint8_t ADC_DISABLE_CALI_FAIL_THRESHOLD = 3;
static constexpr uint32_t ADC_DIAG_LOG_INTERVAL_MS = 15000;
#endif

namespace telemetry {

#if !defined(USE_EINK) && !defined(ARDUINO_LILYGO_T_BEAM)
static bool configureBatteryAdcChannel() {
  if (s_batAdcUnit == nullptr) return false;
  adc_oneshot_chan_cfg_t chanCfg = {};
  chanCfg.atten = ADC_ATTEN_DB_12;
  // ESP32-S3 accepts 12-bit/default; default is safest across core/IDF combos.
  chanCfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  return adc_oneshot_config_channel(s_batAdcUnit, BAT_ADC_CHANNEL, &chanCfg) == ESP_OK;
}

static bool canLogAdcDiag(uint32_t nowMs) {
  if (nowMs - s_lastAdcDiagLogMs < ADC_DIAG_LOG_INTERVAL_MS) return false;
  s_lastAdcDiagLogMs = nowMs;
  return true;
}

static void dropBatteryAdcCalibration() {
  if (!s_batAdcCali) {
    s_batAdcCalibrated = false;
    return;
  }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_delete_scheme_curve_fitting(s_batAdcCali);
#elif !defined(CONFIG_IDF_TARGET_ESP32H2)
  adc_cali_delete_scheme_line_fitting(s_batAdcCali);
#endif
  s_batAdcCali = nullptr;
  s_batAdcCalibrated = false;
}

static void releaseBatteryAdc() {
  dropBatteryAdcCalibration();
  if (s_batAdcUnit) {
    adc_oneshot_del_unit(s_batAdcUnit);
    s_batAdcUnit = nullptr;
  }
  s_batAdcReady = false;
}

static bool setupBatteryAdc() {
  releaseBatteryAdc();

  pinMode(BAT_ADC_CTRL, OUTPUT);
  digitalWrite(BAT_ADC_CTRL, HIGH);   // включить делитель батареи (active HIGH)

  adc_oneshot_unit_init_cfg_t unitCfg = {};
  unitCfg.unit_id = ADC_UNIT_1;
  unitCfg.ulp_mode = ADC_ULP_MODE_DISABLE;
  if (adc_oneshot_new_unit(&unitCfg, &s_batAdcUnit) != ESP_OK) {
    return false;
  }

  adc_oneshot_chan_cfg_t chanCfg = {};
  chanCfg.atten = ADC_ATTEN_DB_12;
  chanCfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  if (adc_oneshot_config_channel(s_batAdcUnit, BAT_ADC_CHANNEL, &chanCfg) != ESP_OK) {
    releaseBatteryAdc();
    return false;
  }
  s_batAdcReady = true;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t calCfg = {};
  calCfg.unit_id = ADC_UNIT_1;
  calCfg.atten = ADC_ATTEN_DB_12;
  calCfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  s_batAdcCalibrated = (adc_cali_create_scheme_curve_fitting(&calCfg, &s_batAdcCali) == ESP_OK);
#elif !defined(CONFIG_IDF_TARGET_ESP32H2)
  adc_cali_line_fitting_config_t calCfg = {};
  calCfg.unit_id = ADC_UNIT_1;
  calCfg.atten = ADC_ATTEN_DB_12;
  calCfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  s_batAdcCalibrated = (adc_cali_create_scheme_line_fitting(&calCfg, &s_batAdcCali) == ESP_OK);
#endif
  return true;
}

static bool reinitBatteryAdcIfNeeded(uint32_t nowMs, const char* reason) {
  if (s_adcConsecutiveReadFails < ADC_REINIT_FAIL_THRESHOLD) return false;
  bool ok = setupBatteryAdc();
  s_adcConsecutiveReadFails = 0;
  s_adcConsecutiveCalFails = 0;
  if (canLogAdcDiag(nowMs)) {
    RIFTLINK_DIAG("TELEM", "event=ADC_REINIT reason=%s ok=%u", reason ? reason : "unknown", (unsigned)ok);
  }
  return ok;
}
#endif

void init() {
  if (!s_adcMutex) s_adcMutex = xSemaphoreCreateMutex();
#if defined(ARDUINO_LILYGO_T_LORA_PAGER)
  (void)bq27220_tpager::probe();
#elif defined(ARDUINO_LILYGO_T_BEAM)
  // T-Beam: батарея через AXP2101 ADC, не GPIO ADC — ничего не нужно
#elif defined(USE_EINK)
  analogReadResolution(12);
  pinMode(PAPER_ADC_CTRL, OUTPUT);
  digitalWrite(PAPER_ADC_CTRL, LOW);
  analogSetPinAttenuation(PAPER_ADC_IN, ADC_11db);
#else
  pinMode(BAT_ADC_CTRL, OUTPUT);
  digitalWrite(BAT_ADC_CTRL, HIGH);
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
  if (kUseEspIdfOneshotAdc && !setupBatteryAdc()) {
    uint32_t nowMs = millis();
    if (canLogAdcDiag(nowMs)) {
      RIFTLINK_DIAG("TELEM", "event=ADC_INIT_FAIL");
    }
  }
#endif
  s_inited = true;
}

uint16_t readBatteryMv() {
#if defined(ARDUINO_LILYGO_T_LORA_PAGER)
  if (!s_inited) return 0;
  uint32_t nowMs = millis();
  if (s_lastBatteryMv > 0 && (nowMs - s_lastBatteryReadMs) < BATTERY_CACHE_MS) {
    return s_lastBatteryMv;
  }
  if (s_adcMutex && xSemaphoreTake(s_adcMutex, pdMS_TO_TICKS(120)) != pdTRUE) {
    return s_lastBatteryMv;
  }
  uint16_t batMv = bq27220_tpager::readVoltageMv();
  s_lastBatteryMv = batMv;
  s_lastBatteryReadMs = nowMs;
  if (s_adcMutex) xSemaphoreGive(s_adcMutex);
  return batMv;
#elif defined(ARDUINO_LILYGO_T_BEAM)
  if (!s_inited) return 0;
  uint32_t nowMs = millis();
  if (s_lastBatteryMv > 0 && (nowMs - s_lastBatteryReadMs) < BATTERY_CACHE_MS) {
    return s_lastBatteryMv;
  }
  uint16_t batMv = lilygoTbeamReadBatteryMv();
  s_lastBatteryMv = batMv;
  s_lastBatteryReadMs = nowMs;
  return batMv;
#else
  if (!s_inited) return 0;
  uint32_t nowMs = millis();
  if (s_lastBatteryMv > 0 && (nowMs - s_lastBatteryReadMs) < BATTERY_CACHE_MS) {
    return s_lastBatteryMv;
  }
  if (s_adcMutex && xSemaphoreTake(s_adcMutex, pdMS_TO_TICKS(120)) != pdTRUE) {
    return s_lastBatteryMv;
  }
  uint16_t batMv = 0;

#if defined(USE_EINK)
  // Paper: GPIO19=LOW (включить), GPIO20=ADC2. Делитель ~50% → умножить на 2.
  uint32_t sum = 0;
  int n = 0;
  for (int i = 0; i < 8; i++) {
    int mv = analogReadMilliVolts(PAPER_ADC_IN);
    if (mv > 0 && mv < 2500) {  // 0–2.5V после делителя (0–5V батарея)
      sum += mv;
      n++;
    }
    delay(2);
  }
  if (n >= 4) {
    uint32_t avg = sum / n;
    batMv = (uint16_t)(avg * 2);  // делитель ~50%
  }
#else
  digitalWrite(BAT_ADC_CTRL, HIGH);
  delay(10);  // стабилизация делителя

  uint32_t sum = 0;
  int samples = 0;
  uint32_t nowDiagMs = nowMs;
  for (int i = 0; i < 8; i++) {
    int mv = 0;
    if (!kUseEspIdfOneshotAdc) {
      mv = analogReadMilliVolts(BAT_ADC_PIN);
    } else if (s_batAdcReady) {
      if (s_batAdcUnit != nullptr) {
        if (!configureBatteryAdcChannel()) {
          s_adcConsecutiveReadFails++;
          if (canLogAdcDiag(nowDiagMs)) {
            RIFTLINK_DIAG("TELEM", "event=ADC_CHAN_CFG_FAIL streak=%u", (unsigned)s_adcConsecutiveReadFails);
          }
          reinitBatteryAdcIfNeeded(nowDiagMs, "chan_cfg_fail");
          delay(2);
          continue;
        }
        int raw = 0;
        if (adc_oneshot_read(s_batAdcUnit, BAT_ADC_CHANNEL, &raw) == ESP_OK && raw > 0) {
          s_adcConsecutiveReadFails = 0;
          if (s_batAdcCalibrated && s_batAdcCali != nullptr) {
            if (adc_cali_raw_to_voltage(s_batAdcCali, raw, &mv) == ESP_OK && mv > 0) {
              s_adcConsecutiveCalFails = 0;
            } else {
              s_adcConsecutiveCalFails++;
              mv = (raw * 3300) / 4095;
              if (canLogAdcDiag(nowDiagMs)) {
                RIFTLINK_DIAG("TELEM", "event=ADC_CAL_FAIL fallback=raw streak=%u", (unsigned)s_adcConsecutiveCalFails);
              }
              if (s_adcConsecutiveCalFails >= ADC_DISABLE_CALI_FAIL_THRESHOLD) {
                dropBatteryAdcCalibration();
                if (canLogAdcDiag(nowDiagMs)) {
                  RIFTLINK_DIAG("TELEM", "event=ADC_CALI_DISABLED reason=fail_streak");
                }
              }
            }
          } else {
            mv = (raw * 3300) / 4095;
          }
        } else {
          s_adcConsecutiveReadFails++;
          if (canLogAdcDiag(nowDiagMs)) {
            RIFTLINK_DIAG("TELEM", "event=ADC_RAW_FAIL streak=%u", (unsigned)s_adcConsecutiveReadFails);
          }
          reinitBatteryAdcIfNeeded(nowDiagMs, "raw_fail_streak");
        }
      }
    }
    if (mv > 0) {
      sum += (uint32_t)mv;
      samples++;
    }
    delay(2);
  }
  uint32_t avgMv = samples > 0 ? (sum / (uint32_t)samples) : 0;
  if (avgMv >= 50) {
    batMv = (uint16_t)(avgMv * BAT_DIVIDER);
  }
#if defined(DEBUG_BATTERY_RAW_LOG)
  uint32_t now = nowMs;
  if (now - s_lastBatteryRawLogMs >= 30000) {
    s_lastBatteryRawLogMs = now;
    Serial.printf("[bat] GPIO%d raw_avg=%lumV bat=%umV\n", BAT_ADC_PIN, avgMv, batMv);
  }
#endif
#endif
  s_lastBatteryMv = batMv;
  s_lastBatteryReadMs = nowMs;
  if (s_adcMutex) xSemaphoreGive(s_adcMutex);
  return batMv;
#endif
}

bool isCharging() {
#if defined(ARDUINO_LILYGO_T_LORA_PAGER)
  return bq27220_tpager::isCharging();
#elif defined(ARDUINO_LILYGO_T_BEAM)
  return lilygoTbeamIsCharging();
#else
  uint16_t mv = readBatteryMv();
  return mv > 4200;
#endif
}

int batteryPercent() {
#if defined(ARDUINO_LILYGO_T_LORA_PAGER)
  if (!s_inited) return -1;
  int soc = bq27220_tpager::readRelativeSocPercent();
  if (soc >= 0) return soc;
  uint16_t mv = readBatteryMv();
  if (mv < 2500) return -1;
  if (mv >= 4200) return 100;
  if (mv <= 3000) return 0;
  return (int)((mv - 3000) / 12);
#else
  uint16_t mv = readBatteryMv();
  if (mv < 2500) return -1;
  if (mv >= 4200) return 100;
  if (mv <= 3000) return 0;
  return (int)((mv - 3000) / 12);
#endif
}

void send() {
  uint16_t batMv = readBatteryMv();
  uint16_t heapKb = (uint16_t)(ESP.getFreeHeap() / 1024);

  uint8_t plain[TELEM_PAYLOAD_LEN];
  memcpy(plain, &batMv, 2);
  memcpy(plain + 2, &heapKb, 2);

  uint8_t encBuf[32];
  size_t encLen = sizeof(encBuf);
  if (!crypto::encrypt(plain, TELEM_PAYLOAD_LEN, encBuf, &encLen)) return;

  uint8_t pkt[protocol::PAYLOAD_OFFSET + 32];
  size_t len = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_TELEMETRY,
      encBuf, encLen, true, false, false);
  if (len > 0) {
    uint8_t txSf = neighbors::rssiToSf(neighbors::getMinRssi());
    char reasonBuf[40];
    if (!queueTxPacket(pkt, len, txSf, false, TxRequestClass::data, reasonBuf, sizeof(reasonBuf))) {
      queueDeferredSend(pkt, len, txSf, 140, true);
      RIFTLINK_DIAG("TELEM", "event=TELEM_TX_DEFER cause=%s", reasonBuf[0] ? reasonBuf : "?");
    }
  }
}

}  // namespace telemetry
