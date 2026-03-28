/// RiftLink BLE — связь с Heltec LoRa по GATT / WiFi WebSocket
/// Протокол: JSON {"cmd":"send","text":"..."} / {"evt":"msg","from":"...","text":"..."}
/// Dual transport: BLE (default) ↔ WiFi (on demand) through radio mode switching.

import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart' show debugPrint, kDebugMode, kIsWeb;
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../connection/transport_reconnect_manager.dart';
import '../transport/transport_response_router.dart'
    show GroupSecurityResponseError, TransportResponseRouter;
import '../transport/wifi_transport.dart';
import '../utils/group_invite_normalize.dart';

/// Волна ответа на `cmd:info`: node + neighbors + routes + groups с одним `seq`/`cmdId`.
/// Tracked-команду завершает первое событие из этого набора с подходящим `cmdId` (см. [TransportResponseRouter]).
/// Завершение tracked `cmd:info` в [TransportResponseRouter]: только якоря с паспортом узла.
/// (Полная волна на прошивке: node → neighbors → routes → groups — см. merge в [_emitParsedJson].)
/// `routes`/`groups` не должны закрывать запрос — иначе Completer срабатывает на первом `evt:routes`,
/// а `id` в UI остаётся пустым, пока `node`/`neighbors` ещё в пути или потеряны.
const Set<String> kInfoTrackedResponseEvents = {'node', 'neighbors'};

class RiftLinkBle {
  static const serviceUuid = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
  static const charTxUuid = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
  static const charRxUuid = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';
  static const deviceName = 'RiftLink';
  static const int bleAttMaxJsonBytes = 512;

  static bool exceedsBleAttLimit(String json) =>
      utf8.encode(json).length > bleAttMaxJsonBytes;

  BluetoothDevice? _device;
  String? _lastBleRemoteId;
  BluetoothCharacteristic? _txChar;
  BluetoothCharacteristic? _rxChar;

  /// Broadcast: каждый подписчик получает каждое событие.
  /// Пока `listen` ещё не вызван (окно после connect, async initState), события копятся в [_preListenBuffer]
  /// и сливаются в поток при первом подписчике — иначе Dart broadcast **теряет** add без слушателей.
  int _eventsStreamListeners = 0;
  final List<RiftLinkEvent> _preListenBuffer = [];
  static const int _maxPreListenBuffer = 256;
  final Map<String, int> _diagCounters = <String, int>{};
  int _diagEventsSinceDump = 0;
  DateTime _diagLastDumpAt = DateTime.fromMillisecondsSinceEpoch(0);

  late final StreamController<RiftLinkEvent> _eventBus = StreamController<RiftLinkEvent>.broadcast(
    onListen: _onEventsStreamListen,
    onCancel: _onEventsStreamCancel,
  );
  late final StreamController<Map<String, dynamic>> _rawEventBus =
      StreamController<Map<String, dynamic>>.broadcast();

  void _trace(String message) {
    debugPrint('[BLE_CHAIN] $message');
  }

  void _diagInc(String key, [int by = 1]) {
    _diagCounters[key] = (_diagCounters[key] ?? 0) + by;
    _diagEventsSinceDump += by;
  }

  void _diagMaybeDump(String reason) {
    final now = DateTime.now();
    if (_diagEventsSinceDump < 20 && now.difference(_diagLastDumpAt) < const Duration(seconds: 30)) {
      return;
    }
    _diagLastDumpAt = now;
    _diagEventsSinceDump = 0;
    _trace(
      'stage=app_diag action=snapshot reason=$reason '
      'tx_attempt=${_diagCounters['tx_attempt'] ?? 0} '
      'tx_ok=${_diagCounters['tx_ok'] ?? 0} '
      'tx_fail=${_diagCounters['tx_fail'] ?? 0} '
      'tx_fail_no_transport=${_diagCounters['tx_fail_no_transport'] ?? 0} '
      'rx_chunk=${_diagCounters['rx_chunk'] ?? 0} '
      'rx_retain_overflow=${_diagCounters['rx_retain_overflow'] ?? 0} '
      'rx_retain_timeout=${_diagCounters['rx_retain_timeout'] ?? 0} '
      'rx_retain_large_partial=${_diagCounters['rx_retain_large_partial'] ?? 0} '
      'drop_unknown_evt=${_diagCounters['drop_unknown_evt'] ?? 0} '
      'drop_json_to_event=${_diagCounters['drop_json_to_event'] ?? 0} '
      'drop_parse_ndjson=${_diagCounters['drop_parse_ndjson'] ?? 0} '
      'drop_parse_extracted=${_diagCounters['drop_parse_extracted'] ?? 0} '
      'rx_tail_byte_mismatch=${_diagCounters['rx_tail_byte_mismatch'] ?? 0} '
      'prelisten_drop=${_diagCounters['prelisten_drop'] ?? 0}',
    );
  }

  void _onEventsStreamListen() {
    _eventsStreamListeners++;
    _trace('stage=app_stream action=listen listeners=$_eventsStreamListeners buffered=${_preListenBuffer.length}');
    if (_eventsStreamListeners == 1 && _preListenBuffer.isNotEmpty) {
      final batch = List<RiftLinkEvent>.from(_preListenBuffer);
      _preListenBuffer.clear();
      for (final e in batch) {
        if (!_eventBus.isClosed) _eventBus.add(e);
      }
      _trace('stage=app_stream action=flush_prelisten count=${batch.length}');
    }
  }

  void _onEventsStreamCancel() {
    if (_eventsStreamListeners > 0) _eventsStreamListeners--;
    _trace('stage=app_stream action=cancel listeners=$_eventsStreamListeners');
  }

  StreamSubscription<OnCharacteristicReceivedEvent>? _rxSub;
  /// Склейка фрагментов notify (длинный `info` / MTU): каждый chunk может быть не целым JSON.
  final List<int> _rxAccum = [];
  int _lastRxIncompleteLogLen = 0;
  Timer? _rxAccumTimeout;
  /// Pipelined BLE write: up to [_txPipelineDepth] concurrent writes to avoid
  /// stalling on sequential Completer waits. The BLE ATT layer with
  /// writeWithoutResponse can handle multiple in-flight writes.
  static const int _txPipelineDepth = 3;
  int _txInFlight = 0;
  Completer<void>? _txDrain;
  DateTime? _lastInfoRequestAt;
  DateTime? _lastInfoEventAt;
  int _nextCmdId = 1;
  late final TransportResponseRouter _responseRouter = TransportResponseRouter(
    sendCommand: _sendCmd,
    responses: _rawEventBus.stream,
    commandIdFactory: _allocateCmdId,
    trace: (message) => _trace(message),
  );
  /// Последняя ошибка `evt: groupSecurityError`, сопоставленная роутером с tracked-командой ([GroupSecurityResponseError]).
  GroupSecurityResponseError? _lastGroupSecurityError;
  /// Последний `evt: groupStatus` с `inviteNoop` после `groupInviteAccept` (группа уже была — не перезаписывали роль).
  bool _groupInviteAcceptNoop = false;
  Timer? _queuedInfoTimer;
  bool _hasQueuedInfoRequest = false;

  /// Последний успешно распарсенный снимок узла (склейка `evt:node` + neighbors/routes/groups).
  RiftLinkInfoEvent? _lastInfo;

  /// Частичные evt склеиваются в один снимок (без монолитного `evt:info` на прошивке).
  RiftLinkInfoEvent? _compositeInfo;

  RiftLinkInfoEvent? get lastInfo => _lastInfo;

  void _replayLastInfo() {
    final info = _lastInfo;
    if (info == null) return;
    if (_eventBus.isClosed) return;
    if (_eventsStreamListeners > 0) {
      _trace('stage=app_event_bus action=replay_last_info');
      scheduleMicrotask(() {
        if (!_eventBus.isClosed && _eventsStreamListeners > 0) {
          _eventBus.add(info);
        }
      });
    }
  }

  BluetoothDevice? get device => _device;
  String? get lastBleRemoteId => _lastBleRemoteId;
  bool get isConnected => _device?.isConnected ?? false;
  bool get isTransportConnected =>
      (_isWifiMode && _wifiTransport?.isConnected == true) || isConnected;

  /// Проверка, что устройство — RiftLink (по имени RL-*, RiftLink, Heltec или service UUID)
  static bool isRiftLink(ScanResult r) {
    final name = r.device.platformName;
    final advName = r.device.advName;
    final n = (name.isNotEmpty ? name : advName).toLowerCase();
    if (n.isNotEmpty) {
      if (n.contains('riftlink') || n.startsWith('rl-') || n.contains('heltec') || n.contains('lora')) return true;
    }
    final uuids = r.advertisementData.serviceUuids;
    if (uuids.isNotEmpty) {
      final s = serviceUuid.toLowerCase().replaceAll('-', '');
      for (final u in uuids) {
        if (u.toString().toLowerCase().replaceAll('-', '') == s) return true;
      }
    }
    return false;
  }

  static Future<void> startScan({Duration timeout = const Duration(seconds: 10)}) async {
    await FlutterBluePlus.startScan(
      timeout: timeout,
      androidUsesFineLocation: true,
      androidCheckLocationServices: false,
      androidLegacy: true,
    );
  }

  static Future<void> stopScan() async {
    await FlutterBluePlus.stopScan();
  }

  /// Нормализация BLE remoteId (MAC / UUID) для сравнения между экранами и версиями FBP.
  static String normalizeBleRemoteId(String s) =>
      s.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();

  static bool remoteIdsMatch(String a, String b) {
    final na = normalizeBleRemoteId(a);
    final nb = normalizeBleRemoteId(b);
    return na.isNotEmpty && na == nb;
  }

  /// Сравнение UUID характеристик (FBP может отдавать разный формат строки).
  static bool characteristicUuidsMatch(Object a, Object b) {
    final sa = a.toString().toLowerCase().replaceAll('-', '');
    final sb = b.toString().toLowerCase().replaceAll('-', '');
    return sa.isNotEmpty && sa == sb;
  }

  /// Короткий ID из имени BLE (`RL-XXXXXXXX`) — пока нет полного `evt.info.id`.
  static String? nodeIdHintFromDevice(BluetoothDevice? dev) {
    if (dev == null) return null;
    final name = dev.platformName.isNotEmpty ? dev.platformName : dev.advName;
    final m = RegExp(r'RL-([0-9A-Fa-f]{16})').firstMatch(name);
    return m != null ? m.group(1)!.toUpperCase() : null;
  }

  static bool isValidFullNodeId(String id) {
    final norm = id.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();
    return RegExp(r'^[0-9A-F]{16}$').hasMatch(norm);
  }

  Future<bool> _sendCmd(Map<String, dynamic> payload) async {
    final cmd = payload['cmd']?.toString() ?? 'unknown';
    final cmdId = _extractOrAttachCmdId(payload);
    final json = jsonEncode(payload);
    final bytes = utf8.encode(json);
    _diagInc('tx_attempt');
    // WiFi mode: route through WebSocket (no MTU limit)
    if (_isWifiMode && _wifiTransport != null && _wifiTransport!.isConnected) {
      final ok = await _wifiTransport!.sendJson(json);
      _diagInc(ok ? 'tx_ok' : 'tx_fail');
      _trace('stage=app_tx action=send mode=wifi cmd=$cmd cmdId=$cmdId len=${json.length} ok=$ok');
      _diagMaybeDump(ok ? 'tx_ok' : 'tx_fail');
      return ok;
    }
    if (_txChar == null || !isConnected) {
      _diagInc('tx_fail');
      _diagInc('tx_fail_no_transport');
      _trace('stage=app_tx action=drop reason=no_transport mode=ble cmd=$cmd cmdId=$cmdId len=${json.length}');
      _diagMaybeDump('tx_fail_no_transport');
      return false;
    }
    if (exceedsBleAttLimit(json)) {
      _diagInc('tx_fail');
      _trace(
        'stage=app_tx action=drop reason=payload_too_long mode=ble cmd=$cmd cmdId=$cmdId len=${bytes.length} limit=$bleAttMaxJsonBytes',
      );
      _diagMaybeDump('tx_fail_payload_too_long');
      _emitParsedJson({
        'evt': 'error',
        'code': 'payload_too_long',
        'msg': 'JSON exceeds $bleAttMaxJsonBytes bytes',
        'cmdId': cmdId,
        'len': bytes.length,
      });
      return false;
    }

    return _writeBleBytesSerialized(
      bytes,
      traceOnSuccess: 'stage=app_tx action=send mode=ble cmd=$cmd cmdId=$cmdId len=${json.length} ok=true',
      traceOnFailure: (e) => 'stage=app_tx action=send mode=ble cmd=$cmd cmdId=$cmdId len=${json.length} ok=false err=$e',
    );
  }

  Future<bool> _writeBleBytesSerialized(
    List<int> bytes, {
    required String traceOnSuccess,
    required String Function(Object error) traceOnFailure,
  }) async {
    while (_txInFlight >= _txPipelineDepth) {
      _txDrain ??= Completer<void>();
      await _txDrain!.future;
    }
    _txInFlight++;
    try {
      await _txChar!.write(bytes, withoutResponse: true);
      _diagInc('tx_ok');
      _trace(traceOnSuccess);
      _diagMaybeDump('tx_ok');
      return true;
    } catch (e) {
      _diagInc('tx_fail');
      _trace(traceOnFailure(e));
      _diagMaybeDump('tx_fail');
      debugPrint('RiftLinkBle: BLE write error: $e');
      return false;
    } finally {
      _txInFlight--;
      if (_txInFlight < _txPipelineDepth && _txDrain != null) {
        final c = _txDrain;
        _txDrain = null;
        c?.complete();
      }
    }
  }

  int _allocateCmdId() {
    final id = _nextCmdId;
    _nextCmdId = (id >= 0x7FFFFFFF) ? 1 : (id + 1);
    return id;
  }

  int _extractOrAttachCmdId(Map<String, dynamic> payload) {
    final raw = payload['cmdId'];
    if (raw is int && raw > 0) return raw;
    final next = _allocateCmdId();
    payload['cmdId'] = next;
    return next;
  }

  Future<bool> _requestCommand({
    required String cmd,
    Map<String, dynamic>? payload,
    required Set<String> expectedEvents,
    Duration timeout = const Duration(seconds: 4),
    int retries = 0,
    int sendAttempts = 1,
  }) async {
    _lastGroupSecurityError = null;
    try {
      await _responseRouter.sendRequest(
        cmd: cmd,
        payload: payload,
        expectedEvents: expectedEvents,
        timeout: timeout,
        retries: retries,
        sendAttempts: sendAttempts,
      );
      return true;
    } catch (e) {
      if (e is StateError && e.message.startsWith('info_superseded')) {
        _trace('stage=app_rr action=request_superseded cmd=$cmd err=$e');
      } else {
        _trace('stage=app_rr action=request_fail cmd=$cmd err=$e');
      }
      if (e is GroupSecurityResponseError) {
        _lastGroupSecurityError = e;
      }
      return false;
    }
  }

  /// Последняя ошибка роутера по `groupSecurityError` (тот же `cmdId`, что у команды). Сбрасывает буфер.
  GroupSecurityResponseError? takeLastGroupSecurityRouterError() {
    final e = _lastGroupSecurityError;
    _lastGroupSecurityError = null;
    return e;
  }

  /// После `groupCreate` / другой tracked-команды: `true`, если узел уже ответил `groupSecurityError` (не показывать таймаут).
  /// Сбрасывает сохранённую ошибку.
  bool consumePendingGroupSecurityRouterError() => takeLastGroupSecurityRouterError() != null;

  /// После успешного [groupInviteAccept]: `true`, если узел ответил `groupStatus` с `inviteNoop` (группа уже была на устройстве).
  bool takeGroupInviteAcceptWasNoop() {
    final v = _groupInviteAcceptNoop;
    _groupInviteAcceptNoop = false;
    return v;
  }

