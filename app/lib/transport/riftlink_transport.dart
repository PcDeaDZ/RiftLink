/// Abstract transport layer for RiftLink device communication.
/// Both BLE GATT and WiFi WebSocket implement this interface.

import 'dart:async';

abstract class RiftLinkTransport {
  /// Transport is connected and ready to send/receive.
  bool get isConnected;

  /// Stream of raw JSON strings received from the device.
  Stream<String> get rawJsonStream;

  /// Send a raw JSON string to the device. Returns true on success.
  Future<bool> sendJson(String json);

  /// Send raw binary data to the device (for BLE OTA chunks).
  Future<bool> sendRaw(List<int> data);

  /// Disconnect and release resources.
  Future<void> disconnect();
}
