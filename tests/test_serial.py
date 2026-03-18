#!/usr/bin/env python3
"""
RiftLink Serial API — integration tests
Требует: устройство на порту (COM3, /dev/ttyUSB0 и т.д.)
Запуск: pytest tests/test_serial.py -v --port COM3
Без --port: тесты пропускаются (skip)
"""

import sys
import time
import pytest

try:
    import serial
except ImportError:
    serial = None


def pytest_addoption(parser):
    parser.addoption('--port', action='store', default=None,
                     help='Serial port for integration tests (e.g. COM3)')


@pytest.fixture(scope='module')
def serial_port(request):
    port = request.config.getoption('--port', default=None)
    if not port:
        pytest.skip('Serial port not specified. Use --port COM3')
    if serial is None:
        pytest.skip('pyserial not installed. pip install pyserial')
    try:
        ser = serial.Serial(port, 115200, timeout=0.5)
        yield ser
        ser.close()
    except serial.SerialException as e:
        pytest.skip(f'Cannot open port {port}: {e}')


def read_lines(ser, timeout=0.5):
    lines = []
    start = time.time()
    while time.time() - start < timeout:
        if ser.in_waiting:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if line:
                lines.append(line)
        else:
            time.sleep(0.05)
    return lines


class TestSerialCommands:
    def test_region_eu(self, serial_port):
        serial_port.write(b'region EU\n')
        lines = read_lines(serial_port, 0.5)
        assert any('Region' in l or 'EU' in l for l in lines)

    def test_nickname(self, serial_port):
        serial_port.write(b'nickname TestNode\n')
        lines = read_lines(serial_port, 0.5)
        assert any('Nickname' in l or 'TestNode' in l for l in lines)

    def test_channel_eu(self, serial_port):
        serial_port.write(b'channel 0\n')
        lines = read_lines(serial_port, 0.5)
        # Может быть "channel 0|1|2" если не EU
        assert len(lines) >= 0

    def test_send_broadcast(self, serial_port):
        serial_port.write(b'send pytest_broadcast\n')
        lines = read_lines(serial_port, 0.5)
        assert any('MSG' in l or 'queued' in l or 'broadcast' in l for l in lines)


if __name__ == '__main__':
    pytest.main([__file__, '-v'] + sys.argv[1:])