  /// Подключение к устройству.
  ///
  /// При смене узла вызывается [disconnect] — без подавления это даёт ложный
  /// `ble_disconnected` в [TransportReconnectManager]. Для пользовательского
  /// подключения подавляем авто‑reconnect на время операции и снимаем с задержкой,
  /// чтобы асинхронное событие от стека BLE успело отфильтроваться.
  ///
  /// [internalReconnect] — вызов из [TransportReconnectManager] / оверлея:
  /// не трогать подавление (иначе сбросится FSM во время активного reconnect).
  Future<bool> connect(BluetoothDevice dev, {bool internalReconnect = false}) async {
    if (!internalReconnect) {
      transportReconnectManager?.suppressAutoReconnectUntilNextConnection();
    }
    try {
      await disconnect();
      _device = dev;
      try {
        await dev.connect();
        await dev.discoverServices();
      } catch (_) {
        _device = null;
        _txChar = null;
        _rxChar = null;
        return false;
      }

      final service = dev.servicesList
          .where((s) => s.uuid.toString().toLowerCase() == serviceUuid)
          .firstOrNull;
      if (service == null) {
        await disconnect();
        return false;
      }

      _txChar = service.characteristics
          .where((c) => c.uuid.toString().toLowerCase() == charTxUuid)
          .firstOrNull;
      _rxChar = service.characteristics
          .where((c) => c.uuid.toString().toLowerCase() == charRxUuid)
          .firstOrNull;

      if (_txChar == null || _rxChar == null) {
        await disconnect();
        return false;
      }
      if (!kIsWeb) {
        try {
          await dev.requestMtu(517);
        } catch (_) {}
      }
      await _startRxDispatcher();
      _lastBleRemoteId = dev.remoteId.toString();
      // Параллельный залп tracked-команд переполняет очередь BLE на узле; серийно снижает cmd_drop / потерю ответов.
      unawaited(() async {
        try {
          await getInfo(force: true);
          await Future<void>.delayed(const Duration(milliseconds: 60));
          await getGroups();
          await Future<void>.delayed(const Duration(milliseconds: 40));
          await getRoutes();
        } catch (_) {}
      }());
      _wifiReconnectIp = null;
      return true;
    } finally {
      if (!internalReconnect) {
        unawaited(
          Future<void>.delayed(const Duration(milliseconds: 450), () {
            transportReconnectManager?.resumeAutoReconnect();
          }),
        );
      }
    }
  }

  /// Запросить снимок узла (`cmd: info` → волна `evt:node` + neighbors/routes/groups с общим `seq`).
  /// На прошивке один `s_pendingInfoCmdId` — новый запрос затирает предыдущий; [TransportResponseRouter]
  /// снимает старые ожидающие `info`, иначе они таймаутятся при новом cmdId.
  /// Централизованный throttle, чтобы экраны не спамили устройству.
  Future<bool> getInfo({bool force = false}) async {
    if (!isTransportConnected) {
      // B2: при недоступном транспорте всё равно отдать кэш в шину — UI остаётся согласованным с последним известным состоянием узла.
      _replayLastInfo();
      return false;
    }
    final now = DateTime.now();
    const forceMinGap = Duration(milliseconds: 900);
    if (force) {
      final last = _lastInfoRequestAt;
      if (last != null && now.difference(last) < forceMinGap) {
        if (_hasQueuedInfoRequest) {
          _replayLastInfo();
          return true;
        }
        _hasQueuedInfoRequest = true;
        _replayLastInfo();
        final wait = forceMinGap - now.difference(last);
        _queuedInfoTimer?.cancel();
        _queuedInfoTimer = Timer(wait, () async {
          _hasQueuedInfoRequest = false;
          if (!isTransportConnected) return;
          _lastInfoRequestAt = DateTime.now();
          await _requestCommand(
            cmd: 'info',
            expectedEvents: kInfoTrackedResponseEvents,
            timeout: const Duration(seconds: 12),
          );
        });
        return true;
      }
      _queuedInfoTimer?.cancel();
      _queuedInfoTimer = null;
      _hasQueuedInfoRequest = false;
      _lastInfoRequestAt = now;
      return _requestCommand(
        cmd: 'info',
        expectedEvents: kInfoTrackedResponseEvents,
        timeout: const Duration(seconds: 12),
      );
    }

    // Keep BLE channel free for status events (sent/delivered/read/undelivered).
    // Frequent info polling floods notify stream and can mask short chat events.
    const minGap = Duration(seconds: 2);
    const freshInfoSkip = Duration(milliseconds: 1800);
    final lastInfoEvt = _lastInfoEventAt;
    if (lastInfoEvt != null && now.difference(lastInfoEvt) < freshInfoSkip) {
      _replayLastInfo();
      return true;
    }
    final last = _lastInfoRequestAt;
    if (last == null || now.difference(last) >= minGap) {
      _lastInfoRequestAt = now;
      return _requestCommand(
        cmd: 'info',
        expectedEvents: kInfoTrackedResponseEvents,
        timeout: const Duration(seconds: 12),
      );
    }

    if (_hasQueuedInfoRequest) {
      _replayLastInfo();
      return true;
    }
    _replayLastInfo();
    _hasQueuedInfoRequest = true;
    final wait = minGap - now.difference(last);
    _queuedInfoTimer?.cancel();
    _queuedInfoTimer = Timer(wait, () async {
      _hasQueuedInfoRequest = false;
      if (!isTransportConnected) return;
      _lastInfoRequestAt = DateTime.now();
      await _requestCommand(
        cmd: 'info',
        expectedEvents: kInfoTrackedResponseEvents,
        timeout: const Duration(seconds: 12),
      );
    });
    return true;
  }

  Future<void> disconnect() async {
    _responseRouter.cancelAll();
    _lastInfo = null;
    _compositeInfo = null;
    _groupInviteAcceptNoop = false;
    _lastInfoEventAt = null;
    _preListenBuffer.clear();
    _queuedInfoTimer?.cancel();
    _queuedInfoTimer = null;
    _hasQueuedInfoRequest = false;
    _lastInfoRequestAt = null;
    await _disconnectWifi();
    _isWifiMode = false;
    _wifiIp = null;
    _wifiReconnectIp = null;
    await _stopRxDispatcher();
    if (_device != null) {
      await _device!.disconnect();
      _device = null;
    }
    _txChar = null;
    _rxChar = null;
  }

  /// Explicit transport-level disconnect (BLE or Wi-Fi).
  /// Kept as a semantic alias for call sites where BLE-only naming is confusing.
  Future<void> disconnectTransport() async => disconnect();

  Future<void> _startRxDispatcher() async {
    if (_rxChar == null || _device == null) return;
    // Не используем [_rxChar.onValueReceived]: в FBP фильтр сравнивает primaryServiceUuid события
    // с полем у объекта из discovery — при null vs не-null (типично на Android) все notify отсекаются.
    final oldRx = _rxSub;
    _rxSub = null;
    await oldRx?.cancel();
    _rxAccum.clear();

    final devId = _device!.remoteId;
    final rx = _rxChar!;

    if (!kIsWeb) {
      await Future<void>.delayed(const Duration(milliseconds: 120));
    }

    // Подписка до setNotifyValue: иначе первые notify после включения CCCD могут прийти до listen().
    _rxSub = FlutterBluePlus.events.onCharacteristicReceived.listen(
      (OnCharacteristicReceivedEvent event) {
        final c = event.characteristic;
        _trace(
          'stage=app_rx action=chr_received mode=ble len=${event.value.length} remote=${c.remoteId.str} chr=${c.characteristicUuid}',
        );
        if (event.error != null && event.value.isEmpty) return;
        if (!RiftLinkBle.remoteIdsMatch(c.remoteId.str, devId.str)) {
          _trace('stage=app_rx action=skip reason=remote_id want=${devId.str} got=${c.remoteId.str}');
          return;
        }
        if (!RiftLinkBle.characteristicUuidsMatch(c.characteristicUuid, rx.characteristicUuid)) {
          _trace('stage=app_rx action=skip reason=uuid want=${rx.characteristicUuid} got=${c.characteristicUuid}');
          return;
        }
        _feedRxChunk(event.value);
      },
      onError: (Object e, StackTrace _) {
        assert(() {
          debugPrint('RiftLinkBle: onCharacteristicReceived error $e');
          return true;
        }());
        _trace('stage=app_rx action=error reason=onCharacteristicReceived err=$e');
      },
    );

    await rx.setNotifyValue(true);
  }

  void _feedRxChunk(List<int> chunk) {
    if (chunk.isEmpty) return;
    _diagInc('rx_chunk');
    _trace('stage=app_rx action=chunk len=${chunk.length} accum_before=${_rxAccum.length}');
    _rxAccum.addAll(chunk);
    const maxAccum = 16384;
    if (_rxAccum.length > maxAccum) {
      final retained = retainRxTailFromLastBraceBytes(_rxAccum, maxRetain: 4096);
      debugPrint(
        'RiftLinkBle: RX buffer overflow (${_rxAccum.length} bytes), retaining tail ${retained.length} bytes',
      );
      _trace('stage=app_rx action=retain reason=overflow before=${_rxAccum.length} after=${retained.length}');
      _diagInc('rx_retain_overflow');
      _diagMaybeDump('rx_retain_overflow');
      _rxAccum
        ..clear()
        ..addAll(retained);
      _rxAccumTimeout?.cancel();
      return;
    }
    _rxAccumTimeout?.cancel();
    _rxAccumTimeout = Timer(const Duration(seconds: 5), () {
      if (_rxAccum.isNotEmpty) {
        final retained = retainRxTailFromLastBraceBytes(_rxAccum, maxRetain: 2048);
        debugPrint(
          'RiftLinkBle: RX timeout, retaining tail ${retained.length} of ${_rxAccum.length} bytes',
        );
        _trace('stage=app_rx action=retain reason=timeout before=${_rxAccum.length} after=${retained.length}');
        _diagInc('rx_retain_timeout');
        _diagMaybeDump('rx_retain_timeout');
        _rxAccum
          ..clear()
          ..addAll(retained);
        _lastRxIncompleteLogLen = 0;
      }
    });
    _drainRxAccum();
  }

  /// База для частичных evt (`neighbors` / `routes` / `groups`): сначала [_compositeInfo], иначе [lastInfo] с непустым id.
  RiftLinkInfoEvent _baseForPartialMerge() {
    final c = _compositeInfo;
    if (c != null) return c;
    final l = _lastInfo;
    if (l != null && l.id.isNotEmpty) return l;
    return RiftLinkInfoEvent(id: '');
  }

  /// Склейка `evt:node` с предыдущими кусками (соседи/маршруты/группы остаются из кэша).
  RiftLinkInfoEvent _mergeFromNode(RiftLinkInfoEvent n) {
    final p = _compositeInfo ?? RiftLinkInfoEvent(id: n.id.isNotEmpty ? n.id : '');
    return RiftLinkInfoEvent(
      cmdId: n.cmdId ?? p.cmdId,
      id: n.id.isNotEmpty ? n.id : p.id,
      nickname: n.hasNicknameField ? n.nickname : p.nickname,
      hasNicknameField: p.hasNicknameField || n.hasNicknameField,
      hasChannelField: p.hasChannelField || n.hasChannelField,
      hasOfflinePendingField: p.hasOfflinePendingField || n.hasOfflinePendingField,
      hasOfflineCourierPendingField: p.hasOfflineCourierPendingField || n.hasOfflineCourierPendingField,
      hasOfflineDirectPendingField: p.hasOfflineDirectPendingField || n.hasOfflineDirectPendingField,
      region: n.region,
      freq: n.freq,
      power: n.power,
      channel: n.channel,
      version: n.version,
      radioMode: n.radioMode,
      radioVariant: n.radioVariant,
      wifiConnected: n.wifiConnected,
      wifiSsid: n.wifiSsid,
      wifiIp: n.wifiIp,
      neighbors: p.neighbors,
      neighborsRssi: p.neighborsRssi,
      neighborsHasKey: p.neighborsHasKey,
      neighborsBatMv: p.neighborsBatMv,
      groups: p.groups,
      routes: p.routes,
      sf: n.sf,
      bw: n.bw,
      cr: n.cr,
      modemPreset: n.modemPreset,
      offlinePending: n.offlinePending ?? p.offlinePending,
      offlineCourierPending: n.offlineCourierPending ?? p.offlineCourierPending,
      offlineDirectPending: n.offlineDirectPending ?? p.offlineDirectPending,
      batteryMv: n.batteryMv ?? p.batteryMv,
      batteryPercent: n.batteryPercent ?? p.batteryPercent,
      charging: n.charging,
      timeHour: n.timeHour ?? p.timeHour,
      timeMinute: n.timeMinute ?? p.timeMinute,
      gpsPresent: n.gpsPresent,
      gpsEnabled: n.gpsEnabled,
      gpsFix: n.gpsFix,
      powersave: n.powersave,
      blePin: n.blePin ?? p.blePin,
      espNowChannel: n.espNowChannel ?? p.espNowChannel,
      espNowAdaptive: n.espNowAdaptive,
      heapFreeBytes: n.heapFreeBytes ?? p.heapFreeBytes,
      heapTotalBytes: n.heapTotalBytes ?? p.heapTotalBytes,
      heapMinFreeBytes: n.heapMinFreeBytes ?? p.heapMinFreeBytes,
      cpuMhz: n.cpuMhz ?? p.cpuMhz,
      flashChipMb: n.flashChipMb ?? p.flashChipMb,
      appPartitionKb: n.appPartitionKb ?? p.appPartitionKb,
      nvsPartitionKb: n.nvsPartitionKb ?? p.nvsPartitionKb,
      nvsEntriesUsed: n.nvsEntriesUsed ?? p.nvsEntriesUsed,
      nvsEntriesFree: n.nvsEntriesFree ?? p.nvsEntriesFree,
      nvsEntriesTotal: n.nvsEntriesTotal ?? p.nvsEntriesTotal,
      nvsNamespaceCount: n.nvsNamespaceCount ?? p.nvsNamespaceCount,
    );
  }

  RiftLinkInfoEvent _mergeFromNeighbors(RiftLinkNeighborsEvent n) {
    final p = _baseForPartialMerge();
    final o = n.nodeOverlay;
    final k = n.jsonKeysPresent;
    bool has(String key) => k.contains(key);

    return RiftLinkInfoEvent(
      cmdId: o.cmdId ?? p.cmdId,
      id: o.id.isNotEmpty ? o.id : p.id,
      nickname: has('nickname') ? o.nickname : p.nickname,
      hasNicknameField: p.hasNicknameField || o.hasNicknameField,
      hasChannelField: has('channel') ? o.hasChannelField : p.hasChannelField,
      hasOfflinePendingField: has('offlinePending') ? o.hasOfflinePendingField : p.hasOfflinePendingField,
      hasOfflineCourierPendingField: has('offlineCourierPending') ? o.hasOfflineCourierPendingField : p.hasOfflineCourierPendingField,
      hasOfflineDirectPendingField: has('offlineDirectPending') ? o.hasOfflineDirectPendingField : p.hasOfflineDirectPendingField,
      region: has('region') ? o.region : p.region,
      freq: has('freq') ? o.freq : p.freq,
      power: has('power') ? o.power : p.power,
      channel: has('channel') ? o.channel : p.channel,
      version: has('version') ? o.version : p.version,
      radioMode: has('radioMode') ? o.radioMode : p.radioMode,
      radioVariant: has('radioVariant') ? o.radioVariant : p.radioVariant,
      wifiConnected: has('wifiConnected') ? o.wifiConnected : p.wifiConnected,
      wifiSsid: has('wifiSsid') ? o.wifiSsid : p.wifiSsid,
      wifiIp: has('wifiIp') ? o.wifiIp : p.wifiIp,
      neighbors: n.neighbors,
      neighborsRssi: n.rssi,
      neighborsHasKey: n.hasKey,
      neighborsBatMv: n.batMv,
      groups: p.groups,
      routes: p.routes,
      sf: has('sf') ? o.sf : p.sf,
      bw: has('bw') ? o.bw : p.bw,
      cr: has('cr') ? o.cr : p.cr,
      modemPreset: has('modemPreset') ? o.modemPreset : p.modemPreset,
      offlinePending: has('offlinePending') ? o.offlinePending : p.offlinePending,
      offlineCourierPending: has('offlineCourierPending') ? o.offlineCourierPending : p.offlineCourierPending,
      offlineDirectPending: has('offlineDirectPending') ? o.offlineDirectPending : p.offlineDirectPending,
      batteryMv: has('batteryMv') || has('battery') ? o.batteryMv : p.batteryMv,
      batteryPercent: has('batteryPercent') ? o.batteryPercent : p.batteryPercent,
      charging: has('charging') ? o.charging : p.charging,
      timeHour: has('timeHour') ? o.timeHour : p.timeHour,
      timeMinute: has('timeMinute') ? o.timeMinute : p.timeMinute,
      gpsPresent: has('gpsPresent') ? o.gpsPresent : p.gpsPresent,
      gpsEnabled: has('gpsEnabled') ? o.gpsEnabled : p.gpsEnabled,
      gpsFix: has('gpsFix') ? o.gpsFix : p.gpsFix,
      powersave: has('powersave') ? o.powersave : p.powersave,
      blePin: has('blePin') ? o.blePin : p.blePin,
      espNowChannel: has('espNowChannel') || has('espnowChannel') ? o.espNowChannel : p.espNowChannel,
      espNowAdaptive: has('espNowAdaptive') || has('espnowAdaptive') ? o.espNowAdaptive : p.espNowAdaptive,
      heapFreeBytes: has('heapFree') ? o.heapFreeBytes : p.heapFreeBytes,
      heapTotalBytes: has('heapTotal') ? o.heapTotalBytes : p.heapTotalBytes,
      heapMinFreeBytes: has('heapMin') ? o.heapMinFreeBytes : p.heapMinFreeBytes,
      cpuMhz: has('cpuMhz') ? o.cpuMhz : p.cpuMhz,
      flashChipMb: has('flashMb') ? o.flashChipMb : p.flashChipMb,
      appPartitionKb: has('appPartKb') ? o.appPartitionKb : p.appPartitionKb,
      nvsPartitionKb: has('nvsPartKb') ? o.nvsPartitionKb : p.nvsPartitionKb,
      nvsEntriesUsed: has('nvsUsedEnt') ? o.nvsEntriesUsed : p.nvsEntriesUsed,
      nvsEntriesFree: has('nvsFreeEnt') ? o.nvsEntriesFree : p.nvsEntriesFree,
      nvsEntriesTotal: has('nvsTotalEnt') ? o.nvsEntriesTotal : p.nvsEntriesTotal,
      nvsNamespaceCount: has('nvsNs') ? o.nvsNamespaceCount : p.nvsNamespaceCount,
    );
  }

