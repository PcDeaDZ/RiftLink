#!/usr/bin/env python3
"""
Regression guards for NODE_A<->NODE_B and BLE<->APP chain hardening.
These checks validate source-level invariants for previously fixed risks.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _read(rel_path: str) -> str:
    return (ROOT / rel_path).read_text(encoding="utf-8")


def test_ble_notify_uses_active_transport_guard():
    src = _read("firmware/src/ble/ble.cpp")
    assert "if (!pRxChar || !s_connected) return;" not in src
    assert "if (!hasActiveTransport()) return;" in src


def test_ble_notify_mutex_drop_is_tracked():
    src = _read("firmware/src/ble/ble.cpp")
    assert "notifyMutexBusyDrop" in src
    assert "notify_mutex_drop" in src
    assert "kNotifyMutexMaxAttempts" in src


def test_ble_cmd_queue_hardened_for_burst():
    src = _read("firmware/src/ble/ble.cpp")
    assert "kBleCmdQueueDepth = 12" in src
    assert "kBleCmdConsumePerTick = 6" in src
    assert "xQueueSend(s_bleCmdQueue, &item, kBleCmdEnqueueTimeoutTicks)" in src


def test_packet_queue_spill_keeps_hello():
    src = _read("firmware/src/async_tasks.cpp")
    assert "Preserve HELLO in queue" in src
    assert "added = false;" in src


def test_msg_batch_decrypt_failure_continues():
    src = _read("firmware/src/main.cpp")
    assert "hasKeyNow ? \"decrypt_fail_batch\" : \"msg_batch_no_key\");" in src
    assert "continue;" in src
    assert "const uint8_t* encPtr = payload + off;" in src


def test_app_ble_tx_serialization_and_prelisten_capacity():
    src = _read("app/lib/ble/riftlink_ble.dart")
    assert "_maxPreListenBuffer = 256" in src
    assert "_writeBleBytesSerialized(" in src
    assert "cmd=bleOtaChunk" in src
