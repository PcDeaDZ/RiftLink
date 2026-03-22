import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../ble/riftlink_ble.dart';
import '../recent_devices/recent_devices_service.dart';
import 'reconnect_overlay_controller.dart';

TransportReconnectManager? transportReconnectManager;

enum TransportReconnectEventType {
  trigger,
  attempt,
  success,
  failed,
}

class TransportReconnectEvent {
  final TransportReconnectEventType type;
  final String reason;
  final int? attempt;

  const TransportReconnectEvent({
    required this.type,
    required this.reason,
    this.attempt,
  });
}

class TransportReconnectUiState {
  final bool reconnecting;
  final int attempt;
  final int attemptDurationMs;
  final double attemptProgress;

  const TransportReconnectUiState({
    required this.reconnecting,
    required this.attempt,
    required this.attemptDurationMs,
    required this.attemptProgress,
  });
}

class TransportReconnectManager {
  static const Duration _disconnectAttemptTimeout = Duration(milliseconds: 1800);
  static const Duration _preConnectDelay = Duration(milliseconds: 500);
  static const Duration _connectAttemptTimeout = Duration(seconds: 8);
  static const Duration _interAttemptDelay = Duration(seconds: 2);
  static const Duration _attemptProgressTick = Duration(milliseconds: 33);

  final RiftLinkBle _ble;
  StreamSubscription<OnConnectionStateChangedEvent>? _connSub;
  StreamSubscription<RiftLinkEvent>? _activitySub;
  Timer? _watchdogTimer;
  final StreamController<TransportReconnectEvent> _events =
      StreamController<TransportReconnectEvent>.broadcast();
  final ValueNotifier<TransportReconnectUiState> uiState =
      ValueNotifier(TransportReconnectUiState(
        reconnecting: false,
        attempt: 0,
        attemptDurationMs: _disconnectAttemptTimeout.inMilliseconds +
            _preConnectDelay.inMilliseconds +
            _connectAttemptTimeout.inMilliseconds,
        attemptProgress: 0,
      ));

  bool _reconnecting = false;
  bool _hasConnectedInRuntime = false;
  bool _awaitingUserActionAfterFailure = false;
  bool _autoReconnectSuppressed = false;
  DateTime? _lastTriggerAt;
  DateTime _lastTransportActivityAt = DateTime.now();
  bool _probePending = false;
  DateTime? _probeAt;
  Timer? _attemptProgressTimer;

  TransportReconnectManager(this._ble);

  Stream<TransportReconnectEvent> get events => _events.stream;

  void start() {
    _connSub?.cancel();
    _activitySub?.cancel();
    _watchdogTimer?.cancel();

    _connSub = FlutterBluePlus.events.onConnectionStateChanged.listen((event) {
      if (event.connectionState == BluetoothConnectionState.connected) {
        _hasConnectedInRuntime = true;
        if (_reconnecting) {
          // Hide fail overlay only when this connected event belongs
          // to an active reconnect flow.
          reconnectOverlayController.hide();
        }
      }
      if (_reconnecting || _awaitingUserActionAfterFailure || _autoReconnectSuppressed) return;
      if (_ble.isWifiMode) return;
      final tracked = _ble.device?.remoteId.toString() ?? _ble.lastBleRemoteId;
      if (tracked == null || tracked.isEmpty) return;
      if (!RiftLinkBle.remoteIdsMatch(event.device.remoteId.toString(), tracked)) return;
      if (event.connectionState == BluetoothConnectionState.disconnected) {
        unawaited(triggerReconnect(reason: 'ble_disconnected'));
      }
    });

    _activitySub = _ble.events.listen((_) {
      _hasConnectedInRuntime = true;
      _lastTransportActivityAt = DateTime.now();
      _probePending = false;
      _probeAt = null;
    });

    _watchdogTimer = Timer.periodic(const Duration(milliseconds: 1200), (_) {
      if (_reconnecting) return;
      final now = DateTime.now();

      if (_autoReconnectSuppressed) {
        return;
      }

      if (_awaitingUserActionAfterFailure) {
        // Terminal failed state: do not auto-restart reconnect loops.
        // Exit only via explicit user action (overlay/manual) or explicit
        // Bluetooth "connected" event in _connSub.
        return;
      }

      // Do not auto-reconnect on cold app start before any real session.
      if (!_hasConnectedInRuntime) {
        if (_ble.isTransportConnected) _hasConnectedInRuntime = true;
        return;
      }

      if (!_ble.isTransportConnected) {
        unawaited(triggerReconnect(reason: 'transport_down'));
        return;
      }

      const idleThreshold = Duration(seconds: 9);
      const probeTimeout = Duration(seconds: 5);
      final idleFor = now.difference(_lastTransportActivityAt);
      if (idleFor < idleThreshold) return;

      if (!_probePending) {
        _probePending = true;
        _probeAt = now;
        unawaited(_ble.getInfo(force: true).then((ok) {
          if (!ok) {
            _probePending = false;
            _probeAt = null;
            unawaited(triggerReconnect(reason: 'probe_send_failed'));
          }
        }));
        return;
      }

      final probeAt = _probeAt;
      if (probeAt != null && now.difference(probeAt) >= probeTimeout) {
        _probePending = false;
        _probeAt = null;
        unawaited(triggerReconnect(reason: 'probe_timeout'));
      }
    });
  }