  RiftLinkInfoEvent _mergeFromRoutes(RiftLinkRoutesEvent r) {
    final p = _baseForPartialMerge();
    return RiftLinkInfoEvent(
      cmdId: p.cmdId,
      id: p.id,
      nickname: p.nickname,
      hasNicknameField: p.hasNicknameField,
      hasChannelField: p.hasChannelField,
      hasOfflinePendingField: p.hasOfflinePendingField,
      hasOfflineCourierPendingField: p.hasOfflineCourierPendingField,
      hasOfflineDirectPendingField: p.hasOfflineDirectPendingField,
      region: p.region,
      freq: p.freq,
      power: p.power,
      channel: p.channel,
      version: p.version,
      radioMode: p.radioMode,
      radioVariant: p.radioVariant,
      wifiConnected: p.wifiConnected,
      wifiSsid: p.wifiSsid,
      wifiIp: p.wifiIp,
      neighbors: p.neighbors,
      neighborsRssi: p.neighborsRssi,
      neighborsHasKey: p.neighborsHasKey,
      neighborsBatMv: p.neighborsBatMv,
      groups: p.groups,
      routes: r.routes,
      sf: p.sf,
      bw: p.bw,
      cr: p.cr,
      modemPreset: p.modemPreset,
      offlinePending: p.offlinePending,
      offlineCourierPending: p.offlineCourierPending,
      offlineDirectPending: p.offlineDirectPending,
      batteryMv: p.batteryMv,
      batteryPercent: p.batteryPercent,
      charging: p.charging,
      timeHour: p.timeHour,
      timeMinute: p.timeMinute,
      gpsPresent: p.gpsPresent,
      gpsEnabled: p.gpsEnabled,
      gpsFix: p.gpsFix,
      powersave: p.powersave,
      blePin: p.blePin,
      espNowChannel: p.espNowChannel,
      espNowAdaptive: p.espNowAdaptive,
      heapFreeBytes: p.heapFreeBytes,
      heapTotalBytes: p.heapTotalBytes,
      heapMinFreeBytes: p.heapMinFreeBytes,
      cpuMhz: p.cpuMhz,
      flashChipMb: p.flashChipMb,
      appPartitionKb: p.appPartitionKb,
      nvsPartitionKb: p.nvsPartitionKb,
      nvsEntriesUsed: p.nvsEntriesUsed,
      nvsEntriesFree: p.nvsEntriesFree,
      nvsEntriesTotal: p.nvsEntriesTotal,
      nvsNamespaceCount: p.nvsNamespaceCount,
    );
  }

  /// Склеивает `evt:groupStatus` в [composite] (и [lastInfo]), пока не пришёл полный `evt:groups`.
  /// Иначе после `groupCreate` сначала приходит `groupStatus` (группа есть в UI), затем
  /// волна `evt:node` с пустым `groups:` — без этого слёта в [lastInfo] список остаётся пустым
  /// и [RiftLinkInfoEvent] затирает экран групп.
  RiftLinkInfoEvent _mergeFromGroupStatus(RiftLinkGroupStatusEvent s) {
    final p = _baseForPartialMerge();
    final gid = s.channelId32;
    if (gid == null || gid <= 1 || s.groupUid.trim().isEmpty) return p;

    final idx = p.groups.indexWhere(
      (g) => g.groupUid == s.groupUid || g.channelId32 == gid,
    );
    final prev = idx >= 0 ? p.groups[idx] : null;
    final keyVer =
        s.keyVersion > 0 ? s.keyVersion : (prev?.keyVersion ?? 0);
    final merged = RiftLinkGroupInfo(
      groupUid: s.groupUid,
      groupTag: (s.groupTag != null && s.groupTag!.isNotEmpty)
          ? s.groupTag!
          : (prev?.groupTag ?? ''),
      canonicalName: s.canonicalName.trim().isNotEmpty
          ? s.canonicalName
          : (prev?.canonicalName ?? ''),
      channelId32: gid,
      keyVersion: keyVer,
      myRole: s.myRole,
      revocationEpoch: prev?.revocationEpoch ?? 0,
      ackApplied: !s.rekeyRequired,
    ).mergedWithPrevious(prev);
    final nextGroups = List<RiftLinkGroupInfo>.from(p.groups);
    if (idx >= 0) {
      nextGroups[idx] = merged;
    } else {
      nextGroups.add(merged);
    }
    return RiftLinkInfoEvent(
      cmdId: p.cmdId,
      id: p.id,
      nickname: p.nickname,
      hasNicknameField: p.hasNicknameField,
      hasChannelField: p.hasChannelField,
      hasOfflinePendingField: p.hasOfflinePendingField,
      hasOfflineCourierPendingField: p.hasOfflineCourierPendingField,
      hasOfflineDirectPendingField: p.hasOfflineDirectPendingField,
      region: p.region,
      freq: p.freq,
      power: p.power,
      channel: p.channel,
      version: p.version,
      radioMode: p.radioMode,
      radioVariant: p.radioVariant,
      wifiConnected: p.wifiConnected,
      wifiSsid: p.wifiSsid,
      wifiIp: p.wifiIp,
      neighbors: p.neighbors,
      neighborsRssi: p.neighborsRssi,
      neighborsHasKey: p.neighborsHasKey,
      neighborsBatMv: p.neighborsBatMv,
      groups: nextGroups,
      routes: p.routes,
      sf: p.sf,
      bw: p.bw,
      cr: p.cr,
      modemPreset: p.modemPreset,
      offlinePending: p.offlinePending,
      offlineCourierPending: p.offlineCourierPending,
      offlineDirectPending: p.offlineDirectPending,
      batteryMv: p.batteryMv,
      batteryPercent: p.batteryPercent,
      charging: p.charging,
      timeHour: p.timeHour,
      timeMinute: p.timeMinute,
      gpsPresent: p.gpsPresent,
      gpsEnabled: p.gpsEnabled,
      gpsFix: p.gpsFix,
      powersave: p.powersave,
      blePin: p.blePin,
      espNowChannel: p.espNowChannel,
      espNowAdaptive: p.espNowAdaptive,
      heapFreeBytes: p.heapFreeBytes,
      heapTotalBytes: p.heapTotalBytes,
      heapMinFreeBytes: p.heapMinFreeBytes,
      cpuMhz: p.cpuMhz,
      flashChipMb: p.flashChipMb,
      appPartitionKb: p.appPartitionKb,
      nvsPartitionKb: p.nvsPartitionKb,
      nvsEntriesUsed: p.nvsEntriesUsed,
      nvsEntriesFree: p.nvsEntriesFree,
      nvsEntriesTotal: p.nvsEntriesTotal,
      nvsNamespaceCount: p.nvsNamespaceCount,
    );
  }

  RiftLinkInfoEvent _mergeFromGroups(RiftLinkGroupsEvent g) {
    final p = _baseForPartialMerge();
    // Узел: notifyInfo() шлёт подряд evt:node → neighbors → routes → groups; у первого
    // evt:groups поле cmdId часто отсутствует (ответ на волну info, не на cmd: groups).
    // Пустой массив без cmdId — подозрительный снимок (гонка/NVS) → не затираем кэш.
    // Пустой массив с cmdId > 0 — ответ на явный cmd: groups → принимаем «0 групп».
    // Неполный `myRole`/имена в очередном `evt:groups` не должны затирать кэш.
    final List<RiftLinkGroupInfo> mergedGroups;
    if (g.groups.isEmpty && p.groups.isNotEmpty) {
      final authoritativeEmpty = g.cmdId != null && g.cmdId! > 0;
      if (authoritativeEmpty) {
        mergedGroups = const [];
        _trace('stage=app_merge action=groups_accept_empty reason=cmd_id');
      } else {
        mergedGroups = p.groups;
        _trace('stage=app_merge action=groups_keep_nonempty reason=ignore_empty_snapshot');
      }
    } else if (g.groups.isNotEmpty && p.groups.isNotEmpty) {
      mergedGroups = _mergeGroupListWithPrevious(g.groups, p.groups);
    } else {
      mergedGroups = g.groups;
    }
    return RiftLinkInfoEvent(
      cmdId: p.cmdId,
      id: p.id,
      nickname: p.nickname,
      hasNicknameField: p.hasNicknameField,
      hasChannelField: p.hasChannelField,
      hasOfflinePendingField: p.hasOfflinePendingField,
      hasOfflineCourierPendingField: p.hasOfflineCourierPendingField,
      hasOfflineDirectPendingField: p.hasOfflineDirectPendingField,
      region: p.region,
      freq: p.freq,
      power: p.power,
      channel: p.channel,
      version: p.version,
      radioMode: p.radioMode,
      radioVariant: p.radioVariant,
      wifiConnected: p.wifiConnected,
      wifiSsid: p.wifiSsid,
      wifiIp: p.wifiIp,
      neighbors: p.neighbors,
      neighborsRssi: p.neighborsRssi,
      neighborsHasKey: p.neighborsHasKey,
      neighborsBatMv: p.neighborsBatMv,
      groups: mergedGroups,
      routes: p.routes,
      sf: p.sf,
      bw: p.bw,
      cr: p.cr,
      modemPreset: p.modemPreset,
      offlinePending: p.offlinePending,
      offlineCourierPending: p.offlineCourierPending,
      offlineDirectPending: p.offlineDirectPending,
      batteryMv: p.batteryMv,
      batteryPercent: p.batteryPercent,
      charging: p.charging,
      timeHour: p.timeHour,
      timeMinute: p.timeMinute,
      gpsPresent: p.gpsPresent,
      gpsEnabled: p.gpsEnabled,
      gpsFix: p.gpsFix,
      powersave: p.powersave,
      blePin: p.blePin,
      espNowChannel: p.espNowChannel,
      espNowAdaptive: p.espNowAdaptive,
      heapFreeBytes: p.heapFreeBytes,
      heapTotalBytes: p.heapTotalBytes,
      heapMinFreeBytes: p.heapMinFreeBytes,
      cpuMhz: p.cpuMhz,
      flashChipMb: p.flashChipMb,
      appPartitionKb: p.appPartitionKb,
      nvsPartitionKb: p.nvsPartitionKb,
      nvsEntriesUsed: p.nvsEntriesUsed,
      nvsEntriesFree: p.nvsEntriesFree,
      nvsEntriesTotal: p.nvsEntriesTotal,
      nvsNamespaceCount: p.nvsNamespaceCount,
    );
  }

  void _emitParsedJson(Map<String, dynamic> json) {
    _trace('stage=app_parse action=json evt=${json['evt']} keys=${json.keys.length}');
    // Сначала UI/prelisten, потом raw → TransportResponseRouter: иначе Completer tracked-команд
    // завершается до onPongEvent/onInfo и любые «ожидания» в виджетах гоняют с ответом.
    final copy = Map<String, dynamic>.from(json);
    RiftLinkEvent? evt;
    try {
      evt = _jsonToEvent(json);
    } catch (e, st) {
      debugPrint('RiftLinkBle: _jsonToEvent FAILED evt=${json['evt']}: $e\n$st');
      _trace('stage=app_parse action=parse_error evt=${json['evt']} reason=json_to_event err=$e');
      _diagInc('drop_json_to_event');
      _diagMaybeDump('drop_json_to_event');
      if (!_rawEventBus.isClosed) {
        _rawEventBus.add(copy);
      }
      return;
    }
    if (evt == null) {
      if (kDebugMode) {
        debugPrint(
          'RiftLinkBle: JSON без известного evt keys=${json.keys.toList()} evt=${json['evt']}',
        );
      }
      _trace('stage=app_parse action=drop reason=unknown_evt evt=${json['evt']}');
      _diagInc('drop_unknown_evt');
      _diagMaybeDump('drop_unknown_evt');
      if (!_rawEventBus.isClosed) {
        _rawEventBus.add(copy);
      }
      return;
    }

    final evtName = json['evt']?.toString() ?? '';
    if (evt is RiftLinkInfoEvent) {
      final hasTopology = evt.neighbors.isNotEmpty ||
          evt.routes.isNotEmpty ||
          evt.groups.isNotEmpty;
      if (evtName == 'info' && hasTopology) {
        final p = _compositeInfo;
        if (p != null && p.groups.isNotEmpty) {
          final infoAuthoritativeEmpty =
              evt.groups.isEmpty && evt.cmdId != null && evt.cmdId! > 0;
          if (evt.groups.isEmpty && !infoAuthoritativeEmpty) {
            _trace('stage=app_merge action=info_keep_groups reason=ignore_empty_groups_on_info');
            evt = _copyInfoReplacingGroups(evt, p.groups);
          } else if (evt.groups.isNotEmpty) {
            evt = _copyInfoReplacingGroups(
              evt,
              _mergeGroupListWithPrevious(evt.groups, p.groups),
            );
          }
        }
        // Монолитный evt:info часто без heap/flash/NVS — не затираем метрики предыдущего evt:node.
        evt = _mergeSystemMetricsFromPrevious(evt, p);
        _compositeInfo = evt;
      } else {
        _compositeInfo = _mergeFromNode(evt);
      }
      evt = _compositeInfo!;
    } else if (evt is RiftLinkNeighborsEvent) {
      _compositeInfo = _mergeFromNeighbors(evt);
      evt = _compositeInfo!;
    } else if (evt is RiftLinkRoutesEvent) {
      _compositeInfo = _mergeFromRoutes(evt);
      evt = _compositeInfo!;
    } else if (evt is RiftLinkGroupsEvent) {
      _compositeInfo = _mergeFromGroups(evt);
      evt = _compositeInfo!;
    } else if (evt is RiftLinkGroupStatusEvent) {
      _compositeInfo = _mergeFromGroupStatus(evt);
    }

    if (evt is RiftLinkInfoEvent) {
      _lastInfo = evt;
      _lastInfoEventAt = DateTime.now();
    } else if (evt is RiftLinkGroupStatusEvent) {
      if (_compositeInfo != null) {
        _lastInfo = _compositeInfo;
        _lastInfoEventAt = DateTime.now();
      }
    }
    if (evt is RiftLinkGroupStatusEvent && evt.inviteNoop) {
      _groupInviteAcceptNoop = true;
    }

    if (!_eventBus.isClosed) {
      if (_eventsStreamListeners == 0) {
        _preListenBuffer.add(evt);
        _trace('stage=app_event_bus action=prelisten_add evt=${evt.runtimeType} size=${_preListenBuffer.length}');
        if (_preListenBuffer.length > _maxPreListenBuffer) {
          _preListenBuffer.removeAt(0);
          _trace('stage=app_event_bus action=prelisten_drop reason=overflow limit=$_maxPreListenBuffer');
          _diagInc('prelisten_drop');
          _diagMaybeDump('prelisten_drop');
        }
      } else {
        _trace('stage=app_event_bus action=emit evt=${evt.runtimeType} listeners=$_eventsStreamListeners');
        _eventBus.add(evt);
      }
    }
    if (!_rawEventBus.isClosed) {
      _rawEventBus.add(copy);
    }
  }

  void _stripToFirstAsciiBraceByte() {
    final i = _rxAccum.indexWhere((b) => b == 0x7B);
    if (i < 0) {
      // Фрагмент без `{` — ждём следующий notify (склейка по MTU), не затираем короткий буфер.
      if (_rxAccum.length < 4096) return;
      _rxAccum.clear();
      return;
    }
    if (i > 0) _rxAccum.removeRange(0, i);
  }

