# Прошивка nRF52840 (FakeTech V5, Heltec Mesh Node T114)

Веб-инструмент в `docs/flasher/` (ESP Web Tools) **не подходит** для nRF52840: он рассчитан на ESP32. Для плат на Nordic используйте **PlatformIO**, **nrfutil** или **UF2/DFU**.

## Какая плата → какой env (`-e`)

- **`faketec_v5`** — Arduino-variant [`firmware/variants/faketec_nrf52840`](../../firmware/variants/faketec_nrf52840).
- **`heltec_t114`** — variant [`firmware/variants/heltec_mesh_t114`](../../firmware/variants/heltec_mesh_t114) (LED на GPIO **35**, не на **15**=TFT BL; I2C по умолчанию 26/27).

Отличие плат — ещё **макросы** и [`firmware/src/faketec/board_pins.h`](../../firmware/src/faketec/board_pins.h). Прошивка **не того** env даёт неверные **I2C** и **SPI (LoRa)** — типично «чёрный экран», «тишина» на эфире.

| Физическое устройство | PlatformIO | I2C OLED (логические GPIO) |
|------------------------|------------|----------------------------|
| FakeTech V5 / NiceNano / nRF52 Pro Micro DIY (как в README FakeTech) | `faketec_v5` | SDA **36**, SCL **11** |
| Heltec Mesh Node **T114** | `heltec_t114` | SDA **26**, SCL **27** |

Подробнее по SPI LoRa и совместимости с Meshtastic — [`firmware/src/faketec/README.md`](../../firmware/src/faketec/README.md).

### Serial (USB CDC) пустой

После прошивки выберите **COM-порт устройства в режиме приложения** (иногда он отличается от порта bootloader). На TinyUSB CDC **первые строки часто теряются**, пока хост не открыл виртуальный COM: в `faketec/main.cpp` перед первым логом ждётся до **15 с** готовности `Serial`, строки дублируются на **аппаратный UART `Serial1`** (`faketec_nrf52840` / `heltec_mesh_t114`: **TX = GPIO 6**, 115200 8N1) — при «пустом» USB подключите внешний USB–UART к TX. Отключить UART1: `-D RIFTLINK_NO_UART1_LOG`. Если порт верный, а строк нет — проверьте драйвер CDC и bootloader (см. ниже).

## Сборка

Из корня репозитория:

```powershell
.\build.ps1 -Faketec
# или
.\build.ps1 -HeltecT114
```

Или из каталога `firmware`:

```bash
pio run -e faketec_v5
pio run -e heltec_t114
```

Артефакты: `.pio/build/<env>/firmware.hex`, `firmware.zip` (пакет для nrfutil). При сборке через `build.ps1` копии лежат в `firmware/out/<env>/`.

## Прошивка через PlatformIO

Подключите плату в режиме загрузчика (для NiceNano/FakeTech: двойной сброс → диск `NICENANO` или последовательный порт bootloader). Укажите COM-порт:

```bash
cd firmware
pio run -t upload -e faketec_v5 --upload-port COM5
```

Из корня репозитория:

```powershell
.\build.ps1 -Faketec -Flash -Port COM5
```

`upload_protocol` в `platformio.ini` для nRF: **nrfutil** (идёт с PlatformIO / Adafruit nRF52).

## DFU ZIP и nrfutil вручную

После `pio run` файл `firmware.zip` совместим с типичным сценарием **Adafruit nRF52** / `adafruit-nrfutil` (перетаскивание на UF2-диск или `nrfutil dfu serial`). Точная команда зависит от версии `nrfutil` на ПК; ориентир — документация [Adafruit nRF52 bootloader](https://learn.adafruit.com/introducing-the-adafruit-nrf52840-feather/update-bootloader).

## Полная перезапись flash

Штатный `pio run -t upload` обновляет приложение при сохранённом bootloader SoftDevice. Полный чип-erase делается **J-Link**, **nrfjprog** или специальными утилитами производителя — это отдельная процедура, не смешивайте её с ESP-потоком «Erase» из веб-флешера.

## Документация по плате

Подробности по пинам, LoRa и BLE: `firmware/src/faketec/README.md`.
