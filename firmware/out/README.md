# Прошивки по устройствам

После сборки в `out/<env>/` создаются:
- `firmware.bin` — app partition
- `<env>_<version>.bin` — app с версией
- `<env>_full_<version>.bin` — merged (bootloader+partitions+app) для прошивки в 0x0

| Папка | Устройство |
|-------|------------|
| `heltec_v3/` | Heltec WiFi LoRa 32 V3 (OLED) |
| `heltec_v3_paper/` | Heltec Wireless Paper (E-Ink, автоопределение BN/B73) |
| `heltec_v4/` | Heltec WiFi LoRa 32 V4 |
| `heltec_v4_safe/` | Heltec V4 (безопасный конфиг) |

**Сборка всех трёх:** `pio run -e heltec_v3 -e heltec_v4 -e heltec_v3_paper`

**Прошивка Paper:**
```bash
cd firmware && pio run -t upload -e heltec_v3_paper
```

**Только .bin (esptool):** используй `heltec_v3_paper_full_1.0.0.bin` из `out/heltec_v3_paper/`:
```bash
esptool --chip esp32s3 --port COM3 erase_flash
esptool --chip esp32s3 --port COM3 --baud 921600 write_flash 0x0 out/heltec_v3_paper/heltec_v3_paper_full_1.0.0.bin
```

---

## Очистка флеша + прошивка (V3 ↔ Paper)

**Важно:** при обычном `pio run -t upload` флеш не стирается — пишется только app-раздел. NVS и старые данные остаются. При смене типа прошивки (V3 OLED ↔ Paper E-Ink) это может вызывать странное поведение: например, прошивка V3 на Paper может «включить» дисплей (разные пины VEXT: 36 vs 45).

**Полная очистка перед прошивкой:**
```bash
# Вариант 1: target upload_erase (erase + upload одной командой)
pio run -t upload_erase -e heltec_v3_paper

# Вариант 2: вручную
pio run -t erase -e heltec_v3_paper
pio run -t upload -e heltec_v3_paper
```

Используйте `upload_erase` при первой прошивке или при переключении между V3 и Paper.