  /// Сначала строгий UTF-8; при обрезке чанка по середине символа — lenient (без вечного «null»).
  String _decodeUtf8Lenient(List<int> bytes) {
    if (bytes.isEmpty) return '';
    try {
      return utf8.decode(bytes, allowMalformed: false);
    } catch (_) {
      return utf8.decode(bytes, allowMalformed: true);
    }
  }

  /// NDJSON: после каждого JSON на прошивке — `\n` (или `\r\n`).
  ///
  /// Важно: префикс до последнего `0x0A` режем **только по байтам** [raw], декодируем его в строку
  /// и парсим строки. Нельзя брать `s.lastIndexOf('\\n')` по полному [utf8.decode] буфера: индекс
  /// символа в [String] не совпадает с байтовым смещением при кириллице в JSON — тогда
  /// `removeRange` по `lastIndexOf(0x0A)` съедал не тот префикс и первым эмитился evt:routes.
  int? _tryDrainNewlineDelimitedJsonBytes(List<int> raw) {
    final lastNl = raw.lastIndexOf(0x0A);
    if (lastNl < 0) return null;
    final completeBytes = raw.sublist(0, lastNl + 1);
    final text = utf8.decode(completeBytes, allowMalformed: true);
    for (final line in text.split('\n')) {
      final lineNoCr = line.trim().replaceAll('\r', '');
      if (lineNoCr.isEmpty) continue;
      try {
        final decoded = jsonDecode(lineNoCr);
        if (decoded is Map) {
          final m = Map<String, dynamic>.from(decoded as Map);
          if (m.containsKey('evt')) {
            _lastRxIncompleteLogLen = 0;
            _emitParsedJson(m);
          }
        }
      } catch (e) {
        // Несколько корневых JSON подряд без \n между объектами: `}{` — jsonDecode падает,
        // иначе теряются evt:node / neighbors, остаётся только следующая строка (routes).
        var emitted = 0;
        final remainder = _tryEmitFromConcatenated(lineNoCr, (Map<String, dynamic> m) {
          emitted++;
          _lastRxIncompleteLogLen = 0;
          _emitParsedJson(m);
        });
        if (emitted == 0) {
          if (kDebugMode) debugPrint('RiftLinkBle: NDJSON parse error: $e');
          _trace('stage=app_parse action=parse_error reason=ndjson err=$e');
          _diagInc('drop_parse_ndjson');
        } else if (remainder.trim().isNotEmpty) {
          _trace('stage=app_parse action=ndjson_concat_tail len=${remainder.length}');
        }
      }
    }
    return lastNl + 1;
  }

  /// Разбор RX: отрезка мусора до первого `{` по **байтам** (не ждём целого UTF-8),
  /// затем decode с lenient для границ чанков внутри многобайтовых символов.
  /// Далее — один объект jsonDecode или несколько подряд по скобкам (осторожно с `\"` в строках).
  void _drainRxAccum() {
    for (var iter = 0; iter < 32; iter++) {
      if (_rxAccum.isEmpty) return;
      _stripToFirstAsciiBraceByte();
      if (_rxAccum.isEmpty) return;

      final s = _decodeUtf8Lenient(_rxAccum);
      final i0 = s.indexOf('{');
      if (i0 < 0) {
        if (_rxAccum.length < 4096) return;
        _rxAccum.clear();
        return;
      }
      if (i0 > 0) {
        // Keep raw bytes to avoid re-encoding/truncating malformed UTF-8 tails.
        final firstBraceByte = _rxAccum.indexWhere((b) => b == 0x7B);
        if (firstBraceByte <= 0) {
          _rxAccum.clear();
        } else {
          _rxAccum.removeRange(0, firstBraceByte);
        }
        continue;
      }

      final t = s;
      // Склейка двух JSON без \n между ними: `}{`. NDJSON по lastIndexOf(0x0A) тогда может
      // съесть не тот префикс (старый \n в буфере) — сначала разбираем concat, не NDJSON.
      var skipNdjson = false;
      for (var i = 0; i + 1 < _rxAccum.length; i++) {
        if (_rxAccum[i] == 0x7D && _rxAccum[i + 1] == 0x7B) {
          skipNdjson = true;
          break;
        }
      }
      final ndConsumed = skipNdjson ? null : _tryDrainNewlineDelimitedJsonBytes(_rxAccum);
      if (ndConsumed != null) {
        _rxAccum.removeRange(0, ndConsumed);
        continue;
      }

      // Сначала склейка нескольких корневых JSON (`}{` без \n). Если раньше вызывать jsonDecode(t) на
      // всём буфере, один полный короткий объект (например routes) очищает _rxAccum и теряется хвост волны info.
      final tail = _tryEmitFromConcatenated(s, _emitParsedJson);
      if (tail.length < s.length) {
        _lastRxIncompleteLogLen = 0;
        final prefix = s.substring(0, s.length - tail.length);
        final off = _byteOffsetForDecodedPrefixMatch(_rxAccum, s, prefix);
        if (off != null) {
          _rxAccum.removeRange(0, off);
        } else {
          _trace('stage=app_rx action=rx_tail_fallback reason=byte_prefix_mismatch tail_len=${tail.length} raw_len=${_rxAccum.length}');
          _diagInc('rx_tail_byte_mismatch');
          _rxAccum
            ..clear()
            ..addAll(utf8.encode(tail));
        }
        if (tail.isEmpty) return;
        continue;
      }

      // Один полный объект в буфере (редкий путь, если concat ничего не извлёк).
      try {
        final decoded = jsonDecode(t);
        if (decoded is Map) {
          _lastRxIncompleteLogLen = 0;
          _emitParsedJson(Map<String, dynamic>.from(decoded as Map));
          _rxAccum.clear();
          return;
        }
        if (decoded is List) {
          _lastRxIncompleteLogLen = 0;
          for (final e in decoded) {
            if (e is Map) {
              _emitParsedJson(Map<String, dynamic>.from(e as Map));
            }
          }
          _rxAccum.clear();
          return;
        }
        if (kDebugMode) {
          debugPrint('RiftLinkBle: RX json root is ${decoded.runtimeType}, trying brace extract');
        }
      } catch (e) {
        if (kDebugMode && s.length < 256) debugPrint('RiftLinkBle: single-object parse: $e');
        final isFragment =
            s.length > 120 &&
            e is FormatException &&
            (e.message?.contains('Unterminated') == true || e.message?.contains('Unexpected end') == true);
        if (!isFragment) {
          _trace('stage=app_parse action=single_object_retry err=$e len=${s.length}');
        }
      }
      if (kDebugMode && t.length >= 64) {
        if (t.length - _lastRxIncompleteLogLen >= 300 || _lastRxIncompleteLogLen == 0) {
          _lastRxIncompleteLogLen = t.length;
          debugPrint(
            'RiftLinkBle: RX no complete JSON yet len=${t.length} (waiting for more notify) head=${_rxDebugPreview(t)}',
          );
        }
      }
      if (_rxAccum.length > 4096) {
        final retained = retainRxTailFromLastBraceBytes(_rxAccum, maxRetain: 4096);
        _trace('stage=app_rx action=retain reason=large_partial before=${_rxAccum.length} after=${retained.length}');
        _diagInc('rx_retain_large_partial');
        _diagMaybeDump('rx_retain_large_partial');
        _rxAccum
          ..clear()
          ..addAll(retained);
      }
      return;
    }
  }

  Future<void> _stopRxDispatcher() async {
    final old = _rxSub;
    _rxSub = null;
    await old?.cancel();
    _rxAccum.clear();
    _lastRxIncompleteLogLen = 0;
    _trace('stage=app_rx action=dispatcher_stopped');
  }

  /// Отправка геолокации (broadcast), опционально с geofence и expiry.
  Future<bool> sendLocation({
    required double lat,
    required double lon,
    int alt = 0,
    int radiusM = 0,
    int? expiryEpochSec,
  }) async => _sendCmd({
        'cmd': 'location',
        'lat': lat,
        'lon': lon,
        'alt': alt,
        if (radiusM > 0) 'radiusM': radiusM,
        if (expiryEpochSec != null && expiryEpochSec > 0) 'expiryEpochSec': expiryEpochSec,
      });

  /// GPS sync от телефона: UTC ms, lat, lon, alt — для beacon-sync (устройство без GPS)
  Future<bool> sendGpsSync({required int utcMs, required double lat, required double lon, int alt = 0}) async =>
      utcMs != 0 ? _sendCmd({'cmd': 'gps_sync', 'utc_ms': utcMs, 'lat': lat, 'lon': lon, 'alt': alt}) : Future.value(false);

  /// Установить регион (EU, RU, UK, US, AU)
  Future<bool> setRegion(String region) async => _requestCommand(
        cmd: 'region',
        payload: {'region': region},
        expectedEvents: const {'region'},
      );

  /// Установить никнейм (до 16 символов)
  Future<bool> setNickname(String nickname) async {
    if (utf8.encode(nickname).length > 32) return false;
    return _requestCommand(
      cmd: 'nickname',
      payload: {'nickname': nickname},
      expectedEvents: const {'node'},
      timeout: const Duration(seconds: 6),
    );
  }

  /// Установить канал (0–2) для EU/UK
  Future<bool> setChannel(int channel) async {
    if (channel < 0 || channel > 2) return false;
    return _requestCommand(
      cmd: 'channel',
      payload: {'channel': channel},
      expectedEvents: const {'region'},
      timeout: const Duration(seconds: 6),
    );
  }

  /// LoRa spreading factor (mesh), 7–12
  Future<bool> setSpreadingFactor(int sf) async {
    if (sf < 7 || sf > 12) return false;
    return _requestCommand(
      cmd: 'sf',
      payload: {'sf': sf},
      expectedEvents: const {'node'},
      timeout: const Duration(seconds: 6),
    );
  }

  /// Modem preset (0=Speed, 1=Normal, 2=Range, 3=MaxRange)
  Future<bool> setModemPreset(int preset) async {
    if (preset < 0 || preset > 3) return false;
    return _requestCommand(
      cmd: 'modemPreset',
      payload: {'preset': preset},
      expectedEvents: const {'node'},
      timeout: const Duration(seconds: 6),
    );
  }

  /// Custom modem: SF 7–12, BW kHz (62.5/125/250/500), CR 5–8
  Future<bool> setCustomModem(int sf, double bw, int cr) async {
    if (sf < 7 || sf > 12 || cr < 5 || cr > 8) return false;
    return _requestCommand(
      cmd: 'modemCustom',
      payload: {'sf': sf, 'bw': bw, 'cr': cr},
      expectedEvents: const {'node'},
      timeout: const Duration(seconds: 6),
    );
  }

  /// Отправить голосовое сообщение (Opus/AAC, base64 чанками)
  Future<bool> sendVoice({required String to, required List<String> chunks}) async {
    if (!isValidFullNodeId(to) || chunks.isEmpty) return false;
    for (var i = 0; i < chunks.length; i++) {
      final ok = await _sendCmd({
        'cmd': 'voice',
        'to': to,
        'chunk': i,
        'total': chunks.length,
        'data': chunks[i],
      });
      if (!ok) return false;
    }
    return true;
  }

  /// Отправить подтверждение прочтения (unicast)
  Future<bool> sendRead({required String from, required int msgId}) async {
    if (!isValidFullNodeId(from) || msgId == 0) return false;
    return _sendCmd({'cmd': 'read', 'from': from, 'msgId': msgId});
  }

  /// Запросить маршруты (evt "routes")
  Future<bool> getRoutes() async => _requestCommand(
        cmd: 'routes',
        expectedEvents: const {'routes'},
        timeout: const Duration(seconds: 5),
        sendAttempts: 3,
      );

  /// Запросить список групп
  Future<bool> getGroups() async =>
      _requestCommand(cmd: 'groups', expectedEvents: const {'groups'}, timeout: const Duration(seconds: 5));

  // --- Groups V2 (thin-device, no-legacy) ---

  bool _isValidGroupUid(String groupUid) => groupUid.trim().isNotEmpty;

  Future<bool> groupCreate({
    required String groupUid,
    required String displayName,
    required int channelId32,
    required String groupTag,
  }) async {
    _lastGroupSecurityError = null;
    if (!_isValidGroupUid(groupUid) || displayName.trim().isEmpty || channelId32 <= 1 || groupTag.trim().isEmpty) {
      return false;
    }
    return _requestCommand(
      cmd: 'groupCreate',
      payload: {
        'groupUid': groupUid.trim(),
        'displayName': displayName.trim(),
        'channelId32': channelId32,
        'groupTag': groupTag.trim(),
      },
      expectedEvents: const {'groupStatus'},
      timeout: const Duration(seconds: 6),
    );
  }

  /// Создать инвайт и вернуть строку для буфера обмена (тело ответа `evt: groupInvite`).
  Future<String?> groupInviteCreateInvite({
    required String groupUid,
    required String role,
    int ttlSec = 600,
  }) async {
    if (!_isValidGroupUid(groupUid) || role.trim().isEmpty || ttlSec <= 0) return null;
    try {
      final resp = await _responseRouter.sendRequest(
        cmd: 'groupInviteCreate',
        payload: {
          'groupUid': groupUid.trim(),
          'role': role.trim(),
          'ttlSec': ttlSec,
        },
        expectedEvents: const {'groupInvite'},
        timeout: const Duration(seconds: 6),
      );
      final s = resp['invite']?.toString() ?? '';
      return s.isEmpty ? null : s;
    } catch (e) {
      _trace('stage=app_rr action=group_invite_payload_fail err=$e');
      return null;
    }
  }

  Future<bool> groupInviteCreate({
    required String groupUid,
    required String role,
    int ttlSec = 600,
  }) async {
    final invite = await groupInviteCreateInvite(groupUid: groupUid, role: role, ttlSec: ttlSec);
    return invite != null;
  }

  Future<bool> groupInviteAccept(String invitePayload) async {
    _groupInviteAcceptNoop = false;
    final normalized = normalizeGroupInvitePayload(invitePayload);
    if (normalized.isEmpty) return false;
    return _requestCommand(
      cmd: 'groupInviteAccept',
      payload: {'invite': normalized},
      expectedEvents: const {'groupStatus'},
      timeout: const Duration(seconds: 6),
    );
  }

  Future<bool> groupGrantIssue({
    required String groupUid,
    required String subjectId,
    required String role,
    int? expiresAt,
  }) async {
    if (!_isValidGroupUid(groupUid) || !isValidFullNodeId(subjectId) || role.trim().isEmpty) return false;
    return _requestCommand(
      cmd: 'groupGrantIssue',
      payload: {
        'groupUid': groupUid.trim(),
        'subjectId': subjectId.toUpperCase(),
        'role': role.trim(),
        if (expiresAt != null && expiresAt > 0) 'expiresAt': expiresAt,
      },
      expectedEvents: const {'groupStatus'},
      timeout: const Duration(seconds: 6),
    );
  }

  Future<bool> groupRevoke({
    required String groupUid,
    required String subjectId,
    String? reason,
  }) async {
    if (!_isValidGroupUid(groupUid) || !isValidFullNodeId(subjectId)) return false;
    return _requestCommand(
      cmd: 'groupRevoke',
      payload: {
        'groupUid': groupUid.trim(),
        'subjectId': subjectId.toUpperCase(),
        if (reason != null && reason.trim().isNotEmpty) 'reason': reason.trim(),
      },
      expectedEvents: const {'groupStatus'},
      timeout: const Duration(seconds: 6),
    );
  }

  Future<bool> groupRekey({
    required String groupUid,
    String? reason,
  }) async {
    if (!_isValidGroupUid(groupUid)) return false;
    return _requestCommand(
      cmd: 'groupRekey',
      payload: {
        'groupUid': groupUid.trim(),
        if (reason != null && reason.trim().isNotEmpty) 'reason': reason.trim(),
      },
      expectedEvents: const {'groupRekeyProgress'},
      timeout: const Duration(seconds: 8),
    );
  }

  Future<bool> groupAckKeyApplied({
    required String groupUid,
    required int keyVersion,
  }) async {
    if (!_isValidGroupUid(groupUid) || keyVersion <= 0) return false;
    return _requestCommand(
      cmd: 'groupAckKeyApplied',
      payload: {
        'groupUid': groupUid.trim(),
        'keyVersion': keyVersion,
      },
      expectedEvents: const {'groupMemberKeyState', 'groupStatus'},
      timeout: const Duration(seconds: 6),
    );
  }

  Future<bool> groupStatus(String groupUid) async {
    if (!_isValidGroupUid(groupUid)) return false;
    return _requestCommand(
      cmd: 'groupStatus',
      payload: {'groupUid': groupUid.trim()},
      expectedEvents: const {'groupStatus'},
      timeout: const Duration(seconds: 5),
    );
  }

