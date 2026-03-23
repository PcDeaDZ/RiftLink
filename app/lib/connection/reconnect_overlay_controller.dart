import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../ble/riftlink_ble.dart';

class ReconnectOverlayController extends ChangeNotifier {
  bool _visible = false;
  bool _busy = false;
  Future<void> Function()? _retryAction;

  bool get visible => _visible;
  bool get busy => _busy;

  void show({Future<void> Function()? onRetry}) {
    _retryAction = onRetry;
    _visible = true;
    _busy = false;
    notifyListeners();
  }

  void hide() {
    if (!_visible && !_busy && _retryAction == null) return;
    _visible = false;
    _busy = false;
    _retryAction = null;
    notifyListeners();
  }

  Future<void> runRetry(RiftLinkBle ble) async {
    if (_busy) return;
    final customAction = _retryAction;
    _busy = true;
    notifyListeners();

    if (customAction != null) {
      _visible = false;
      _retryAction = null;
      _busy = false;
      notifyListeners();
      await customAction();
      return;
    }

    _visible = false;
    notifyListeners();
    final ok = await _retryDefault(ble);
    _busy = false;
    if (!ok) _visible = true;
    notifyListeners();
  }

  Future<bool> _retryDefault(RiftLinkBle ble) async {
    if (ble.isWifiMode) return false;
    final remoteId = ble.device?.remoteId.toString() ?? ble.lastBleRemoteId;
    if (remoteId == null || remoteId.isEmpty) return false;
    for (var attempt = 1; attempt <= 3; attempt++) {
      try {
        await ble.disconnect();
        await Future<void>.delayed(const Duration(milliseconds: 500));
        final ok = await ble.connect(
          BluetoothDevice.fromId(remoteId),
          internalReconnect: true,
        );
        if (ok) return true;
      } catch (_) {}
      if (attempt < 3) {
        await Future<void>.delayed(const Duration(seconds: 2));
      }
    }
    await ble.disconnect();
    return false;
  }
}

final reconnectOverlayController = ReconnectOverlayController();
