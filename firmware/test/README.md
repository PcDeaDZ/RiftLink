# Тесты прошивки

## Native-тесты

**protocol/packet** — roundtrip, битые пакеты, relay, opcodes, граничные случаи, сценарии (ожидания синхронизированы с `packet.cpp`: v2.3 HELLO с 2-байтовым tag, strict-only парсер без сканирования sync).

**ui** (`test_ui.cpp`, один `main` в `test_packet.cpp`) — прокрутка списка (`ui_scroll`), RSSI→полоски (`ui_topbar`), прокрутка текста сообщения (`ui_msg_scroll`).

Сборка native-тестов линкует `src/protocol/packet.cpp` (`test_build_src = yes` в `[env:native]`); без этого линковка пустая, а два `main` / два `setUp` в `test/*.cpp` дают ошибку линкера.

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