  Future<bool> groupCanonicalRename({
    required String groupUid,
    required String canonicalName,
  }) async {
    if (!_isValidGroupUid(groupUid) || canonicalName.trim().isEmpty) return false;
    return _requestCommand(
      cmd: 'groupCanonicalRename',
      payload: {
        'groupUid': groupUid.trim(),
        'canonicalName': canonicalName.trim(),
      },
      expectedEvents: const {'groupStatus'},
      timeout: const Duration(seconds: 6),
    );
  }

  Future<bool> groupSyncSnapshot(List<Map<String, dynamic>> groups) async {
    if (groups.isEmpty) return false;
    return _requestCommand(
      cmd: 'groupSyncSnapshot',
      payload: {'groups': groups},
      expectedEvents: const {'groups'},
      timeout: const Duration(seconds: 6),
    );
  }

  Future<bool> groupLeave({required String groupUid}) async {
    final u = groupUid.trim();
    if (!_isValidGroupUid(u)) return false;
    // Плейсхолдер из локальной БД (сообщения без groupUid в evt) — не отправлять на узел.
    if (u.startsWith('UNRESOLVED_')) return false;
    return _requestCommand(
      cmd: 'groupLeave',
      payload: {'groupUid': u},
      expectedEvents: const {'groups'},
      timeout: const Duration(seconds: 6),
    );
  }

  /// Отправить PING на узел (проверка связи)
  Future<bool> sendPing(String to) async => (await sendPingTracked(to)) != null;

  /// Отправка ping с возвратом [cmdId] для сопоставления с [RiftLinkPongEvent.cmdId];
  /// в [RiftLinkPongEvent.pingPktId] — эхо `pktId` кадра OP_PING в эфире (если не 0).
  /// Ждёт короткое окно, чтобы отловить [request_send_failed] (BLE не принял команду);
  /// иначе считаем, что TX ушёл и ждём pong по радио (до таймаута роутера).
  ///
  /// **Не путать с доставкой сообщения:** у личного MSG — очередь, повторы по таймауту ACK,
  /// ACK/NACK; у PING/PONG — два лёгких кадра без того же уровня подтверждения — статистика
  /// «дошло / не дошло» может отличаться.
  Future<int?> sendPingTracked(String to) async {
    if (!isValidFullNodeId(to)) return null;
    final ticket = _responseRouter.sendTrackedRequest(
      cmd: 'ping',
      payload: <String, dynamic>{'to': to},
      expectedEvents: const {'pong'},
      timeout: const Duration(seconds: 20),
      sendAttempts: 3,
    );
    try {
      // Достаточно для ответа «команда не записалась»; при занятой очереди BLE подождём дольше, чем мгновенный fail.
      await ticket.response.timeout(const Duration(milliseconds: 650));
    } on TimeoutException {
      // Ранний выход: ответ по эфиру может прийти через секунды; роутер всё ещё ждёт до [timeout].
      // Иначе через ~20 с completer завершится с TimeoutException без слушателя → необработанная ошибка в Zone.
      unawaited(ticket.response.catchError((_) => <String, dynamic>{}));
      return ticket.cmdId;
    } catch (e) {
      final s = e.toString();
      if (s.contains('request_send_failed')) return null;
      unawaited(ticket.response.catchError((_) => <String, dynamic>{}));
      return ticket.cmdId;
    }
    return ticket.cmdId;
  }

  // --- Radio Mode Switching (Time-sharing BLE ↔ WiFi) ---

  WifiTransport? _wifiTransport;
  StreamSubscription? _wifiRxSub;
  bool _isWifiMode = false;

  /// Текущий радио-режим: true = WiFi, false = BLE
  bool get isWifiMode => _isWifiMode;

  String? _wifiIp;
  /// IP address used for the current WiFi connection (null if BLE).
  String? get wifiIp => _wifiIp;

  /// Последний успешный IP WebSocket (`connectWifi`), не сбрасывается при [abandonWifiSession].
  /// Нужен [TransportReconnectManager], чтобы после обрыва снова вызывать `connectWifi`, а не BLE.
  String? _wifiReconnectIp;
  String? get lastWifiReconnectIp => _wifiReconnectIp;

  /// Переключить в WiFi STA-режим (подключение к сети)
  ///
  /// Узел гасит BLE — без подавления [TransportReconnectManager] сработает ложный «связь потеряна».
  /// Возобновление авто‑reconnect: успешный [connectWifi] или ручное [connect] по BLE.
  Future<bool> switchToWifiSta({required String ssid, required String pass}) async {
    transportReconnectManager?.suppressAutoReconnectUntilNextConnection();
    return _sendCmd({'cmd': 'radioMode', 'mode': 'wifi', 'variant': 'sta', 'ssid': ssid, 'pass': pass});
  }

  /// Переключить обратно в BLE
  Future<bool> switchToBle() async {
    // Разрыв WebSocket при уходе узла с Wi‑Fi — тоже не считаем «потерей» до следующего connect().
    transportReconnectManager?.suppressAutoReconnectUntilNextConnection();
    // Важно: radioMode устройства и локальный флаг _isWifiMode не эквивалентны.
    // Команду нужно отправлять всегда, иначе UI может думать, что режим сменился, а устройство останется в WiFi.
    final ok = await _sendCmd({'cmd': 'radioMode', 'mode': 'ble'});
    if (ok && _isWifiMode) {
      await _disconnectWifi();
      _isWifiMode = false;
    }
    return ok;
  }

  /// После получения evt:wifi_ready с IP, подключиться по WebSocket
  Future<bool> connectWifi(String ip) async {
    _wifiTransport = WifiTransport();
    final ok = await _wifiTransport!.connectToDevice(ip);
    if (!ok) {
      _wifiTransport = null;
      return false;
    }
    _isWifiMode = true;
    _wifiIp = ip;
    _wifiReconnectIp = ip;
    _wifiTransport!.onConnectionLost = _onWifiSocketLost;
    // Тот же путь, что и BLE: склейка чанков, NDJSON и извлечение по скобкам — иначе Wi‑Fi ломается
    // на длинных evt/фрагментах и на нескольких JSON подряд без разделителя.
    _rxAccum.clear();
    _rxAccumTimeout?.cancel();
    _wifiRxSub = _wifiTransport!.rawJsonStream.listen(
      (raw) {
        _trace('stage=app_rx action=ws_chunk mode=wifi len=${raw.length}');
        _feedRxChunk(utf8.encode(raw));
      },
      onError: (Object e, StackTrace st) {
        if (kDebugMode) debugPrint('RiftLinkBle: WiFi stream error: $e\n$st');
        _trace('stage=app_rx action=error mode=wifi err=$e');
      },
    );
    // После BLE недавний getInfo(force) попадает под forceMinGap 900 мс и откладывает реальный
    // запрос таймером — по новому WebSocket cmd:info не уходит, в UI «нет данных».
    _queuedInfoTimer?.cancel();
    _queuedInfoTimer = null;
    _hasQueuedInfoRequest = false;
    _lastInfoRequestAt = null;
    // Wi-Fi transport does not push initial state automatically like BLE connect flow.
    // Request baseline payload right after WS attach (чуть больше задержки — очередь cmd на узле после смены режима).
    await Future<void>.delayed(const Duration(milliseconds: 280));
    var infoOk = await getInfo(force: true);
    if (!infoOk) {
      await Future<void>.delayed(const Duration(milliseconds: 900));
      await getInfo(force: true);
    }
    await Future<void>.delayed(const Duration(milliseconds: 80));
    await getGroups();
    await getRoutes();
    unawaited(
      Future<void>.delayed(const Duration(milliseconds: 450), () {
        transportReconnectManager?.resumeAutoReconnect();
      }),
    );
    return true;
  }

  /// Обрыв WebSocket (узел ушёл с Wi‑Fi, TCP закрыт) — без этого UI ждёт только таймаут cmd (например info).
  void _onWifiSocketLost() => _teardownWifiLink('socket_lost');

  /// Watchdog / полуоткрытый TCP: принудительно сбросить Wi‑Fi‑сессию (как потерю связи).
  /// Иначе [TransportReconnectManager] видит «transport connected» до минут TCP и не показывает overlay.
  void abandonWifiSession() => _teardownWifiLink('abandon_session');

  void _teardownWifiLink(String action) {
    if (!_isWifiMode) return;
    _trace('stage=app_wifi action=$action');
    _wifiTransport?.onConnectionLost = null;
    _responseRouter.cancelAll();
    unawaited(_wifiRxSub?.cancel() ?? Future<void>.value());
    _wifiRxSub = null;
    final t = _wifiTransport;
    _wifiTransport = null;
    _isWifiMode = false;
    final lastIp = _wifiIp;
    _wifiIp = null;
    if (!_eventBus.isClosed) {
      _eventBus.add(RiftLinkWifiEvent(connected: false, ssid: '', ip: lastIp ?? ''));
    }
    unawaited(t?.disconnect() ?? Future<void>.value());
  }

  Future<void> _disconnectWifi() async {
    _wifiTransport?.onConnectionLost = null;
    await _wifiRxSub?.cancel();
    _wifiRxSub = null;
    _rxAccumTimeout?.cancel();
    _rxAccum.clear();
    await _wifiTransport?.disconnect();
    _wifiTransport = null;
  }

  /// BLE OTA: начать OTA обновление
  Future<bool> startBleOta({required int size, String? md5}) =>
      _sendCmd({'cmd': 'bleOtaStart', 'size': size, if (md5 != null) 'md5': md5});

  /// BLE OTA: отправить чанк бинарных данных
  Future<bool> sendBleOtaChunk(List<int> data) async {
    // WiFi transport: send OTA chunk as base64 JSON command.
    if (_isWifiMode && _wifiTransport != null && _wifiTransport!.isConnected) {
      return _wifiTransport!.sendJson(
        jsonEncode(<String, dynamic>{
          'cmd': 'bleOtaChunk',
          'data': base64Encode(data),
        }),
      );
    }
    if (_txChar == null || !isConnected) return false;
    return _writeBleBytesSerialized(
      data,
      traceOnSuccess: 'stage=app_tx action=send mode=ble cmd=bleOtaChunk len=${data.length} ok=true',
      traceOnFailure: (e) => 'stage=app_tx action=send mode=ble cmd=bleOtaChunk len=${data.length} ok=false err=$e',
    );
  }

  /// BLE OTA: завершить OTA
  Future<bool> endBleOta() => _sendCmd({'cmd': 'bleOtaEnd'});

  /// BLE OTA: отменить OTA
  Future<bool> abortBleOta() => _sendCmd({'cmd': 'bleOtaAbort'});

  /// Выключить устройство (deep sleep, пробуждение кнопкой)
  Future<bool> shutdown() => _sendCmd({'cmd': 'shutdown'});

  /// Тест сигнала: пинг всех соседей (ответы приходят как evt:pong)
  Future<bool> signalTest() async {
    for (var attempt = 0; attempt < 3; attempt++) {
      if (attempt > 0) await Future<void>.delayed(const Duration(milliseconds: 100));
      if (await _sendCmd({'cmd': 'signalTest'})) return true;
    }
    return false;
  }

  /// Трассировка: запрос маршрута до узла (ответ evt:routes)
  Future<bool> traceroute(String to) async =>
      !isValidFullNodeId(to)
          ? false
          : _requestCommand(
              cmd: 'traceroute',
              payload: {'to': to},
              expectedEvents: const {'routes'},
              timeout: const Duration(seconds: 18),
              sendAttempts: 3,
            );

  /// ESP-NOW: канал 1..13 (для WiFi-режима)
  Future<bool> setEspNowChannel(int channel) async {
    if (channel < 1 || channel > 13) return false;
    return _requestCommand(
      cmd: 'espnowChannel',
      payload: {'channel': channel},
      expectedEvents: const {'node'},
      timeout: const Duration(seconds: 6),
    );
  }

  /// ESP-NOW: адаптивный подбор канала
  Future<bool> setEspNowAdaptive(bool enabled) async =>
      _requestCommand(
        cmd: 'espnowAdaptive',
        payload: {'enabled': enabled},
        expectedEvents: const {'node'},
        timeout: const Duration(seconds: 6),
      );

  /// WiFi: SSID + пароль (переключает в WiFi STA)
  Future<bool> setWifi({required String ssid, required String pass}) async =>
      switchToWifiSta(ssid: ssid, pass: pass);

  /// GPS: вкл/выкл
  Future<bool> setGps(bool enabled) async => _requestCommand(
        cmd: 'gps',
        payload: {'enabled': enabled},
        expectedEvents: const {'gps'},
        timeout: const Duration(seconds: 5),
      );

  /// Powersave
  Future<bool> setPowersave(bool enabled) async =>
      _sendCmd({'cmd': 'powersave', 'enabled': enabled});

  /// Язык прошивки (ru/en)
  Future<bool> setLang(String lang) async =>
      _sendCmd({'cmd': 'lang', 'lang': lang});

  /// Создать E2E invite (evt "invite"), TTL ограничен на устройстве.
  Future<bool> createInvite({int ttlSec = 600}) async => _requestCommand(
        cmd: 'invite',
        payload: {'ttlSec': ttlSec},
        expectedEvents: const {'invite'},
        timeout: const Duration(seconds: 6),
      );

  /// Принять invite (id + pubKey base64, опционально channelKey/inviteToken).
  Future<bool> acceptInvite({
    required String id,
    required String pubKey,
    String? channelKey,
    String? inviteToken,
  }) async {
    if (!isValidFullNodeId(id) || pubKey.trim().isEmpty) return false;
    final payload = <String, dynamic>{'id': id, 'pubKey': pubKey};
    if (channelKey != null && channelKey.isNotEmpty) payload['channelKey'] = channelKey;
    if (inviteToken != null && inviteToken.isNotEmpty) payload['inviteToken'] = inviteToken;
    return _requestCommand(
      cmd: 'acceptInvite',
      payload: payload,
      expectedEvents: const {'node'},
      timeout: const Duration(seconds: 6),
    );
  }

  /// Перегенерировать BLE PIN (passkey) — устройство покажет новый PIN на экране
  Future<bool> regeneratePin() async => _requestCommand(
    cmd: 'regeneratePin',
    expectedEvents: const {'node'},
    timeout: const Duration(seconds: 5),
  );

  /// Selftest (evt "selftest")
  Future<bool> selftest() async =>
      _requestCommand(cmd: 'selftest', expectedEvents: const {'selftest'}, timeout: const Duration(seconds: 15));

  /// Отправка сообщения (broadcast, unicast или в группу).
  Future<bool> send({
    String? to,
    int? group,
    required String text,
    int ttlMinutes = 0,
    String lane = 'normal',
    String? trigger,
    int? triggerAtMs,
  }) async {
    final Map<String, dynamic>? payload = group != null && group > 1
        ? {
            'cmd': 'send',
            'group': group,
            'text': text,
            if (ttlMinutes > 0) 'ttl': ttlMinutes,
            if (lane != 'normal') 'lane': lane,
            if (trigger != null && trigger.isNotEmpty) 'trigger': trigger,
            if (triggerAtMs != null) 'triggerAtMs': triggerAtMs,
          }
        : to != null
            ? (!isValidFullNodeId(to)
                ? null
                : {
                    'cmd': 'send',
                    'to': to,
                    'text': text,
                    if (ttlMinutes > 0) 'ttl': ttlMinutes,
                    if (lane != 'normal') 'lane': lane,
                    if (trigger != null && trigger.isNotEmpty) 'trigger': trigger,
                    if (triggerAtMs != null) 'triggerAtMs': triggerAtMs,
                  })
            : {
                'cmd': 'send',
                'text': text,
                if (ttlMinutes > 0) 'ttl': ttlMinutes,
                if (lane != 'normal') 'lane': lane,
                if (trigger != null && trigger.isNotEmpty) 'trigger': trigger,
                if (triggerAtMs != null) 'triggerAtMs': triggerAtMs,
              };
    if (payload == null) {
      if (to != null) {
        _trace(
          'stage=app_tx action=drop reason=invalid_recipient mode=ble to=$to '
          '(expected 16 hex chars)',
        );
      }
      return false;
    }
    return _sendCmd(payload);
  }

  /// Emergency flood
  Future<bool> sendSos({String text = 'SOS'}) => _sendCmd({'cmd': 'sos', 'text': text});

  /// Все подписчики получают каждое событие (multicast). Длинные JSON склеиваются в [_drainRxAccum].
  Stream<RiftLinkEvent> get events => _eventBus.stream;

  /// Raw parsed JSON events for request/response routing by cmdId.
  Stream<Map<String, dynamic>> get rawEvents => _rawEventBus.stream;
}

