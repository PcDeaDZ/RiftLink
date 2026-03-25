"""Post-build: копирует firmware.bin в out/<env>/ и создаёт merged файл в стабильном имени."""
import os
import shutil
import subprocess
import sys

Import("env")

def copy_to_out(target, source, env):
    proj = env["PROJECT_DIR"]
    env_name = env.get("PIOENV", "unknown")
    build_dir = env.subst("$BUILD_DIR")
    # target[0] — SCons Node, путь к firmware.bin
    t = target[0]
    src = t.get_abspath() if hasattr(t, "get_abspath") else str(t)
    if not os.path.exists(src):
        return
    out_dir = os.path.join(proj, "out", env_name)
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, "firmware.bin")
    shutil.copy2(src, dst)
    # Стабильный app bin: heltec_v3.bin
    app_bin = os.path.join(out_dir, f"{env_name}.bin")
    shutil.copy2(src, app_bin)
    print(f"[out] {env_name}/firmware.bin, {env_name}.bin")

    # Merged bin: bootloader + partitions + app — heltec_v3_full.bin
    bl = os.path.join(build_dir, "bootloader.bin")
    pt = os.path.join(build_dir, "partitions.bin")
    if os.path.exists(bl) and os.path.exists(pt):
        merged = os.path.join(out_dir, f"{env_name}_full.bin")
        # ESP32 (классика): bootloader @ 0x1000; ESP32-S3: @ 0x0 — иначе merge-bin для T-Beam и т.п. ломается.
        chip = "esp32s3"
        bl_off, pt_off, app_off = "0x0", "0x8000", "0x10000"
        if env_name.startswith("lilygo_t_beam"):
            chip = "esp32"
            bl_off = "0x1000"
        try:
            subprocess.run([
                sys.executable, "-m", "esptool", "--chip", chip,
                "merge-bin", "-o", merged, "--flash-mode", "dio",
                bl_off, bl, pt_off, pt, app_off, src
            ], check=True, capture_output=True)
            print(f"[out] {env_name}/{env_name}_full.bin (merged, chip={chip})")
        except subprocess.CalledProcessError as e:
            print(f"[out] merge skipped: {e}")

env.AddPostAction("$BUILD_DIR/firmware.bin", copy_to_out)
