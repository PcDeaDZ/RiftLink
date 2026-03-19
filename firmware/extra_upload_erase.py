"""Добавляет target upload_erase: полная очистка флеша + прошивка.
   Используйте при смене типа прошивки (V3 OLED ↔ Paper E-Ink), чтобы NVS и старые данные не мешали.
   Только для serial upload (heltec_v3, heltec_v4, heltec_v3_paper). OTA не поддерживает erase."""
Import("env")
import os

def erase_then_upload(source, target, env):
    if env.get("UPLOAD_PROTOCOL") == "espota":
        print("ERROR: upload_erase только для serial. OTA не поддерживает erase. Используйте -e heltec_v3 или heltec_v3_paper")
        return
    port = env.get("UPLOAD_PORT", "")
    if not port:
        print("ERROR: UPLOAD_PORT not set. Укажите порт: pio run -t upload_erase -e heltec_v3_paper")
        return
    chip = env.get("BOARD_MCU", "esp32s3")
    python = env.get("PYTHONEXE", "python")
    baud = env.get("UPLOAD_SPEED", "921600")
    build_dir = env.subst("$BUILD_DIR")
    firmware = str(source[0].abspath) if source else os.path.join(build_dir, "firmware.bin")
    if not os.path.exists(firmware):
        print("ERROR: firmware.bin не найден:", firmware)
        return

    # esptool v5: erase-flash (вместо deprecated erase_flash)
    env.Execute(env.VerboseAction(
        f'"{python}" -m esptool --chip {chip} --port {port} --baud {baud} erase-flash',
        "Erasing flash..."
    ))
    # Собираем write_flash вручную ($SOURCE зарезервирован, UPLOADCMD не подходит для кастомного target)
    # ESP32-S3: bootloader в 0x0 (не 0x1000 как у классического ESP32)
    bl = os.path.join(build_dir, "bootloader.bin")
    pt = os.path.join(build_dir, "partitions.bin")
    parts = []
    if os.path.exists(bl):
        parts.extend(["0x0", f'"{bl}"'])
    if os.path.exists(pt):
        parts.extend(["0x8000", f'"{pt}"'])
    parts.extend(["0x10000", f'"{firmware}"'])
    cmd = f'"{python}" -m esptool --chip {chip} --port {port} --baud {baud} write_flash {" ".join(parts)}'
    env.Execute(env.VerboseAction(cmd, "Uploading firmware..."))

env.AddCustomTarget(
    name="upload_erase",
    dependencies="$BUILD_DIR/firmware.bin",
    actions=[erase_then_upload]
)