List<int> retainRxTailFromLastBraceBytes(List<int> bytes, {int maxRetain = 4096}) {
  if (bytes.isEmpty) return const <int>[];
  final lastBrace = bytes.lastIndexOf(0x7B);
  if (lastBrace < 0) return const <int>[];
  var start = lastBrace;
  final minStart = bytes.length - maxRetain;
  if (minStart > 0 && start < minStart) start = minStart;
  return List<int>.from(bytes.sublist(start));
}

int _jsonInt(dynamic e) {
  if (e == null) return 0;
  if (e is int) return e;
  if (e is num) return e.toInt();
  return int.tryParse(e.toString()) ?? 0;
}

double _jsonDouble(dynamic v, [double fallback = 868.0]) {
  if (v == null) return fallback;
  if (v is num) return v.toDouble();
  return double.tryParse(v.toString()) ?? fallback;
}

double? _jsonDoubleNullable(dynamic v) {
  if (v == null) return null;
  if (v is num) return v.toDouble();
  return double.tryParse(v.toString());
}

int _jsonIntDefault(dynamic v, int d) {
  if (v == null) return d;
  if (v is int) return v;
  if (v is num) return v.toInt();
  return int.tryParse(v.toString()) ?? d;
}

int? _jsonIntNullable(dynamic v) {
  if (v == null) return null;
  if (v is int) return v;
  if (v is num) return v.toInt();
  return int.tryParse(v.toString());
}

String _regionCodeOrDefault(dynamic v, [String fallback = 'EU']) {
  if (v == null) return fallback;
  final s = v.toString().trim();
  return s.isEmpty ? fallback : s;
}

String? _trimmedStringOrNull(dynamic v) {
  if (v == null) return null;
  final s = v.toString().trim();
  return s.isEmpty ? null : s;
}

/// Сколько байт [raw] соответствует префиксу [prefix] из [full] (оба из одного lenient decode).
/// Сначала канонический UTF-8 префикса — совпадает с «сырыми» байтами почти всегда; иначе линейный
/// поиск (изолированный decode префикса может отличаться от decode всего буфера на границе UTF-8).
int? _byteOffsetForDecodedPrefixMatch(List<int> raw, String full, String prefix) {
  if (prefix.isEmpty) return 0;
  if (prefix.length > full.length || !full.startsWith(prefix)) return null;
  try {
    final enc = utf8.encode(prefix);
    if (enc.length <= raw.length) {
      var same = true;
      for (var i = 0; i < enc.length; i++) {
        if (raw[i] != enc[i]) {
          same = false;
          break;
        }
      }
      if (same) return enc.length;
    }
  } catch (_) {}
  for (var b = 1; b <= raw.length; b++) {
    final d = utf8.decode(raw.sublist(0, b), allowMalformed: true);
    if (d == prefix) return b;
  }
  return null;
}

/// Индекс закрывающей `}` для `{` в [openIndex]; учитывает строки и экранирование. null если неполный.
int? _indexOfMatchingBrace(String s, int openIndex) {
  if (openIndex >= s.length || s.codeUnitAt(openIndex) != 0x7B) return null;
  var inStr = false;
  var esc = false;
  var depth = 0;
  for (var i = openIndex; i < s.length; i++) {
    final ch = s.codeUnitAt(i);
    if (inStr) {
      if (esc) {
        esc = false;
        continue;
      }
      if (ch == 0x5C) esc = true;
      else if (ch == 0x22) inStr = false;
      continue;
    }
    if (ch == 0x22) {
      inStr = true;
      continue;
    }
    if (ch == 0x7B) {
      depth++;
      continue;
    }
    if (ch == 0x7D && depth > 0) {
      depth--;
      if (depth == 0) return i;
    }
  }
  return null;
}

/// Из буфера с конкатенированным/обрезанным JSON извлекает полные **корневые** уведомления (`evt` в корне).
/// Вложенные `{...}` (routes и т.д.) не трогаем — иначе портим буфер при обрезанном `info`.
/// Если ничего не извлечено — возвращаем [s] целиком (ожидаем следующий notify).
///
/// Важно: при **неполной** первой «{» (длинный `evt:node` обрезан по MTU) **нельзя** искать следующую «{»:
/// иначе первой окажется полная пара скобок у короткого `evt:routes`, а узел потеряется для merge/UI.
String _tryEmitFromConcatenated(String s, void Function(Map<String, dynamic>) emit) {
  var tailStart = 0;
  var searchPos = 0;
  while (searchPos < s.length) {
    final pos = s.indexOf('{', searchPos);
    if (pos < 0) break;
    final end = _indexOfMatchingBrace(s, pos);
    if (end == null) {
      // Ждём следующий notify — не делаем searchPos = pos + 1 (ломало порядок info-волны).
      return s.substring(tailStart);
    }
    try {
      final decoded = jsonDecode(s.substring(pos, end + 1));
      if (decoded is Map) {
        final m = Map<String, dynamic>.from(decoded as Map);
        if (m.containsKey('evt')) {
          emit(m);
          tailStart = end + 1;
          searchPos = end + 1;
          continue;
        }
        // Полный объект без корневого evt — сдвигаемся за закрывающую «}», не на pos+1.
        searchPos = end + 1;
        continue;
      }
      searchPos = end + 1;
      continue;
    } catch (_) {
      // jsonDecode упал при «закрытой» паре скобок: не делаем searchPos=pos+1 — иначе
      // следующая «{» (evt:routes/groups) эмитится раньше, чем доберётся обрезанный node.
      return s.substring(tailStart);
    }
  }
  return s.substring(tailStart);
}

String _rxDebugPreview(String s, [int max = 200]) {
  final t = s.replaceAll('\n', '\\n').replaceAll('\r', '\\r');
  if (t.length <= max) return t;
  return '${t.substring(0, max)}...';
}

Map<String, dynamic> _normalizeRouteMap(Map<dynamic, dynamic> raw) {
  final m = Map<String, dynamic>.from(raw);
  final out = <String, dynamic>{
    'dest': m['dest']?.toString() ?? '',
    'nextHop': m['nextHop']?.toString() ?? '',
    'hops': _jsonIntDefault(m['hops'], 0),
    'rssi': _jsonIntDefault(m['rssi'], 0),
  };
  // Optional extended route data (for richer traceroute/modem UI).
  final pathRaw = m['path'];
  if (pathRaw is List) {
    out['path'] = pathRaw.map((e) => e.toString()).toList();
  }
  final hopRssiRaw = m['hopRssi'] ?? m['pathRssi'];
  if (hopRssiRaw is List) {
    out['hopRssi'] = hopRssiRaw.map(_jsonInt).toList();
  }
  if (m.containsKey('modemPreset')) out['modemPreset'] = _jsonIntNullable(m['modemPreset']);
  if (m.containsKey('sf')) out['sf'] = _jsonIntNullable(m['sf']);
  if (m.containsKey('bw')) out['bw'] = _jsonDoubleNullable(m['bw']);
  if (m.containsKey('cr')) out['cr'] = _jsonIntNullable(m['cr']);
  if (m.containsKey('trustScore')) out['trustScore'] = _jsonIntNullable(m['trustScore']);
  return out;
}

/// Список групп в JSON: только объекты (`groupUid`, `channelId32`, …). Legacy-массив только из чисел игнорируется.
List<RiftLinkGroupInfo> _parseGroupInfoList(dynamic raw) {
  if (raw is! List || raw.isEmpty) return const <RiftLinkGroupInfo>[];
  if (raw.every((e) => e is int || e is num)) {
    return const <RiftLinkGroupInfo>[];
  }
  final out = <RiftLinkGroupInfo>[];
  for (final item in raw) {
    if (item is! Map) continue;
    final m = Map<String, dynamic>.from(item as Map);
    final uid = (m['groupUid'] ?? '').toString();
    if (uid.trim().isEmpty) continue;
    out.add(
      RiftLinkGroupInfo(
        groupUid: uid,
        groupTag: (m['groupTag'] ?? '').toString(),
        canonicalName: (m['canonicalName'] ?? '').toString(),
        channelId32: _jsonIntDefault(m['channelId32'], 0),
        keyVersion: _jsonIntDefault(m['keyVersion'], 0),
        myRole: (m['myRole'] ?? 'none').toString(),
        revocationEpoch: _jsonIntDefault(m['revocationEpoch'], 0),
        ackApplied: m['ackApplied'] == true || m['ackApplied'] == 1,
      ),
    );
  }
  return out;
}

/// Список id соседей: в прошивке обычно JSON-массив; иначе (строка, одна запись) — не терять данные.
List<String> _parseNeighborIdList(dynamic raw) {
  if (raw == null) return [];
  if (raw is List) {
    return raw.map((e) => e.toString()).toList();
  }
  if (raw is String) {
    final s = raw.trim();
    if (s.isEmpty) return [];
    if (s.startsWith('[')) {
      try {
        final decoded = jsonDecode(s);
        if (decoded is List) {
          return decoded.map((e) => e.toString()).toList();
        }
      } catch (_) {}
    }
    return s
        .split(',')
        .map((e) => e.trim())
        .where((e) => e.isNotEmpty)
        .toList();
  }
  return [];
}

/// Поля как в evt:node / evt:info; для evt:neighbors с тем же набором ключей (дубль паспорта на прошивке).
RiftLinkInfoEvent riftLinkInfoEventFromNodeJson(Map<String, dynamic> json) {
  final neighbors = _parseNeighborIdList(json['neighbors']);
  final rssiList = json['neighborsRssi'] ?? json['rssi'];
  final neighborsRssi = rssiList is List ? (rssiList as List).map(_jsonInt).toList() : <int>[];
  final hasKeyList = json['neighborsHasKey'] ?? json['hasKey'];
  final neighborsHasKey = hasKeyList is List
      ? (hasKeyList as List).map((e) => e == true || e == 1).toList()
      : <bool>[];
  final batMvRaw = json['batMv'];
  final neighborsBatMv = batMvRaw is List ? (batMvRaw as List).map(_jsonInt).toList() : <int>[];
  final groups = _parseGroupInfoList(json['groups']);
  final routesList = json['routes'];
  final routes = <Map<String, dynamic>>[];
  if (routesList is List) {
    for (final r in routesList) {
      if (r is Map) {
        routes.add(_normalizeRouteMap(r as Map));
      }
    }
  }
  final idRaw = json['id'] ?? json['nodeId'];
  final idStr = idRaw == null ? '' : idRaw.toString();
  return RiftLinkInfoEvent(
    cmdId: _jsonIntNullable(json['cmdId']),
    id: idStr,
    nickname: _trimmedStringOrNull(json['nickname']),
    hasNicknameField: json.containsKey('nickname'),
    hasChannelField: json.containsKey('channel'),
    hasOfflinePendingField: json.containsKey('offlinePending'),
    hasOfflineCourierPendingField: json.containsKey('offlineCourierPending'),
    hasOfflineDirectPendingField: json.containsKey('offlineDirectPending'),
    region: _regionCodeOrDefault(json['region']),
    freq: _jsonDouble(json['freq']),
    power: _jsonIntDefault(json['power'], 14),
    channel: _jsonIntNullable(json['channel']),
    version: json['version']?.toString(),
    radioMode: json['radioMode']?.toString() ?? 'ble',
    radioVariant: json['radioVariant']?.toString(),
    wifiConnected: json['wifiConnected'] == true || json['wifiConnected'] == 1,
    wifiSsid: _trimmedStringOrNull(json['wifiSsid']),
    wifiIp: _trimmedStringOrNull(json['wifiIp']),
    neighbors: neighbors,
    neighborsRssi: neighborsRssi,
    neighborsHasKey: neighborsHasKey,
    neighborsBatMv: neighborsBatMv,
    groups: groups,
    routes: routes,
    sf: _jsonIntNullable(json['sf']),
    bw: _jsonDoubleNullable(json['bw']),
    cr: _jsonIntNullable(json['cr']),
    modemPreset: _jsonIntNullable(json['modemPreset']),
    offlinePending: _jsonIntNullable(json['offlinePending']),
    offlineCourierPending: _jsonIntNullable(json['offlineCourierPending']),
    offlineDirectPending: _jsonIntNullable(json['offlineDirectPending']),
    batteryMv: _jsonIntNullable(json['batteryMv']) ?? _jsonIntNullable(json['battery']),
    batteryPercent: _jsonIntNullable(json['batteryPercent']),
    charging: json['charging'] == true || json['charging'] == 1,
    timeHour: _jsonIntNullable(json['timeHour']),
    timeMinute: _jsonIntNullable(json['timeMinute']),
    gpsPresent: json['gpsPresent'] == true || json['gpsPresent'] == 1,
    gpsEnabled: json['gpsEnabled'] == true || json['gpsEnabled'] == 1,
    gpsFix: json['gpsFix'] == true || json['gpsFix'] == 1,
    powersave: json['powersave'] == true || json['powersave'] == 1,
    blePin: _jsonIntNullable(json['blePin']),
    espNowChannel: _jsonIntNullable(json['espNowChannel']) ?? _jsonIntNullable(json['espnowChannel']),
    espNowAdaptive: json['espNowAdaptive'] == true || json['espNowAdaptive'] == 1 || json['espnowAdaptive'] == true || json['espnowAdaptive'] == 1,
    heapFreeBytes: _jsonIntNullable(json['heapFree']),
    heapTotalBytes: _jsonIntNullable(json['heapTotal']),
    heapMinFreeBytes: _jsonIntNullable(json['heapMin']),
    cpuMhz: _jsonIntNullable(json['cpuMhz']),
    flashChipMb: _jsonIntNullable(json['flashMb']),
    appPartitionKb: _jsonIntNullable(json['appPartKb']),
    nvsPartitionKb: _jsonIntNullable(json['nvsPartKb']),
    nvsEntriesUsed: _jsonIntNullable(json['nvsUsedEnt']),
    nvsEntriesFree: _jsonIntNullable(json['nvsFreeEnt']),
    nvsEntriesTotal: _jsonIntNullable(json['nvsTotalEnt']),
    nvsNamespaceCount: _jsonIntNullable(json['nvsNs']),
  );
}

