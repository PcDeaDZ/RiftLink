# RiftLink — тесты

## Python (pytest)

**Unit-тесты** (без железа):
```bash
pip install -r tests/requirements.txt
pytest tests/test_protocol.py tests/test_api.py -v
```

**Serial integration** (требует устройство на порту):
```bash
pytest tests/test_serial.py -v --port COM3
```
Без `--port` тесты пропускаются.

## Flutter

```bash
cd app
flutter test
```

## Структура

| Файл | Описание |
|------|----------|
| test_protocol.py | Формат пакета LoRa (build/parse) |
| test_api.py | BLE/Serial JSON команды и события |
| test_serial.py | Интеграционные тесты Serial API |
| app/test/widget_test.dart | Flutter: события BLE, ChatScreen |