  Future<void> stop() async {
    await _connSub?.cancel();
    await _activitySub?.cancel();
    _watchdogTimer?.cancel();
    _attemptProgressTimer?.cancel();
    await _events.close();
    uiState.dispose();
    _connSub = null;
    _activitySub = null;
    _watchdogTimer = null;
  }

  Future<void> triggerReconnect({String reason = 'manual'}) async {
    if (_reconnecting) return;
    final userInitiated = reason == 'overlay_retry' || reason == 'manual';
    if (userInitiated) {
      _awaitingUserActionAfterFailure = false;
      reconnectOverlayController.hide();
    }
    if (_autoReconnectSuppressed && !userInitiated) return;
    if (_awaitingUserActionAfterFailure && !userInitiated) return;
    final now = DateTime.now();
    final last = _lastTriggerAt;
    if (last != null && now.difference(last) < const Duration(milliseconds: 1200)) return;
    _lastTriggerAt = now;
    if (!_events.isClosed) {
      _events.add(TransportReconnectEvent(
        type: TransportReconnectEventType.trigger,
        reason: reason,
      ));
    }
    try {
      await _reconnectWithRetry();
    } catch (_) {
      // Hard fail-safe: any unexpected exception must move FSM
      // into terminal failed state and show action overlay.
      _awaitingUserActionAfterFailure = true;
      _reconnecting = false;
      _attemptProgressTimer?.cancel();
      uiState.value = TransportReconnectUiState(
        reconnecting: false,
        attempt: 0,
        attemptDurationMs: _disconnectAttemptTimeout.inMilliseconds +
            _preConnectDelay.inMilliseconds +
            _connectAttemptTimeout.inMilliseconds,
        attemptProgress: 0,
      );
      _showFailedOverlay();
    }
  }

  void suppressAutoReconnectUntilNextConnection() {
    _autoReconnectSuppressed = true;
    _awaitingUserActionAfterFailure = false;
    _reconnecting = false;
    uiState.value = TransportReconnectUiState(
      reconnecting: false,
      attempt: 0,
      attemptDurationMs: _disconnectAttemptTimeout.inMilliseconds +
          _preConnectDelay.inMilliseconds +
          _connectAttemptTimeout.inMilliseconds,
      attemptProgress: 0,
    );
    reconnectOverlayController.hide();
  }

  /// Re-enable automatic reconnect logic (usually called before a user-initiated connect flow).
  void resumeAutoReconnect() {
    _autoReconnectSuppressed = false;
  }