/// Парсинг одного JSON-уведомления с RX (вынесено из потока для единого диспетчера).
RiftLinkEvent? _jsonToEvent(Map<String, dynamic> json) {
  final evtRaw = json['evt'];
  final evt = evtRaw is String ? evtRaw : evtRaw?.toString();
  if (evt == 'msg') {
    return RiftLinkMsgEvent(
      from: json['from'] as String? ?? '',
      text: json['text'] as String? ?? '',
      msgId: (json['msgId'] as num?)?.toInt(),
      rssi: (json['rssi'] as num?)?.toInt(),
      ttlMinutes: (json['ttl'] as num?)?.toInt(),
      lane: json['lane']?.toString() ?? 'normal',
      type: json['type']?.toString() ?? 'text',
      group: _jsonIntNullable(json['group']),
      groupUid: _trimmedStringOrNull(json['groupUid']),
    );
  }
  if (evt == 'sent') {
    return RiftLinkSentEvent(
      to: json['to'] as String? ?? '',
      msgId: (json['msgId'] as num?)?.toInt() ?? 0,
    );
  }
  if (evt == 'delivered') {
    return RiftLinkDeliveredEvent(
      from: json['from'] as String? ?? '',
      msgId: (json['msgId'] as num?)?.toInt() ?? 0,
      rssi: _jsonIntNullable(json['rssi']),
    );
  }
  if (evt == 'read') {
    return RiftLinkReadEvent(
      from: json['from'] as String? ?? '',
      msgId: (json['msgId'] as num?)?.toInt() ?? 0,
      rssi: _jsonIntNullable(json['rssi']),
    );
  }
  if (evt == 'undelivered') {
    return RiftLinkUndeliveredEvent(
      to: json['to'] as String? ?? '',
      msgId: (json['msgId'] as num?)?.toInt() ?? 0,
      delivered: (json['delivered'] as num?)?.toInt(),
      total: (json['total'] as num?)?.toInt(),
    );
  }
  if (evt == 'broadcast_delivery') {
    return RiftLinkBroadcastDeliveryEvent(
      msgId: (json['msgId'] as num?)?.toInt() ?? 0,
      delivered: (json['delivered'] as num?)?.toInt() ?? 0,
      total: (json['total'] as num?)?.toInt() ?? 0,
    );
  }
  if (evt == 'info' || evt == 'node') {
    return riftLinkInfoEventFromNodeJson(json);
  }
  if (evt == 'routes') {
    final routesList = json['routes'];
    final routes = <Map<String, dynamic>>[];
    if (routesList is List) {
      for (final r in routesList) {
        if (r is Map) {
          routes.add(_normalizeRouteMap(r as Map));
        }
      }
    }
    return RiftLinkRoutesEvent(routes: routes);
  }
  if (evt == 'groups') {
    return RiftLinkGroupsEvent(
      groups: _parseGroupInfoList(json['groups']),
      cmdId: _jsonIntNullable(json['cmdId']),
    );
  }
  if (evt == 'groupStatus') {
    final tagRaw = json['groupTag']?.toString();
    return RiftLinkGroupStatusEvent(
      groupUid: json['groupUid']?.toString() ?? '',
      channelId32: _jsonIntNullable(json['channelId32']),
      canonicalName: json['canonicalName']?.toString() ?? '',
      groupTag: tagRaw != null && tagRaw.isNotEmpty ? tagRaw : null,
      myRole: json['myRole']?.toString() ?? 'none',
      keyVersion: _jsonIntDefault(json['keyVersion'], 0),
      status: json['status']?.toString() ?? 'unknown',
      rekeyRequired: json['rekeyRequired'] == true,
      inviteNoop: json['inviteNoop'] == true || json['inviteNoop'] == 1,
    );
  }
  if (evt == 'groupRekeyProgress') {
    return RiftLinkGroupRekeyProgressEvent(
      groupUid: json['groupUid']?.toString() ?? '',
      rekeyOpId: json['rekeyOpId']?.toString() ?? '',
      keyVersion: _jsonIntDefault(json['keyVersion'], 0),
      pending: _jsonIntDefault(json['pending'], 0),
      delivered: _jsonIntDefault(json['delivered'], 0),
      applied: _jsonIntDefault(json['applied'], 0),
      failed: _jsonIntDefault(json['failed'], 0),
    );
  }
  if (evt == 'groupMemberKeyState') {
    return RiftLinkGroupMemberKeyStateEvent(
      groupUid: json['groupUid']?.toString() ?? '',
      memberId: json['memberId']?.toString() ?? '',
      status: json['status']?.toString() ?? 'pending',
      ackAt: _jsonIntNullable(json['ackAt']),
    );
  }
  if (evt == 'groupSecurityError') {
    return RiftLinkGroupSecurityErrorEvent(
      groupUid: json['groupUid']?.toString() ?? '',
      code: json['code']?.toString() ?? 'unknown',
      msg: json['msg']?.toString() ?? '',
      cmdId: _jsonIntNullable(json['cmdId']),
    );
  }
  if (evt == 'groupInvite') {
    return RiftLinkGroupInviteEvent(
      groupUid: json['groupUid']?.toString() ?? '',
      canonicalName: json['canonicalName']?.toString() ?? '',
      role: json['role']?.toString() ?? 'member',
      invite: json['invite']?.toString() ?? '',
      expiresAt: _jsonIntNullable(json['expiresAt']),
      channelId32: _jsonIntNullable(json['channelId32']),
    );
  }
  if (evt == 'telemetry') {
    final bat = (json['battery'] as num?)?.toInt() ?? 0;
    final heap = (json['heapKb'] as num?)?.toInt() ?? 0;
    return RiftLinkTelemetryEvent(
      from: json['from'] as String? ?? '',
      batteryMv: bat,
      heapKb: heap,
      rssi: _jsonIntNullable(json['rssi']),
    );
  }
  if (evt == 'location') {
    final lat = (json['lat'] as num?)?.toDouble() ?? 0.0;
    final lon = (json['lon'] as num?)?.toDouble() ?? 0.0;
    final alt = (json['alt'] as num?)?.toInt() ?? 0;
    return RiftLinkLocationEvent(
      from: json['from'] as String? ?? '',
      lat: lat,
      lon: lon,
      alt: alt,
      rssi: _jsonIntNullable(json['rssi']),
    );
  }
  if (evt == 'region') {
    return RiftLinkRegionEvent(
      region: _regionCodeOrDefault(json['region']),
      freq: (json['freq'] as num?)?.toDouble() ?? 868.0,
      power: (json['power'] as num?)?.toInt() ?? 14,
      channel: (json['channel'] as num?)?.toInt(),
      cmdId: _jsonIntNullable(json['cmdId']),
    );
  }
  if (evt == 'neighbors') {
    final neighbors = _parseNeighborIdList(json['neighbors']);
    final rssiList = json['rssi'];
    final rssi = rssiList is List ? (rssiList as List).map(_jsonInt).toList() : <int>[];
    final hasKeyList = json['hasKey'];
    final hasKey = hasKeyList is List
        ? (hasKeyList as List).map((e) => e == true || e == 1).toList()
        : <bool>[];
    final batMvRaw = json['batMv'];
    final batMv = batMvRaw is List ? (batMvRaw as List).map(_jsonInt).toList() : <int>[];
    final keysPresent = json.keys.toSet();
    return RiftLinkNeighborsEvent(
      neighbors: neighbors,
      rssi: rssi,
      hasKey: hasKey,
      batMv: batMv,
      nodeOverlay: riftLinkInfoEventFromNodeJson(json),
      jsonKeysPresent: keysPresent,
    );
  }
  if (evt == 'pong') {
    return RiftLinkPongEvent(
      from: json['from'] as String? ?? '',
      rssi: _jsonIntNullable(json['rssi']),
      cmdId: _jsonIntNullable(json['cmdId']),
      pingPktId: _jsonIntNullable(json['pingPktId']),
    );
  }
  if (evt == 'error') {
    final code = json['code'] as String? ?? 'unknown';
    final msg = json['msg'] as String? ?? '';
    if (code.startsWith('group_') || code.startsWith('group_v2_')) {
      return RiftLinkGroupSecurityErrorEvent(
        groupUid: json['groupUid']?.toString() ?? '',
        code: code,
        msg: msg,
        cmdId: _jsonIntNullable(json['cmdId']),
      );
    }
    return RiftLinkErrorEvent(code: code, msg: msg);
  }
  if (evt == 'waiting_key') {
    return RiftLinkWaitingKeyEvent(
      to: json['to'] as String? ?? '',
    );
  }
  if (evt == 'voice') {
    return RiftLinkVoiceEvent(
      from: json['from'] as String? ?? '',
      chunk: (json['chunk'] as num?)?.toInt() ?? 0,
      total: (json['total'] as num?)?.toInt() ?? 1,
      data: json['data'] as String? ?? '',
      msgId: _jsonIntNullable(json['msgId']),
    );
  }
  if (evt == 'bleOtaReady') {
    return RiftLinkBleOtaReadyEvent(
      chunkSize: _jsonIntDefault(json['chunkSize'], 509),
    );
  }
  if (evt == 'bleOtaProgress') {
    return RiftLinkBleOtaProgressEvent(
      written: _jsonIntDefault(json['written'], 0),
    );
  }
  if (evt == 'bleOtaResult') {
    return RiftLinkBleOtaResultEvent(
      ok: json['ok'] == true,
      reason: json['reason'] as String?,
    );
  }
  if (evt == 'wifi') {
    return RiftLinkWifiEvent(
      connected: json['connected'] == true,
      ssid: json['ssid'] as String? ?? '',
      ip: json['ip'] as String? ?? '',
    );
  }
  if (evt == 'gps') {
    return RiftLinkGpsEvent(
      present: json['present'] == true,
      enabled: json['enabled'] == true,
      hasFix: json['hasFix'] == true,
      rx: _jsonIntNullable(json['rx']),
      tx: _jsonIntNullable(json['tx']),
      en: _jsonIntNullable(json['en']),
    );
  }
  if (evt == 'invite') {
    return RiftLinkInviteEvent(
      id: json['id'] as String? ?? '',
      pubKey: json['pubKey'] as String? ?? '',
      channelKey: json['channelKey'] as String?,
      inviteToken: json['inviteToken'] as String?,
      inviteExpiresMs: _jsonIntNullable(json['inviteExpiresMs']),
      inviteTtlMs: _jsonIntNullable(json['inviteTtlMs']),
    );
  }
  if (evt == 'relayProof') {
    return RiftLinkRelayProofEvent(
      relayedBy: json['relayedBy'] as String? ?? '',
      from: json['from'] as String? ?? '',
      to: json['to'] as String? ?? '',
      pktId: _jsonIntDefault(json['pktId'], 0),
      opcode: _jsonIntDefault(json['opcode'], 0),
    );
  }
  if (evt == 'timeCapsuleQueued') {
    return RiftLinkTimeCapsuleQueuedEvent(
      to: json['to'] as String?,
      trigger: json['trigger']?.toString() ?? '',
      triggerAtMs: _jsonIntNullable(json['triggerAtMs']),
    );
  }
  if (evt == 'timeCapsuleReleased') {
    return RiftLinkTimeCapsuleReleasedEvent(
      to: json['to'] as String? ?? '',
      msgId: _jsonIntDefault(json['msgId'], 0),
      trigger: json['trigger']?.toString() ?? '',
    );
  }
  if (evt == 'selftest') {
    return RiftLinkSelftestEvent(
      radioOk: json['radioOk'] == true,
      displayOk: json['displayOk'] == true,
      antennaOk: json['antennaOk'] != false,
      batteryMv: (json['batteryMv'] as num?)?.toInt() ?? 0,
      batteryPercent: _jsonIntNullable(json['batteryPercent']),
      charging: json['charging'] == true,
      heapFree: (json['heapFree'] as num?)?.toInt() ?? 0,
    );
  }
  if (evt == 'loraScan') {
    final raw = json['results'];
    final out = <RiftLinkLoraScanResult>[];
    if (raw is List) {
      for (final e in raw) {
        if (e is Map) {
          final m = Map<String, dynamic>.from(e);
          out.add(RiftLinkLoraScanResult(
            sf: _jsonIntDefault(m['sf'], 0),
            bw: (m['bw'] as num?)?.toDouble() ?? 0.0,
            rssi: _jsonIntDefault(m['rssi'], 0),
          ));
        }
      }
    }
    return RiftLinkLoraScanEvent(
      count: _jsonIntDefault(json['count'], out.length),
      quick: json['quick'] == true,
      results: out,
      cmdId: _jsonIntNullable(json['cmdId']),
    );
  }
  return null;
}

sealed class RiftLinkEvent {}

class RiftLinkGroupInfo {
  final String groupUid;
  final String groupTag;
  final String canonicalName;
  final int channelId32;
  final int keyVersion;
  final String myRole;
  final int revocationEpoch;
  final bool ackApplied;
  const RiftLinkGroupInfo({
    required this.groupUid,
    required this.groupTag,
    this.canonicalName = '',
    required this.channelId32,
    required this.keyVersion,
    required this.myRole,
    required this.revocationEpoch,
    this.ackApplied = false,
  });

  /// Только поле `myRole`: «пусто»/`none` в новом снимке не затирает известную роль
  /// (частичный `groupStatus`, гонки после ребута узла).
  static String mergeMyRoleWithPrevious(String incoming, String? previous) {
    final inc = incoming.trim().toLowerCase();
    final prev = (previous ?? '').trim().toLowerCase();
    if ((inc.isEmpty || inc == 'none') && prev.isNotEmpty && prev != 'none') {
      return previous!.trim();
    }
    if (incoming.trim().isEmpty) {
      return (previous != null && previous.trim().isNotEmpty) ? previous.trim() : 'none';
    }
    return incoming.trim();
  }

  /// Слияние с прошлым снимком: пустой/`none` [myRole] и пустые строки не затирают
  /// известную роль и метаданные (частичные JSON при `info` / гонки notify).
  RiftLinkGroupInfo mergedWithPrevious(RiftLinkGroupInfo? previous) {
    if (previous == null) return this;
    return RiftLinkGroupInfo(
      groupUid: groupUid.trim().isNotEmpty ? groupUid : previous.groupUid,
      groupTag: groupTag.trim().isNotEmpty ? groupTag : previous.groupTag,
      canonicalName: canonicalName.trim().isNotEmpty ? canonicalName : previous.canonicalName,
      channelId32: channelId32,
      keyVersion: keyVersion > 0 ? keyVersion : previous.keyVersion,
      myRole: mergeMyRoleWithPrevious(myRole, previous.myRole),
      revocationEpoch: revocationEpoch != 0 ? revocationEpoch : previous.revocationEpoch,
      ackApplied: ackApplied || previous.ackApplied,
    );
  }
}

class RiftLinkMsgEvent extends RiftLinkEvent {
  final String from;
  final String text;
  final int? msgId;
  final int? rssi;
  final int? ttlMinutes;
  final String lane; // normal|critical
  final String type; // text|sos|...
  final int? group; // channelId32 for GROUP_MSG
  final String? groupUid; // canonical UID for GROUP_MSG
  RiftLinkMsgEvent({
    required this.from,
    required this.text,
    this.msgId,
    this.rssi,
    this.ttlMinutes,
    this.lane = 'normal',
    this.type = 'text',
    this.group,
    this.groupUid,
  });
}

class RiftLinkSentEvent extends RiftLinkEvent {
  final String to;
  final int msgId;
  RiftLinkSentEvent({required this.to, required this.msgId});
}

class RiftLinkDeliveredEvent extends RiftLinkEvent {
  final String from;
  final int msgId;
  final int? rssi;
  RiftLinkDeliveredEvent({required this.from, required this.msgId, this.rssi});
}

class RiftLinkReadEvent extends RiftLinkEvent {
  final String from;
  final int msgId;
  final int? rssi;
  RiftLinkReadEvent({required this.from, required this.msgId, this.rssi});
}

/// Unicast: ACK не получен. Broadcast: delivered=0 при total>0.
class RiftLinkUndeliveredEvent extends RiftLinkEvent {
  final String to;
  final int msgId;
  final int? delivered;  // для broadcast: 0
  final int? total;      // для broadcast: кол-во соседей
  RiftLinkUndeliveredEvent({required this.to, required this.msgId, this.delivered, this.total});
}

/// Broadcast: доставлено X из Y
class RiftLinkBroadcastDeliveryEvent extends RiftLinkEvent {
  final int msgId;
  final int delivered;
  final int total;
  RiftLinkBroadcastDeliveryEvent({required this.msgId, required this.delivered, required this.total});
}

class RiftLinkInfoEvent extends RiftLinkEvent {
  final int? cmdId;
  final String id;
  final String? nickname;
  final bool hasNicknameField;
  final bool hasChannelField;
  final bool hasOfflinePendingField;
  final bool hasOfflineCourierPendingField;
  final bool hasOfflineDirectPendingField;
  final String region;
  final double freq;
  final int power;
  final int? channel;
  final String? version;
  final String radioMode;  // "ble" or "wifi"
  final String? radioVariant;  // "sta"
  final bool wifiConnected;
  final String? wifiSsid;
  final String? wifiIp;
  final List<String> neighbors;
  final List<int> neighborsRssi;
  final List<bool> neighborsHasKey;
  /// Напряжение соседей (мВ) из `batMv` в `evt:neighbors` (0 = неизвестно); длина по идее как у [neighbors].
  final List<int> neighborsBatMv;
  /// Полные записи групп (в JSON ключ `groups`; legacy-массив id не используется).
  final List<RiftLinkGroupInfo> groups;
  final List<Map<String, dynamic>> routes;
  final int? sf;
  final double? bw;
  final int? cr;
  final int? modemPreset;   // 0=Speed,1=Normal,2=Range,3=MaxRange,4=Custom
  final int? offlinePending;
  final int? offlineCourierPending;
  final int? offlineDirectPending;
  final int? batteryMv;
  final int? batteryPercent;
  final bool charging;
  final int? timeHour;
  final int? timeMinute;
  final bool gpsPresent;
  final bool gpsEnabled;
  final bool gpsFix;
  final bool powersave;
  final int? blePin;
  final int? espNowChannel;
  final bool espNowAdaptive;
  /// Internal DRAM heap (bytes), с `evt:node` / прошивка ≥ с полями heapFree/heapTotal.
  final int? heapFreeBytes;
  final int? heapTotalBytes;
  final int? heapMinFreeBytes;
  final int? cpuMhz;
  final int? flashChipMb;
  final int? appPartitionKb;
  final int? nvsPartitionKb;
  final int? nvsEntriesUsed;
  final int? nvsEntriesFree;
  final int? nvsEntriesTotal;
  final int? nvsNamespaceCount;
  RiftLinkInfoEvent({
    this.cmdId,
    required this.id,
    this.nickname,
    this.hasNicknameField = false,
    this.hasChannelField = false,
    this.hasOfflinePendingField = false,
    this.hasOfflineCourierPendingField = false,
    this.hasOfflineDirectPendingField = false,
    this.region = 'EU',
    this.freq = 868.0,
    this.power = 14,
    this.channel,
    this.version,
    this.radioMode = 'ble',
    this.radioVariant,
    this.wifiConnected = false,
    this.wifiSsid,
    this.wifiIp,
    this.neighbors = const [],
    this.neighborsRssi = const [],
    this.neighborsHasKey = const [],
    this.neighborsBatMv = const [],
    this.groups = const [],
    this.routes = const [],
    this.sf,
    this.bw,
    this.cr,
    this.modemPreset,
    this.offlinePending,
    this.offlineCourierPending,
    this.offlineDirectPending,
    this.batteryMv,
    this.batteryPercent,
    this.charging = false,
    this.timeHour,
    this.timeMinute,
    this.gpsPresent = false,
    this.gpsEnabled = false,
    this.gpsFix = false,
    this.powersave = false,
    this.blePin,
    this.espNowChannel,
    this.espNowAdaptive = false,
    this.heapFreeBytes,
    this.heapTotalBytes,
    this.heapMinFreeBytes,
    this.cpuMhz,
    this.flashChipMb,
    this.appPartitionKb,
    this.nvsPartitionKb,
    this.nvsEntriesUsed,
    this.nvsEntriesFree,
    this.nvsEntriesTotal,
    this.nvsNamespaceCount,
  });
}

