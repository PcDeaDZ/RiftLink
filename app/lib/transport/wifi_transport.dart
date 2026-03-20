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

    _socketSub = _socket!.listen(
      (data) {
        if (data is String) {
          // May contain multiple JSON objects separated by newlines
          for (final line in data.split('\n')) {
            final trimmed = line.trim();
            if (trimmed.isNotEmpty) {
              _jsonController.add(trimmed);
            }
          }
        }
      },
      onDone: () {
        if (kDebugMode) debugPrint('WifiTransport: WebSocket closed');
        _socket = null;
      },
      onError: (e) {
        if (kDebugMode) debugPrint('WifiTransport: WebSocket error: $e');
        _socket = null;
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

  @override
  Future<void> disconnect() async {
    await _socketSub?.cancel();
    _socketSub = null;
    try { await _socket?.close(); } catch (_) {}
    _socket = null;
  }
}
