#!/usr/bin/env python3
"""
Статическая сверка цепочки приложение ↔ узел (BLE JSON):
- события из firmware/src/ble/ble.cpp (doc["evt"] = / ev["evt"] = / qdoc / out)
- tracked expectedEvents из app/lib/ble/riftlink_ble.dart
- команды strcmp(cmd, ...) в ble.cpp vs ble_nrf.cpp (паритет наличия)

Запуск из корня репозитория: python scripts/verify_app_node_chain.py
Код выхода: 0 — ок, 1 — найдены evt из Flutter без эмита в ESP ble.cpp.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read_text(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def extract_ble_cpp_evts(content: str) -> set[str]:
    evts: set[str] = set()
    # doc["evt"] = "name"; ev["evt"] = "name"; qdoc["evt"] = "name"; out["evt"]
    for m in re.finditer(
        r'\[["\']evt["\']\]\s*=\s*["\']([a-zA-Z][a-zA-Z0-9_]*)["\']',
        content,
    ):
        evts.add(m.group(1))
    # raw strings: {\"evt\":\"bleOtaResult\"
    for m in re.finditer(r'["\']evt["\']\s*:\s*["\']([a-zA-Z][a-zA-Z0-9_]*)["\']', content):
        evts.add(m.group(1))
    return evts


def extract_dart_expected_evts(content: str) -> set[str]:
    out: set[str] = set()
    # expectedEvents: const {'node', 'neighbors'} or single {'region'}
    for block in re.finditer(r"expectedEvents:\s*(const\s*)?\{([^}]+)\}", content):
        inner = block.group(2)
        for m in re.finditer(r"['\"]([a-zA-Z][a-zA-Z0-9_]*)['\"]", inner):
            out.add(m.group(1))
    return out


def extract_strcmp_cmds(content: str) -> set[str]:
    cmds: set[str] = set()
    for m in re.finditer(r'strcmp\s*\(\s*cmd\s*,\s*["\']([^"\']+)["\']', content):
        cmds.add(m.group(1))
    return cmds


def main() -> int:
    ble_cpp = read_text(ROOT / "firmware" / "src" / "ble" / "ble.cpp")
    ble_nrf = read_text(ROOT / "firmware" / "src" / "faketec" / "ble_nrf.cpp")
    dart = read_text(ROOT / "app" / "lib" / "ble" / "riftlink_ble.dart")

    fw_evts = extract_ble_cpp_evts(ble_cpp)
    dart_evts = extract_dart_expected_evts(dart)

    # Router обрабатывает groupSecurityError отдельно — не в expectedEvents в dart, но важен.
    # loraScan — ответ приходит как evt (см. ble.cpp out["evt"] = "loraScan")
    missing = sorted(dart_evts - fw_evts)
    if missing:
        print("FAIL: Flutter expectedEvents has evt not emitted in ble.cpp:")
        for e in missing:
            print(f"  - {e}")
        print()
        print("ble.cpp evts:", ", ".join(sorted(fw_evts)))
        return 1

    print("OK: all riftlink_ble.dart expectedEvents evt names exist in ble.cpp.")

    esp_cmds = extract_strcmp_cmds(ble_cpp)
    nrf_cmds = extract_strcmp_cmds(ble_nrf)
    only_esp = sorted(esp_cmds - nrf_cmds)
    only_nrf = sorted(nrf_cmds - esp_cmds)
    print()
    print("strcmp(cmd) parity ble.cpp vs ble_nrf.cpp:")
    print(f"  only_ESP: {len(only_esp)} (WiFi/ESP-only expected)")
    if only_esp[:20]:
        print("    e.g.:", ", ".join(only_esp[:20]))
    print(f"  only_nRF: {len(only_nrf)}")
    if only_nrf[:15]:
        print("    e.g.:", ", ".join(only_nrf[:15]))

    return 0


if __name__ == "__main__":
    sys.exit(main())
