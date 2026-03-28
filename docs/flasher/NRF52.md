# Прошивка nRF52840 (FakeTech V5, Heltec Mesh Node T114)

Веб-инструмент в `docs/flasher/` (ESP Web Tools) **не подходит** для nRF52840: он рассчитан на ESP32. Для плат на Nordic используйте **PlatformIO**, **nrfutil** или **UF2/DFU**.

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
