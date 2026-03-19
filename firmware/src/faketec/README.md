# RiftLink для FakeTech V5 Rev B

Прошивка RiftLink для платы **FakeTech V5 Rev B** (NiceNano nRF52840 + HT-RA62/RA-01SH LoRa).

## Сборка

```bash
cd firmware
pio run -e faketec_v5
```

Артефакты: `.pio/build/faketec_v5/firmware.hex`, `firmware.zip`

## Прошивка

1. **DFU-режим:** двойной клик по кнопке RST на NiceNano (в течение 0.5 с)
2. Появится диск `NICENANO` — скопируйте `firmware.zip` на диск (или используйте `nrfutil`)

Или через PlatformIO:
```bash
pio run -t upload -e faketec_v5
```

## Распиновка (board.h)

**ВАЖНО:** Пины заданы для Adafruit Feather nRF52840. FakeTech использует **NiceNano** с другим pinout — проверьте схему вашей платы и при необходимости скорректируйте `board.h`.

| Сигнал | Пин |
|--------|-----|
| LoRa NSS | 25 |
| LoRa RST | 12 |
| LoRa DIO1 | 36 |
| LoRa BUSY | 39 |
| LoRa SCK | 15 |
| LoRa MISO | 16 |
| LoRa MOSI | 2 |
| OLED SDA | 17 |
| OLED SCL | 20 |

## Функции

- **LoRa mesh:** HELLO, MSG, ACK, relay
- **Дисплей:** автоопределение I2C OLED (SSD1306 @ 0x3C)
- **Команды:** через Serial (JSON `{"cmd":"send","text":"..."}`)
- **Совместимость:** тот же протокол, что и Heltec — узлы в одной сети

## Ограничения (v1)

- **Storage:** в RAM — Node ID сбрасывается при отключении питания (TODO: Flash)
- **BLE:** заглушка — команды через Serial
- **Шифрование:** pass-through (без ChaCha20)
- **Board:** используется `adafruit_feather_nrf52840` — пины могут не совпадать с NiceNano
