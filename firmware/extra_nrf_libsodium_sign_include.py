"""pre: добавляет include-пути пакета esphome/libsodium для сборки lib/riftlink_sodium_sign."""
import os

Import("env")


def _add_libsodium_includes():
    project_dir = env["PROJECT_DIR"]
    pioenv = env.get("PIOENV", "")
    if not pioenv:
        return
    root = os.path.join(project_dir, ".pio", "libdeps", pioenv, "libsodium")
    inc = os.path.join(root, "libsodium", "src", "libsodium", "include")
    port_inc = os.path.join(root, "port_include")
    sodium_inc = os.path.join(inc, "sodium")
    for p in (inc, port_inc, sodium_inc):
        if os.path.isdir(p):
            env.Append(CPPPATH=[p])


_add_libsodium_includes()
