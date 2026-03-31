/// BLE GATT transport for RiftLink — NUS (Nordic UART Service).
/// Extracted from RiftLinkBle's BLE-specific connection logic.

import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart' show debugPrint, kDebugMode;
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'riftlink_transport.dart';

class BleTransport implements RiftLinkTransport {
  static const _serviceUuid = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
  static const _charTxUuid = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
  static const _charRxUuid = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';

  BluetoothDevice? _device;
  BluetoothCharacteristic? _txChar;
  BluetoothCharacteristic? _rxChar;
  StreamSubscription<OnCharacteristicReceivedEvent>? _rxSub;

  final _jsonController = StreamController<String>.broadcast();
  final List<int> _rxAccum = [];

  BluetoothDevice? get device => _device;

  @override
  bool get isConnected => _device?.isConnected ?? false;

  @override
  Stream<String> get rawJsonStream => _jsonController.stream;

  /// Connect to a BLE device and discover NUS service.
  Future<bool> connectDevice(BluetoothDevice dev) async {
    await disconnect();
    _device = dev;
    try {
      await dev.connect();
      await dev.discoverServices();
    } catch (_) {
      _device = null;
      rethrow;
    }

    final service = dev.servicesList
        .where((s) => s.uuid.toString().toLowerCase() == _serviceUuid)
        .firstOrNull;
    if (service == null) {
      await disconnect();
      return false;
    }

    _txChar = service.characteristics
        .where((c) => c.uuid.toString().toLowerCase() == _charTxUuid)
        .firstOrNull;
    _rxChar = service.characteristics
        .where((c) => c.uuid.toString().toLowerCase() == _charRxUuid)
        .firstOrNull;

    if (_txChar == null || _rxChar == null) {
      await disconnect();
      return false;
    }

    // MTU уже запрошен внутри BluetoothDevice.connect() на Android (дефолт 512); повтор — GATT_INVALID_PDU.

    await _startRx();
    return true;
  }

  @override
  Future<bool> sendJson(String json) async {
    if (_txChar == null || !isConnected) return false;
    try {
      final b = utf8.encode(json);
      await _txChar!.write([...b, 0x0A], withoutResponse: true);
      return true;
    } catch (_) {
      return false;
    }
  }

  @override
  Future<bool> sendRaw(List<int> data) async {
    if (_txChar == null || !isConnected) return false;
    try {
      await _txChar!.write(data, withoutResponse: true);
      return true;
    } catch (_) {
      return false;
    }
  }

  @override
  Future<void> disconnect() async {
    final old = _rxSub;
    _rxSub = null;
    await old?.cancel();
    _rxAccum.clear();
    if (_device != null) {
      try { await _device!.disconnect(); } catch (_) {}
      _device = null;
    }
    _txChar = null;
    _rxChar = null;
  }

  Future<void> _startRx() async {
    if (_rxChar == null || _device == null) return;
    final oldSub = _rxSub;
    _rxSub = null;
    await oldSub?.cancel();

    final devId = _device!.remoteId;
    final rxUuid = _rxChar!.uuid;

    _rxSub = FlutterBluePlus.events.onCharacteristicReceived.listen((event) {
      if (event.device.remoteId != devId) return;
      if (event.characteristic.uuid != rxUuid) return;
      _onRxData(event.value);
    });

    await Future<void>.delayed(const Duration(milliseconds: 500));
    try {
      await _rxChar!.setNotifyValue(true);
    } catch (e) {
      if (kDebugMode) debugPrint('BleTransport: setNotifyValue error: $e');
    }
  }

  void _onRxData(List<int> bytes) {
    if (bytes.isEmpty) return;
    
    // Лог для отладки — покажем что пришло
    final hexBytes = bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join(' ');
    print('[BLE_TRANSPORT] RX bytes=[${bytes.join(',')}] hex=[$hexBytes] accum_before=${_rxAccum.length}');
    
    _rxAccum.addAll(bytes);

    // Try to extract complete JSON objects from accumulator
    final s = utf8.decode(_rxAccum, allowMalformed: true);
    
    // Лог для отладки — покажем что декодировалось
    print('[BLE_TRANSPORT] Decoded string (${s.length} chars): ${s.length > 100 ? s.substring(0, 100) + '...' : s}');

    // NDJSON: split by newlines
    final lines = s.split('\n');
    print('[BLE_TRANSPORT] Split into ${lines.length} lines');
    
    if (lines.length > 1) {
      _rxAccum.clear();
      for (int i = 0; i < lines.length; i++) {
        final line = lines[i].trim();
        print('[BLE_TRANSPORT] Line $i: (${line.length} chars) endsWithBrace=${line.endsWith('}')}');
        if (line.isEmpty) continue;

        // Проверяем содержит ли сегмент полный JSON (закрывается на })
        final hasCompleteJson = line.contains('{') && line.endsWith('}');

        if (i == lines.length - 1 && !hasCompleteJson) {
          // Last segment may be incomplete - keep in accumulator
          print('[BLE_TRANSPORT] Keeping incomplete line in accumulator');
          _rxAccum.addAll(utf8.encode(line));
        } else {
          // Complete JSON line - emit immediately
          print('[BLE_TRANSPORT] Emitting complete JSON line');
          _jsonController.add(line);
        }
      }
      return;
    }

    // Try single JSON object
    if (s.contains('{') && s.contains('}')) {
      // Try to find complete JSON objects
      var depth = 0;
      var start = -1;
      var inStr = false;
      var esc = false;
      var lastEnd = 0;

      for (var i = 0; i < s.length; i++) {
        final ch = s.codeUnitAt(i);
        if (inStr) {
          if (esc) { esc = false; continue; }
          if (ch == 0x5C) { esc = true; continue; }
          if (ch == 0x22) inStr = false;
          continue;
        }
        if (ch == 0x22) { inStr = true; continue; }
        if (ch == 0x7B) {
          if (depth == 0) start = i;
          depth++;
        } else if (ch == 0x7D && depth > 0) {
          depth--;
          if (depth == 0 && start >= 0) {
            _jsonController.add(s.substring(start, i + 1));
            lastEnd = i + 1;
            start = -1;
          }
        }
      }

      if (lastEnd > 0) {
        final remaining = s.substring(lastEnd);
        _rxAccum.clear();
        if (remaining.isNotEmpty) _rxAccum.addAll(utf8.encode(remaining));
        return;
      }
    }

    // Overflow protection
    if (_rxAccum.length > 4096) {
      print('[BLE_TRANSPORT] RX overflow, clearing accumulator');
      _rxAccum.clear();
    }
  }
}
