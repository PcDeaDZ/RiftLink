#!/usr/bin/env python3
"""
RiftLink Serial API — тест команд
Использование: python serial_test.py COM3
Требуется: pyserial (pip install pyserial)
"""

import sys
import time
import serial

BAUD = 115200
TIMEOUT = 0.5


def main():
    if len(sys.argv) < 2:
        print("Usage: python serial_test.py <port>")
        print("Example: python serial_test.py COM3")
        sys.exit(1)

    port = sys.argv[1]
    try:
        ser = serial.Serial(port, BAUD, timeout=TIMEOUT)
    except serial.SerialException as e:
        print(f"Error: {e}")
        sys.exit(1)

    print(f"Connected to {port} at {BAUD} baud")
    print("Reading for 2 sec...")
    time.sleep(2)
    while ser.in_waiting:
        print(ser.readline().decode("utf-8", errors="replace").strip())

    # Test: nickname
    print("\n--- Test: nickname ---")
    ser.write(b"nickname Alice\n")
    time.sleep(0.3)
    while ser.in_waiting:
        print(ser.readline().decode("utf-8", errors="replace").strip())

    # Test: region
    print("\n--- Test: region EU ---")
    ser.write(b"region EU\n")
    time.sleep(0.5)
    while ser.in_waiting:
        print(ser.readline().decode("utf-8", errors="replace").strip())

    # Test: ping (use first 8 chars of a node ID if you have one)
    print("\n--- Test: ping (broadcast) ---")
    print("Note: ping <hex8> sends to specific node. Use device's Node ID.")
    # ser.write(b"ping A1B2C3D4\n")  # Uncomment and set target hex8
    # time.sleep(1)
    # while ser.in_waiting:
    #     print(ser.readline().decode("utf-8", errors="replace").strip())

    # Test: send broadcast
    print("\n--- Test: send broadcast ---")
    ser.write(b"send test from serial_test.py\n")
    time.sleep(0.5)
    while ser.in_waiting:
        print(ser.readline().decode("utf-8", errors="replace").strip())

    print("\nDone.")
    ser.close()


if __name__ == "__main__":
    main()
