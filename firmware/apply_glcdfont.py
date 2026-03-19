"""Pre-build: apply patches (glcdfont, GxEPD2 setBusyCallback)."""
import os
import shutil

Import("env")

def apply_patches(source, target, env):
    proj = env["PROJECT_DIR"]
    pioenv = env.get("PIOENV", "")
    libdeps_base = os.path.join(proj, ".pio", "libdeps")
    if not os.path.exists(libdeps_base):
        print("[patches] WARNING: .pio/libdeps not found - run 'pio run' first to install deps")
        return

    # Патчим только текущий env — не трогаем устаревшие (v4_safe, paper_v10 и т.д.)
    env_dirs = [pioenv] if pioenv else os.listdir(libdeps_base)

    # 1. Adafruit GFX glcdfont.c
    src = os.path.join(proj, "patches", "glcdfont.c")
    if os.path.exists(src):
        patched = 0
        for env_dir in env_dirs:
            gfx_path = os.path.join(libdeps_base, env_dir, "Adafruit GFX Library")
            dst = os.path.join(gfx_path, "glcdfont.c")
            if os.path.exists(gfx_path):
                shutil.copy2(src, dst)
                patched += 1
                print(f"[patches] glcdfont: {env_dir}/Adafruit GFX Library")
        if patched == 0 and pioenv:
            print("[patches] WARNING: No Adafruit GFX Library found")
    else:
        print("[patches] WARNING: patches/glcdfont.c not found")

    # 2. GxEPD2 setBusyCallback (Paper builds only)
    for name in ("GxEPD2_EPD.h", "GxEPD2_EPD.cpp"):
        src = os.path.join(proj, "patches", name)
        if not os.path.exists(src):
            continue
        for env_dir in env_dirs:
            gx_path = os.path.join(libdeps_base, env_dir, "GxEPD2", "src")
            dst = os.path.join(gx_path, name)
            if os.path.exists(gx_path):
                shutil.copy2(src, dst)
                print(f"[patches] GxEPD2 {name}: {env_dir}")

# Run before build AND at env init (for first-time setup)
def on_env(env):
    apply_patches(None, None, env)
env.AddPreAction("build", apply_patches)
on_env(env)