  Future<void> _reconnectWithRetry() async {
    final isWifi = _ble.isWifiMode;
    final wifiIp = _ble.lastInfo?.wifiIp?.trim();
    String? remoteId = _ble.device?.remoteId.toString() ?? _ble.lastBleRemoteId;

    if (!isWifi && (remoteId == null || remoteId.isEmpty)) {
      final recent = await RecentDevicesService.load();
      if (recent.isNotEmpty) remoteId = recent.first.remoteId;
    }

    if ((isWifi && (wifiIp == null || wifiIp.isEmpty)) || (!isWifi && (remoteId == null || remoteId.isEmpty))) {
      _awaitingUserActionAfterFailure = true;
      _reconnecting = false;
      _attemptProgressTimer?.cancel();
      uiState.value = TransportReconnectUiState(
        reconnecting: false,
        attempt: 0,
        attemptDurationMs: _disconnectAttemptTimeout.inMilliseconds +
            _preConnectDelay.inMilliseconds +
            _connectAttemptTimeout.inMilliseconds,
        attemptProgress: 0,
      );
      _showFailedOverlay();
      return;
    }

    _reconnecting = true;
    _awaitingUserActionAfterFailure = false;
    reconnectOverlayController.hide();
    _attemptProgressTimer?.cancel();
    for (var attempt = 1; attempt <= 3; attempt++) {
      final baseAttemptDurationMs = _disconnectAttemptTimeout.inMilliseconds +
          _preConnectDelay.inMilliseconds +
          _connectAttemptTimeout.inMilliseconds;
      final uiAttemptDurationMs = baseAttemptDurationMs +
          (attempt < 3 ? _interAttemptDelay.inMilliseconds : 0);
      final attemptStartedAt = DateTime.now();
      uiState.value = TransportReconnectUiState(
        reconnecting: true,
        attempt: attempt,
        attemptDurationMs: uiAttemptDurationMs,
        attemptProgress: 0,
      );
      _attemptProgressTimer?.cancel();
      _attemptProgressTimer = Timer.periodic(_attemptProgressTick, (_) {
        if (!_reconnecting) return;
        final elapsedMs = DateTime.now().difference(attemptStartedAt).inMilliseconds;
        final progress = (elapsedMs / uiAttemptDurationMs).clamp(0, 1).toDouble();
        uiState.value = TransportReconnectUiState(
          reconnecting: true,
          attempt: attempt,
          attemptDurationMs: uiAttemptDurationMs,
          attemptProgress: progress,
        );
      });
      if (!_events.isClosed) {
        _events.add(TransportReconnectEvent(
          type: TransportReconnectEventType.attempt,
          reason: 'retry',
          attempt: attempt,
        ));
      }
      try {
        await _ble.disconnect().timeout(
          _disconnectAttemptTimeout,
          onTimeout: () {},
        );
        await Future<void>.delayed(_preConnectDelay);
        final ok = await (() async {
          if (isWifi) {
            return _ble.connectWifi(wifiIp!);
          }
          return _ble.connect(BluetoothDevice.fromId(remoteId!));
        })().timeout(_connectAttemptTimeout, onTimeout: () => false);
        if (ok) {
          final elapsedMs = DateTime.now().difference(attemptStartedAt).inMilliseconds;
          final remainingMs = baseAttemptDurationMs - elapsedMs;
          if (remainingMs > 0) {
            await Future<void>.delayed(Duration(milliseconds: remainingMs));
          }
          await _ble.getInfo(force: true);
          _reconnecting = false;
          _attemptProgressTimer?.cancel();
          uiState.value = TransportReconnectUiState(
            reconnecting: false,
            attempt: 0,
            attemptDurationMs: _disconnectAttemptTimeout.inMilliseconds +
                _preConnectDelay.inMilliseconds +
                _connectAttemptTimeout.inMilliseconds,
            attemptProgress: 0,
          );
          reconnectOverlayController.hide();
          if (!_events.isClosed) {
            _events.add(const TransportReconnectEvent(
              type: TransportReconnectEventType.success,
              reason: 'connected',
            ));
          }
          return;
        }
      } catch (_) {}
      final elapsedMs = DateTime.now().difference(attemptStartedAt).inMilliseconds;
      final remainingMs = uiAttemptDurationMs - elapsedMs;
      if (remainingMs > 0) {
        await Future<void>.delayed(Duration(milliseconds: remainingMs));
      }
    }
    // Enter terminal failed state immediately after 3/3 and show actions first.
    _awaitingUserActionAfterFailure = true;
    _reconnecting = false;
    _attemptProgressTimer?.cancel();
    uiState.value = TransportReconnectUiState(
      reconnecting: false,
      attempt: 0,
      attemptDurationMs: _disconnectAttemptTimeout.inMilliseconds +
          _preConnectDelay.inMilliseconds +
          _connectAttemptTimeout.inMilliseconds,
      attemptProgress: 0,
    );
    _showFailedOverlay();
    try {
      await _ble.disconnect();
    } catch (_) {}
    if (!_events.isClosed) {
      _events.add(const TransportReconnectEvent(
        type: TransportReconnectEventType.failed,
        reason: 'max_attempts',
      ));
    }
  }

  void _showFailedOverlay() {
    reconnectOverlayController.show(onRetry: () => triggerReconnect(reason: 'overlay_retry'));
  }
}
