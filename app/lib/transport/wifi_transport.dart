/// WiFi WebSocket transport for RiftLink.
/// Connects to the device's WebSocket server at ws://<ip>:80/ws.
/// Same JSON protocol as BLE GATT.

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart' show debugPrint, kDebugMode;

import 'riftlink_transport.dart';

class WifiTransport implements RiftLinkTransport {
  WebSocket? _socket;
  StreamSubscription? _socketSub;
  final _jsonController = StreamController<String>.broadcast();
  String? _ip;

  /// Вызывается при обрыве сокета со стороны узла/сети (не при [disconnect]).
  /// Колбэк сбрасывает Wi‑Fi‑режим в приложении и шлёт evt в UI — иначе ждут только таймаут cmd.
  void Function()? onConnectionLost;

  bool _manualDisconnect = false;
  bool _lostNotified = false;

  String? get ip => _ip;

  @override
  bool get isConnected => _socket != null;

  @override
  Stream<String> get rawJsonStream => _jsonController.stream;

  /// Connect to the device's WebSocket server.
  Future<bool> connectToDevice(String ip, {int port = 80}) async {
    await disconnect();
    _ip = ip;
    try {
      _socket = await WebSocket.connect('ws://$ip:$port/ws')
          .timeout(const Duration(seconds: 10));
    } catch (e) {
      if (kDebugMode) debugPrint('WifiTransport: connect error: $e');
      _socket = null;
      return false;
    }

    // Ускоряет обнаружение «полуоткрытого» TCP, когда узел выключили без FIN/RST.
    _socket!.pingInterval = const Duration(seconds: 15);

    _socketSub = _socket!.listen(
      (data) {
        // Один WebSocket-текстовый фрейм = один кусок потока (как BLE notify). Не режем по '\n':
        // внутри JSON-строк полей могут быть переводы строк — split ломает разбор.
        // Склейка фрагментов и NDJSON — в RiftLinkBle._feedRxChunk / _drainRxAccum.
        if (data is String) {
          if (data.isNotEmpty) _jsonController.add(data);
        } else if (data is List<int>) {
          if (data.isNotEmpty) {
            _jsonController.add(utf8.decode(data, allowMalformed: true));
          }
        }
      },
      onDone: () {
        if (kDebugMode) debugPrint('WifiTransport: WebSocket closed');
        _socket = null;
        _notifyConnectionLost();
      },
      onError: (e) {
        if (kDebugMode) debugPrint('WifiTransport: WebSocket error: $e');
        _socket = null;
        _notifyConnectionLost();
      },
    );

    if (kDebugMode) debugPrint('WifiTransport: connected to ws://$ip:$port/ws');
    return true;
  }

  @override
  Future<bool> sendJson(String json) async {
    if (_socket == null) return false;
    try {
      _socket!.add(json);
      return true;
    } catch (_) {
      return false;
    }
  }

  @override
  Future<bool> sendRaw(List<int> data) async {
    if (_socket == null) return false;
    try {
      _socket!.add(data);
      return true;
    } catch (_) {
      return false;
    }
  }

  void _notifyConnectionLost() {
    if (_manualDisconnect || _lostNotified) return;
    _lostNotified = true;
    final cb = onConnectionLost;
    onConnectionLost = null;
    cb?.call();
  }

  @override
  Future<void> disconnect() async {
    _manualDisconnect = true;
    await _socketSub?.cancel();
    _socketSub = null;
    try { await _socket?.close(); } catch (_) {}
    _socket = null;
    _manualDisconnect = false;
    _lostNotified = false;
  }
}
