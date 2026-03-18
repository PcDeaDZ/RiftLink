"""Добавляет target upload_erase: полная очистка флеша + прошивка.
   Используйте при смене типа прошивки (V3 OLED ↔ Paper E-Ink), чтобы NVS и старые данные не мешали.
   Только для serial upload (heltec_v3, heltec_v4, heltec_v3_paper). OTA не поддерживает erase."""
Import("env")

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
    env.Execute(env.VerboseAction(
        f'"{python}" -m esptool --chip {chip} --port {port} erase_flash',
        "Erasing flash..."
    ))
    env.Execute(env.VerboseAction("$UPLOADCMD", "Uploading firmware..."))

env.AddCustomTarget(
    name="upload_erase",
    dependencies="$BUILD_DIR/firmware.bin",
    actions=[erase_then_upload]
)
