<p align="center">
  <img src="https://img.shields.io/badge/RiftLink-Recovery_Guide-42A5F5?style=for-the-badge&logo=radio&logoColor=white" alt="RiftLink" />
</p>

# 🔧 Восстановление Heltec WiFi LoRa 32 (V3/V4)

> Инструкция при «не включается» после прошивки

<p align="center">
  <img src="https://img.shields.io/badge/Heltec-V3%20%7C%20V4-00B0FF?style=flat-square&logo=lorawan" alt="Heltec" />
  <img src="https://img.shields.io/badge/ESP32--S3-Bootloader-E7352C?style=flat-square&logo=espressif" alt="ESP32" />
  <img src="https://img.shields.io/badge/esptool-Erase%20%26%20Flash-00979D?style=flat-square&logo=platformio" alt="esptool" />
</p>

---

## ⚠️ Устройство не включается после прошивки

### 1. 🔍 Определите плату

- **V3**: ESP32-S3FN8, 8MB встроенная flash, CP2102 (USB-UART)
- **V4**: ESP32-S3R2, 16MB внешняя flash, native USB (без CP2102)

### 2. 📥 Режим загрузчика (bootloader)

**V4 (native USB):** для прошивки нужно войти в bootloader:
1. Отключите питание (USB и батарея)
2. Зажмите кнопку **PRG** (Boot)
3. Подключите USB, не отпуская PRG
4. Отпустите PRG — устройство появится как USB JTAG/Serial
5. Сразу выполните прошивку

**V3:** обычно входит в bootloader автоматически при подключении USB.

### 3. 🧹 Полная очистка и перепрошивка

```powershell
# Полная очистка flash + прошивка V3
.\build.ps1 -Flash -Erase

# Для V4
.\build.ps1 -V4 -Flash -Erase
```

### 4. ⌨️ Ручная очистка через esptool

```powershell
cd firmware

# Узнайте порт: COM3, COM4, /dev/ttyUSB0 и т.д.
# Для V4: зажмите PRG, подключите USB, отпустите PRG

pio run -e heltec_v3 -t erase
# или
pio run -e heltec_v4 -t erase
```

### 5. 🔄 Если прошили не ту прошивку

| Плата | Прошили | Решение |
|-------|---------|---------|
| V3 | V4 (16MB) | `-Flash -Erase` без -V4 — прошить V3 |
| V4 | V3 | Работает (V4 совместим) |
| V4 | V4, не грузится | `-V4 -Flash -Erase` — полная очистка и перепрошивка |

### 6. 🔌 V4: USB не определяется

- Используйте USB-C кабель с данными (не только зарядка)
- Попробуйте другой порт USB
- Держите PRG при подключении — может потребоваться несколько попыток
- Перезагрузите ПК при проблемах с драйвером
