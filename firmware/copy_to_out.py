"""Post-build: копирует firmware.bin в out/<env>/ и создаёт merged файл с версией в имени."""
import os
import re
import shutil
import subprocess
import sys

Import("env")

def get_version(proj):
    """Читает RIFTLINK_VERSION из src/version.h"""
    vn_path = os.path.join(proj, "src", "version.h")
    if os.path.exists(vn_path):
        with open(vn_path, encoding="utf-8") as f:
            for line in f:
                m = re.search(r'RIFTLINK_VERSION\s+"([^"]+)"', line)
                if m:
                    return m.group(1)
    return "0.0.0"

def copy_to_out(target, source, env):
    proj = env["PROJECT_DIR"]
    env_name = env.get("PIOENV", "unknown")
    build_dir = env.subst("$BUILD_DIR")
    version = get_version(proj)
    # target[0] — SCons Node, путь к firmware.bin
    t = target[0]
    src = t.get_abspath() if hasattr(t, "get_abspath") else str(t)
    if not os.path.exists(src):
        return
    out_dir = os.path.join(proj, "out", env_name)
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, "firmware.bin")
    shutil.copy2(src, dst)
    # Версионированный app bin: heltec_v3_1.0.0.bin
    ver_app = os.path.join(out_dir, f"{env_name}_{version}.bin")
    shutil.copy2(src, ver_app)
    print(f"[out] {env_name}/firmware.bin, {env_name}_{version}.bin")

    # Merged bin: bootloader + partitions + app — heltec_v3_full_1.0.0.bin
    bl = os.path.join(build_dir, "bootloader.bin")
    pt = os.path.join(build_dir, "partitions.bin")
    if os.path.exists(bl) and os.path.exists(pt):
        merged = os.path.join(out_dir, f"{env_name}_full_{version}.bin")
        try:
            subprocess.run([
                sys.executable, "-m", "esptool", "--chip", "esp32s3",
                "merge-bin", "-o", merged, "--flash-mode", "dio",
                "0x0", bl, "0x8000", pt, "0x10000", src
            ], check=True, capture_output=True)
            print(f"[out] {env_name}/{env_name}_full_{version}.bin (merged, flash at 0x0)")
        except subprocess.CalledProcessError as e:
            print(f"[out] merge skipped: {e}")

env.AddPostAction("$BUILD_DIR/firmware.bin", copy_to_out)
