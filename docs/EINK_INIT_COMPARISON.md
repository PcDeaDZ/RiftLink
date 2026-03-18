# Сравнение последовательности инициализации E-Ink

## Наша последовательность (display_paper.cpp + main.cpp)

```
main::setup():
  1. VEXT LOW (питание)
  2. LED blink 3× (600 ms)
  3. Serial.begin, delay(500)
  4. nvs_flash_init, locale::init
  5. displayInit()

displayInit():
  6. VEXT LOW, delay(100)  ← повторно
  7. DC OUTPUT, BUSY INPUT
  8. hspi.begin(SCLK=3, -1, MOSI=2, CS=4)
  9. RST: LOW 20ms, HIGH 20ms
  10. readChipId() — bit-bang SPI (cmd 0x2F)
  11. selectSPI(hspi, 6MHz)
  12. disp->init(0, true, 20, false, hspi, ...)
      → GxEPD2_EPD::init():
         - _reset() — RST LOW 20ms, HIGH 20ms (ещё раз!)
         - _pSPIx->begin() — БЕЗ АРГУМЕНТОВ! Сбрасывает пины HSPI на дефолт
  13. hspi.begin(3, -1, 2, 4) — восстанавливаем пины
  14. fillScreen, display(false)
```

## Потенциальные проблемы

### 1. SPI begin() без аргументов
`GxEPD2_EPD::init()` вызывает `_pSPIx->begin()` без аргументов. Мы восстанавливаем пины вызовом `hspi.begin(3, -1, 2, 4)` сразу после init, до fillScreen/display. SPI к дисплею идёт только при display() — к тому моменту пины уже правильные. Предотвратить вызов begin() в init нельзя (библиотека).

### 2. readChipId перед init
Bit-bang SPI идёт до создания дисплея. После readChipId MOSI возвращается в OUTPUT. Порядок в целом допустим.

### 3. Двойной RST
RST делается дважды: вручную (LOW 20ms, HIGH 20ms) и в `_reset()`. Лишний, но не должен ломать инициализацию.

---

## Различия B73 vs E0213A367 (контроллеры)

| Регистр | Наш B73 (SSD1675B) | Meshtastic E0213A367 (SSD1682) |
|---------|--------------------|---------------------------------|
| **0x12** | —                    | SWRESET + delay(20) |
| **0x74** | 0x54                 | — |
| **0x7E** | 0x3B                 | — |
| **0x01** | 0xF9, 0x00, 0x00     | 0xF9, 0x00 |
| **0x3C** | 0x03                 | 0x01 |
| **0x18** | —                    | 0x80 |
| **0x37** | —                    | 0x40, 0x80, 0x03, 0x0E |
| **0x2C** | 0x50                 | — (в Part) |
| **0x03, 0x04, 0x3A, 0x3B** | есть   | — |

**Вывод:** B73 (SSD1675B) и E0213A367 (SSD1682) — разные контроллеры. Init для B73 не подходит для панели E0213A367-BW (V1.2).

---

## Рекомендации

### Для Wireless Paper V1.2 (E0213A367-BW)

1. **Использовать GxEPD2 от Meshtastic** с драйвером `GxEPD2_213_E0213A367` — единственный рабочий вариант. Панель V1.2 использует SSD1682, наш B73 рассчитан на SSD1675B.

2. **В platformio.ini** заменить зависимость:
   ```ini
   ; вместо zinggjm/GxEPD2
   lib_deps =
     https://github.com/meshtastic/GxEPD2/archive/c7eb4c3c167cf396ef4f541cc5d4c6aa42f3c46b.zip
   ```

3. **В display_paper.cpp** использовать `GxEPD2_213_E0213A367` вместо `GxEPD2_213_B73` для V1.2 (или добавить env с фиксированным драйвером).

### Что не требуется

- Пины SPI — уже восстанавливаются после init.
- Патч GxEPD2_EPD — не нужен, порядок вызовов корректен.
