# Тесты прошивки

## Native-тесты (protocol/packet)

54 теста: roundtrip, битые пакеты, relay, opcodes, граничные случаи, сценарии.

| Категория | Тесты |
|-----------|-------|
| **Базовые** | broadcast/unicast roundtrip, invalid too short, BROADCAST_ID, sync byte |
| **Битые пакеты** | garbage before, shifted sync (SX1262), wrong sync/version, truncated, HELLO с payload, MSG payload too short, all garbage, ACK wrong length |
| **Relay** | broadcast HELLO, unicast from/to, ECHO broadcast, POLL (RIT) |
| **Opcodes** | KEY_EXCHANGE, ROUTE_REQ, ROUTE_REPLY, PING/PONG, READ, MSG_FRAG, GROUP_MSG |
| **buildPacket** | buffer too small, null payload, all flags, channel 0–2, TTL 255 |
| **parsePacket** | zero length, buffer too large rejected |
| **API** | getExpectedPayloadRange (все opcodes), getExpectedPacketLength |
| **Граничные** | ACK broadcast, NACK payload, MAX_PAYLOAD, multiple packets |
| **Сценарии** | mesh chain A→B→C, KEY_EXCHANGE→MSG, TELEMETRY/LOCATION/VOICE_MSG/XOR_RELAY roundtrip, payload integrity, node_id=0, unicast ACK, exact buffer size, flags independent, garbage between packets, ROUTE_REQ broadcast, READ unicast |

Требуется **GCC** в PATH:
- **Windows**: установите [MSYS2](https://www.msys2.org/), добавьте `C:\msys64\mingw64\bin` в PATH
- **Linux**: `sudo apt install build-essential`
- **macOS**: `xcode-select --install`

Запуск:
```bash
cd firmware
pio test -e native
```