class RiftLinkWifiEvent extends RiftLinkEvent {
  final bool connected;
  final String ssid;
  final String ip;
  RiftLinkWifiEvent({required this.connected, required this.ssid, required this.ip});
}

class RiftLinkGpsEvent extends RiftLinkEvent {
  final bool present;
  final bool enabled;
  final bool hasFix;
  /// Пины UART/GPIO с прошивки (nRF/ESP), если есть.
  final int? rx;
  final int? tx;
  final int? en;
  RiftLinkGpsEvent({
    required this.present,
    required this.enabled,
    required this.hasFix,
    this.rx,
    this.tx,
    this.en,
  });
}

class RiftLinkInviteEvent extends RiftLinkEvent {
  final String id;
  final String pubKey;
  final String? channelKey;  // base64, опционально
  final String? inviteToken;
  final int? inviteExpiresMs;
  final int? inviteTtlMs;
  RiftLinkInviteEvent({
    required this.id,
    required this.pubKey,
    this.channelKey,
    this.inviteToken,
    this.inviteExpiresMs,
    this.inviteTtlMs,
  });
}

class RiftLinkRelayProofEvent extends RiftLinkEvent {
  final String relayedBy;
  final String from;
  final String to;
  final int pktId;
  final int opcode;
  RiftLinkRelayProofEvent({
    required this.relayedBy,
    required this.from,
    required this.to,
    required this.pktId,
    required this.opcode,
  });
}

class RiftLinkTimeCapsuleQueuedEvent extends RiftLinkEvent {
  final String? to;
  final String trigger;
  final int? triggerAtMs;
  RiftLinkTimeCapsuleQueuedEvent({
    this.to,
    required this.trigger,
    this.triggerAtMs,
  });
}

class RiftLinkTimeCapsuleReleasedEvent extends RiftLinkEvent {
  final String to;
  final int msgId;
  final String trigger;
  RiftLinkTimeCapsuleReleasedEvent({
    required this.to,
    required this.msgId,
    required this.trigger,
  });
}

class RiftLinkSelftestEvent extends RiftLinkEvent {
  final bool radioOk;
  final bool displayOk;
  final bool antennaOk;
  final int batteryMv;
  final int? batteryPercent;
  final bool charging;
  /// Свободная куча в **байтах** (`evt.selftest` / прошивка `ESP.getFreeHeap()`).
  final int heapFree;
  RiftLinkSelftestEvent({
    required this.radioOk,
    required this.displayOk,
    this.antennaOk = true,
    required this.batteryMv,
    this.batteryPercent,
    this.charging = false,
    required this.heapFree,
  });
}

/// Элемент `results[]` в `evt:loraScan` (docs/API §2.10.1).
class RiftLinkLoraScanResult {
  final int sf;
  final double bw;
  final int rssi;
  const RiftLinkLoraScanResult({required this.sf, required this.bw, required this.rssi});
}

class RiftLinkLoraScanEvent extends RiftLinkEvent {
  final int count;
  final bool quick;
  final List<RiftLinkLoraScanResult> results;
  final int? cmdId;
  RiftLinkLoraScanEvent({
    required this.count,
    required this.quick,
    required this.results,
    this.cmdId,
  });
}

class RiftLinkRoutesEvent extends RiftLinkEvent {
  final List<Map<String, dynamic>> routes;  // dest, nextHop, hops, rssi
  RiftLinkRoutesEvent({required this.routes});
}

class RiftLinkGroupsEvent extends RiftLinkEvent {
  final List<RiftLinkGroupInfo> groups;
  /// Ответ на `cmd: groups` с `cmdId` с узла; в волне notifyInfo() часто отсутствует.
  final int? cmdId;
  RiftLinkGroupsEvent({this.groups = const [], this.cmdId});
}

class RiftLinkGroupStatusEvent extends RiftLinkEvent {
  final String groupUid;
  final int? channelId32;
  final String canonicalName;
  /// Опционально: приходит с прошивки вместе с группой.
  final String? groupTag;
  final String myRole;
  final int keyVersion;
  final String status;
  final bool rekeyRequired;
  /// Прошивка: приём инвайта не менял запись — группа уже была (свой инвайт и т.п.).
  final bool inviteNoop;
  RiftLinkGroupStatusEvent({
    required this.groupUid,
    this.channelId32,
    this.canonicalName = '',
    this.groupTag,
    required this.myRole,
    required this.keyVersion,
    required this.status,
    this.rekeyRequired = false,
    this.inviteNoop = false,
  });
}

class RiftLinkGroupRekeyProgressEvent extends RiftLinkEvent {
  final String groupUid;
  final String rekeyOpId;
  final int keyVersion;
  final int pending;
  final int delivered;
  final int applied;
  final int failed;
  RiftLinkGroupRekeyProgressEvent({
    required this.groupUid,
    required this.rekeyOpId,
    required this.keyVersion,
    required this.pending,
    required this.delivered,
    required this.applied,
    required this.failed,
  });
}

class RiftLinkGroupMemberKeyStateEvent extends RiftLinkEvent {
  final String groupUid;
  final String memberId;
  final String status;
  final int? ackAt;
  RiftLinkGroupMemberKeyStateEvent({
    required this.groupUid,
    required this.memberId,
    required this.status,
    this.ackAt,
  });
}

class RiftLinkGroupSecurityErrorEvent extends RiftLinkEvent {
  final String groupUid;
  final String code;
  final String msg;
  /// Совпадает с `cmdId` tracked-команды, если прошивка его вложила.
  final int? cmdId;
  RiftLinkGroupSecurityErrorEvent({
    required this.groupUid,
    required this.code,
    required this.msg,
    this.cmdId,
  });
}

class RiftLinkGroupInviteEvent extends RiftLinkEvent {
  final String groupUid;
  final String canonicalName;
  final String role;
  final String invite;
  final int? expiresAt;
  final int? channelId32;
  RiftLinkGroupInviteEvent({
    required this.groupUid,
    this.canonicalName = '',
    required this.role,
    required this.invite,
    this.expiresAt,
    this.channelId32,
  });
}

class RiftLinkNeighborsEvent extends RiftLinkEvent {
  final List<String> neighbors;
  final List<int> rssi;
  final List<bool> hasKey;  // true = можно отправить
  /// МВ по соседям (`batMv` в JSON nRF/ESP); длина как у [neighbors].
  final List<int> batMv;
  /// Те же поля, что в evt:node (прошивка дублирует паспорт + метрики в evt:neighbors при малом MTU).
  final RiftLinkInfoEvent nodeOverlay;
  /// Какие ключи были в JSON — чтобы не затирать кэш значениями по умолчанию из парсера.
  final Set<String> jsonKeysPresent;
  RiftLinkNeighborsEvent({
    required this.neighbors,
    this.rssi = const [],
    this.hasKey = const [],
    this.batMv = const [],
    required this.nodeOverlay,
    required this.jsonKeysPresent,
  });
}

class RiftLinkRegionEvent extends RiftLinkEvent {
  final String region;
  final double freq;
  final int power;
  final int? channel;  // 0–2 для EU/UK
  final int? cmdId;
  RiftLinkRegionEvent({
    required this.region,
    required this.freq,
    required this.power,
    this.channel,
    this.cmdId,
  });
}

class RiftLinkTelemetryEvent extends RiftLinkEvent {
  final String from;
  final int batteryMv;
  final int heapKb;
  final int? rssi;
  RiftLinkTelemetryEvent({
    required this.from,
    required this.batteryMv,
    required this.heapKb,
    this.rssi,
  });
}

class RiftLinkLocationEvent extends RiftLinkEvent {
  final String from;
  final double lat;
  final double lon;
  final int alt;
  final int? rssi;
  RiftLinkLocationEvent({
    required this.from,
    required this.lat,
    required this.lon,
    this.alt = 0,
    this.rssi,
  });
}

class RiftLinkPongEvent extends RiftLinkEvent {
  final String from;
  final int? rssi;
  final int? cmdId;
  /// Эхо `pktId` из OP_PING в эфире (связка ответа с конкретным пингом).
  final int? pingPktId;
  RiftLinkPongEvent({required this.from, this.rssi, this.cmdId, this.pingPktId});
}

class RiftLinkErrorEvent extends RiftLinkEvent {
  final String code;
  final String msg;
  RiftLinkErrorEvent({required this.code, required this.msg});
}

class RiftLinkWaitingKeyEvent extends RiftLinkEvent {
  final String to;
  RiftLinkWaitingKeyEvent({required this.to});
}

class RiftLinkVoiceEvent extends RiftLinkEvent {
  final String from;
  final int chunk;
  final int total;
  final String data;  // base64
  final int? msgId;
  RiftLinkVoiceEvent({
    required this.from,
    required this.chunk,
    required this.total,
    required this.data,
    this.msgId,
  });
}

class RiftLinkBleOtaReadyEvent extends RiftLinkEvent {
  final int chunkSize;
  RiftLinkBleOtaReadyEvent({required this.chunkSize});
}

class RiftLinkBleOtaProgressEvent extends RiftLinkEvent {
  final int written;
  RiftLinkBleOtaProgressEvent({required this.written});
}

class RiftLinkBleOtaResultEvent extends RiftLinkEvent {
  final bool ok;
  final String? reason;
  RiftLinkBleOtaResultEvent({required this.ok, this.reason});
}

List<RiftLinkGroupInfo> _mergeGroupListWithPrevious(
  List<RiftLinkGroupInfo> incoming,
  List<RiftLinkGroupInfo> previous,
) {
  final prevByCh = <int, RiftLinkGroupInfo>{};
  final prevByUid = <String, RiftLinkGroupInfo>{};
  for (final g in previous) {
    if (g.channelId32 > 1) prevByCh[g.channelId32] = g;
    final u = g.groupUid.trim().toUpperCase();
    if (u.isNotEmpty) prevByUid[u] = g;
  }
  return incoming
      .map((g) {
        RiftLinkGroupInfo? pr;
        if (g.channelId32 > 1) pr = prevByCh[g.channelId32];
        final u = g.groupUid.trim().toUpperCase();
        if (u.isNotEmpty) pr ??= prevByUid[u];
        return g.mergedWithPrevious(pr);
      })
      .toList();
}

/// Сохраняет снимок узла из [p], если в [n] не пришли системные поля (монолитный `evt:info` с топологией).
RiftLinkInfoEvent _mergeSystemMetricsFromPrevious(RiftLinkInfoEvent n, RiftLinkInfoEvent? p) {
  if (p == null) return n;
  return RiftLinkInfoEvent(
    cmdId: n.cmdId,
    id: n.id,
    nickname: n.nickname,
    hasNicknameField: n.hasNicknameField,
    hasChannelField: n.hasChannelField,
    hasOfflinePendingField: n.hasOfflinePendingField,
    hasOfflineCourierPendingField: n.hasOfflineCourierPendingField,
    hasOfflineDirectPendingField: n.hasOfflineDirectPendingField,
    region: n.region,
    freq: n.freq,
    power: n.power,
    channel: n.channel,
    version: n.version,
    radioMode: n.radioMode,
    radioVariant: n.radioVariant,
    wifiConnected: n.wifiConnected,
    wifiSsid: n.wifiSsid,
    wifiIp: n.wifiIp,
    neighbors: n.neighbors,
    neighborsRssi: n.neighborsRssi,
    neighborsHasKey: n.neighborsHasKey,
    neighborsBatMv: n.neighborsBatMv.isNotEmpty ? n.neighborsBatMv : p.neighborsBatMv,
    groups: n.groups,
    routes: n.routes,
    sf: n.sf,
    bw: n.bw,
    cr: n.cr,
    modemPreset: n.modemPreset,
    offlinePending: n.offlinePending,
    offlineCourierPending: n.offlineCourierPending,
    offlineDirectPending: n.offlineDirectPending,
    batteryMv: n.batteryMv ?? p.batteryMv,
    batteryPercent: n.batteryPercent ?? p.batteryPercent,
    charging: n.charging,
    timeHour: n.timeHour ?? p.timeHour,
    timeMinute: n.timeMinute ?? p.timeMinute,
    gpsPresent: n.gpsPresent,
    gpsEnabled: n.gpsEnabled,
    gpsFix: n.gpsFix,
    powersave: n.powersave,
    blePin: n.blePin ?? p.blePin,
    espNowChannel: n.espNowChannel ?? p.espNowChannel,
    espNowAdaptive: n.espNowAdaptive,
    heapFreeBytes: n.heapFreeBytes ?? p.heapFreeBytes,
    heapTotalBytes: n.heapTotalBytes ?? p.heapTotalBytes,
    heapMinFreeBytes: n.heapMinFreeBytes ?? p.heapMinFreeBytes,
    cpuMhz: n.cpuMhz ?? p.cpuMhz,
    flashChipMb: n.flashChipMb ?? p.flashChipMb,
    appPartitionKb: n.appPartitionKb ?? p.appPartitionKb,
    nvsPartitionKb: n.nvsPartitionKb ?? p.nvsPartitionKb,
    nvsEntriesUsed: n.nvsEntriesUsed ?? p.nvsEntriesUsed,
    nvsEntriesFree: n.nvsEntriesFree ?? p.nvsEntriesFree,
    nvsEntriesTotal: n.nvsEntriesTotal ?? p.nvsEntriesTotal,
    nvsNamespaceCount: n.nvsNamespaceCount ?? p.nvsNamespaceCount,
  );
}

RiftLinkInfoEvent _copyInfoReplacingGroups(RiftLinkInfoEvent e, List<RiftLinkGroupInfo> groups) {
  return RiftLinkInfoEvent(
    cmdId: e.cmdId,
    id: e.id,
    nickname: e.nickname,
    hasNicknameField: e.hasNicknameField,
    hasChannelField: e.hasChannelField,
    hasOfflinePendingField: e.hasOfflinePendingField,
    hasOfflineCourierPendingField: e.hasOfflineCourierPendingField,
    hasOfflineDirectPendingField: e.hasOfflineDirectPendingField,
    region: e.region,
    freq: e.freq,
    power: e.power,
    channel: e.channel,
    version: e.version,
    radioMode: e.radioMode,
    radioVariant: e.radioVariant,
    wifiConnected: e.wifiConnected,
    wifiSsid: e.wifiSsid,
    wifiIp: e.wifiIp,
    neighbors: e.neighbors,
    neighborsRssi: e.neighborsRssi,
    neighborsHasKey: e.neighborsHasKey,
    neighborsBatMv: e.neighborsBatMv,
    groups: groups,
    routes: e.routes,
    sf: e.sf,
    bw: e.bw,
    cr: e.cr,
    modemPreset: e.modemPreset,
    offlinePending: e.offlinePending,
    offlineCourierPending: e.offlineCourierPending,
    offlineDirectPending: e.offlineDirectPending,
    batteryMv: e.batteryMv,
    batteryPercent: e.batteryPercent,
    charging: e.charging,
    timeHour: e.timeHour,
    timeMinute: e.timeMinute,
    gpsPresent: e.gpsPresent,
    gpsEnabled: e.gpsEnabled,
    gpsFix: e.gpsFix,
    powersave: e.powersave,
    blePin: e.blePin,
    espNowChannel: e.espNowChannel,
    espNowAdaptive: e.espNowAdaptive,
    heapFreeBytes: e.heapFreeBytes,
    heapTotalBytes: e.heapTotalBytes,
    heapMinFreeBytes: e.heapMinFreeBytes,
    cpuMhz: e.cpuMhz,
    flashChipMb: e.flashChipMb,
    appPartitionKb: e.appPartitionKb,
    nvsPartitionKb: e.nvsPartitionKb,
    nvsEntriesUsed: e.nvsEntriesUsed,
    nvsEntriesFree: e.nvsEntriesFree,
    nvsEntriesTotal: e.nvsEntriesTotal,
    nvsNamespaceCount: e.nvsNamespaceCount,
  );
}
