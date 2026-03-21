/// RiftLink BLE — связь с Heltec LoRa по GATT / WiFi WebSocket
/// Протокол: JSON {"cmd":"send","text":"..."} / {"evt":"msg","from":"...","text":"..."}
/// Dual transport: BLE (default) ↔ WiFi (on demand) through radio mode switching.

import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart' show debugPrint, kDebugMode, kIsWeb;
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../transport/wifi_transport.dart';

class RiftLinkBle {
  static const serviceUuid = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
  static const charTxUuid = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
  static const charRxUuid = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';
  static const deviceName = 'RiftLink';

  BluetoothDevice? _device;
  BluetoothCharacteristic? _txChar;
  BluetoothCharacteristic? _rxChar;

  /// Broadcast: каждый подписчик получает каждое событие.
  /// Пока `listen` ещё не вызван (окно после connect, async initState), события копятся в [_preListenBuffer]
  /// и сливаются в поток при первом подписчике — иначе Dart broadcast **теряет** add без слушателей.
  int _eventsStreamListeners = 0;
  final List<RiftLinkEvent> _preListenBuffer = [];
  static const int _maxPreListenBuffer = 64;

  late final StreamController<RiftLinkEvent> _eventBus = StreamController<RiftLinkEvent>.broadcast(
    onListen: _onEventsStreamListen,
    onCancel: _onEventsStreamCancel,
  );

  void _onEventsStreamListen() {
    _eventsStreamListeners++;
    if (_eventsStreamListeners == 1 && _preListenBuffer.isNotEmpty) {
      final batch = List<RiftLinkEvent>.from(_preListenBuffer);
      _preListenBuffer.clear();
      for (final e in batch) {
        if (!_eventBus.isClosed) _eventBus.add(e);
      }
    }
  }

  void _onEventsStreamCancel() {
    if (_eventsStreamListeners > 0) _eventsStreamListeners--;
  }

  StreamSubscription<OnCharacteristicReceivedEvent>? _rxSub;
  /// Склейка фрагментов notify (длинный `info` / MTU): каждый chunk может быть не целым JSON.
  final List<int> _rxAccum = [];
  int _lastRxIncompleteLogLen = 0;
  Timer? _rxAccumTimeout;
  Completer<void>? _txLock;
  DateTime? _lastInfoRequestAt;
  Timer? _queuedInfoTimer;
  bool _hasQueuedInfoRequest = false;

  /// Последний успешно распарсенный `evt:info` (до первого кадра экрана / после connect).
  RiftLinkInfoEvent? _lastInfo;

  RiftLinkInfoEvent? get lastInfo => _lastInfo;

  BluetoothDevice? get device => _device;
  bool get isConnected => _device?.isConnected ?? false;

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
    final m = RegExp(r'RL-([0-9A-Fa-f]{8})').firstMatch(name);
    return m != null ? m.group(1)!.toUpperCase() : null;
  }

  Future<bool> _sendCmd(Map<String, dynamic> payload) async {
    // WiFi mode: route through WebSocket (no MTU limit)
    if (_isWifiMode && _wifiTransport != null && _wifiTransport!.isConnected) {
      return _wifiTransport!.sendJson(jsonEncode(payload));
    }
    if (_txChar == null || !isConnected) return false;

    while (_txLock != null) {
      await _txLock!.future;
    }
    _txLock = Completer<void>();
    try {
      final json = jsonEncode(payload);
      await _txChar!.write(utf8.encode(json), withoutResponse: true);
      return true;
    } catch (e) {
      debugPrint('RiftLinkBle: _sendCmd error: $e');
      return false;
    } finally {
      final c = _txLock;
      _txLock = null;
      c?.complete();
    }
  }

  /// Подключение к устройству
  Future<bool> connect(BluetoothDevice dev) async {
    await disconnect();
    _device = dev;
    try {
      await dev.connect();
      await dev.discoverServices();
    } catch (_) {
      _device = null;
      _txChar = null;
      _rxChar = null;
      rethrow;
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
    getInfo(force: true);
    getGroups();
    getRoutes();
    return true;
  }

  /// Запросить info (evt "info"). Централизованный throttle, чтобы экраны не спамили устройству.
  Future<bool> getInfo({bool force = false}) async {
    if (!isConnected) return false;
    if (force) {
      _queuedInfoTimer?.cancel();
      _queuedInfoTimer = null;
      _hasQueuedInfoRequest = false;
      _lastInfoRequestAt = DateTime.now();
      return _sendCmd({'cmd': 'info'});
    }

    const minGap = Duration(milliseconds: 700);
    final now = DateTime.now();
    final last = _lastInfoRequestAt;
    if (last == null || now.difference(last) >= minGap) {
      _lastInfoRequestAt = now;
      return _sendCmd({'cmd': 'info'});
    }

    if (_hasQueuedInfoRequest) return true;
    _hasQueuedInfoRequest = true;
    final wait = minGap - now.difference(last);
    _queuedInfoTimer?.cancel();
    _queuedInfoTimer = Timer(wait, () async {
      _hasQueuedInfoRequest = false;
      if (!isConnected) return;
      _lastInfoRequestAt = DateTime.now();
      await _sendCmd({'cmd': 'info'});
    });
    return true;
  }

  Future<void> disconnect() async {
    _lastInfo = null;
    _preListenBuffer.clear();
    _queuedInfoTimer?.cancel();
    _queuedInfoTimer = null;
    _hasQueuedInfoRequest = false;
    _lastInfoRequestAt = null;
    await _disconnectWifi();
    _isWifiMode = false;
    _wifiIp = null;
    await _stopRxDispatcher();
    if (_device != null) {
      await _device!.disconnect();
      _device = null;
    }
    _txChar = null;
    _rxChar = null;
  }

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
        if (kDebugMode) {
          debugPrint(
            'RiftLinkBle: chrReceived ${event.value.length}b remote=${c.remoteId.str} chr=${c.characteristicUuid}',
          );
        }
        if (event.error != null && event.value.isEmpty) return;
        if (!RiftLinkBle.remoteIdsMatch(c.remoteId.str, devId.str)) {
          if (kDebugMode) {
            debugPrint('RiftLinkBle: chrReceived skipped (remoteId) want=${devId.str} got=${c.remoteId.str}');
          }
          return;
        }
        if (!RiftLinkBle.characteristicUuidsMatch(c.characteristicUuid, rx.characteristicUuid)) {
          if (kDebugMode) {
            debugPrint(
              'RiftLinkBle: chrReceived skipped (uuid) want=${rx.characteristicUuid} got=${c.characteristicUuid}',
            );
          }
          return;
        }
        _feedRxChunk(event.value);
      },
      onError: (Object e, StackTrace _) {
        assert(() {
          debugPrint('RiftLinkBle: onCharacteristicReceived error $e');
          return true;
        }());
      },
    );

    await rx.setNotifyValue(true);
  }

  void _feedRxChunk(List<int> chunk) {
    if (chunk.isEmpty) return;
    _rxAccum.addAll(chunk);
    const maxAccum = 16384;
    if (_rxAccum.length > maxAccum) {
      debugPrint('RiftLinkBle: RX buffer overflow (${_rxAccum.length} bytes), clearing');
      _rxAccum.clear();
      _rxAccumTimeout?.cancel();
      return;
    }
    _rxAccumTimeout?.cancel();
    _rxAccumTimeout = Timer(const Duration(seconds: 5), () {
      if (_rxAccum.isNotEmpty) {
        debugPrint('RiftLinkBle: RX timeout, discarding ${_rxAccum.length} incomplete bytes');
        _rxAccum.clear();
        _lastRxIncompleteLogLen = 0;
      }
    });
    _drainRxAccum();
  }

  void _emitParsedJson(Map<String, dynamic> json) {
    RiftLinkEvent? evt;
    try {
      evt = _jsonToEvent(json);
    } catch (e, st) {
      debugPrint('RiftLinkBle: _jsonToEvent FAILED evt=${json['evt']}: $e\n$st');
      return;
    }
    if (evt is RiftLinkInfoEvent) {
      _lastInfo = evt;
    }
    if (_eventBus.isClosed) return;
    if (evt == null) {
      if (kDebugMode) {
        debugPrint(
          'RiftLinkBle: JSON без известного evt keys=${json.keys.toList()} evt=${json['evt']}',
        );
      }
      return;
    }

    debugPrint('RiftLinkBle: evt ${evt.runtimeType}'); // отладка: видно в I/flutter, что парсинг доходит до Dart
    if (_eventsStreamListeners == 0) {
      _preListenBuffer.add(evt);
      if (_preListenBuffer.length > _maxPreListenBuffer) {
        _preListenBuffer.removeAt(0);
      }
    } else {
      _eventBus.add(evt);
    }
  }

  void _stripToFirstAsciiBraceByte() {
    final i = _rxAccum.indexWhere((b) => b == 0x7B);
    if (i < 0) {
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

  /// NDJSON: после каждого JSON на прошивке — `\n` (или `\r\n`). Тогда границы не зависят от MTU.
  /// Внутри строковых полей неэкранированный `\n` ломает разбор — экранировать или не использовать.
  String? _tryDrainNewlineDelimitedJson(String s) {
    if (!s.contains('\n')) return null;
    final lastNl = s.lastIndexOf('\n');
    final complete = s.substring(0, lastNl + 1);
    final tail = s.substring(lastNl + 1);
    for (final line in complete.split('\n')) {
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
        if (kDebugMode) debugPrint('RiftLinkBle: NDJSON parse error: $e');
      }
    }
    return tail;
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
        _rxAccum.clear();
        return;
      }
      if (i0 > 0) {
        _rxAccum
          ..clear()
          ..addAll(utf8.encode(s.substring(i0)));
        continue;
      }

      final t = s;
      final ndTail = _tryDrainNewlineDelimitedJson(t);
      if (ndTail != null) {
        _rxAccum
          ..clear()
          ..addAll(utf8.encode(ndTail));
        if (ndTail.isEmpty) return;
        continue;
      }

      // Один полный объект в буфере (самый частый случай).
      try {
        final decoded = jsonDecode(t);
        if (decoded is Map) {
          _lastRxIncompleteLogLen = 0;
          _emitParsedJson(Map<String, dynamic>.from(decoded as Map));
          _rxAccum.clear();
          return;
        }
        // Массив объектов на корне — без этого разбор молча зависел от экстрактора по скобкам.
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
        // Неполный JSON или несколько объектов подряд — разбираем ниже
        if (kDebugMode && s.length < 256) debugPrint('RiftLinkBle: single-object parse: $e');
      }

      final objs = _extractTopLevelJsonObjects(s);
      if (objs.objects.isEmpty) {
        // Неполный первый объект (обрезка по границе notify) — ищем любой полный объект с позиции `{`.
        final tail = _tryEmitFromConcatenated(s, _emitParsedJson);
        if (tail.length < s.length) {
          _lastRxIncompleteLogLen = 0;
          _rxAccum
            ..clear()
            ..addAll(utf8.encode(tail));
          if (tail.isEmpty) return;
          continue;
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
          final lastBrace = s.lastIndexOf('{');
          if (lastBrace > 0) {
            _rxAccum
              ..clear()
              ..addAll(utf8.encode(s.substring(lastBrace)));
          } else {
            _rxAccum.clear();
          }
        }
        return;
      }
      _lastRxIncompleteLogLen = 0;
      for (final raw in objs.objects) {
        try {
          final decoded = jsonDecode(raw);
          if (decoded is! Map) continue;
          _emitParsedJson(Map<String, dynamic>.from(decoded as Map));
        } catch (e) {
          if (kDebugMode) debugPrint('RiftLinkBle: extracted object parse error: $e');
        }
      }
      final tailBytes = utf8.encode(objs.tail);
      _rxAccum
        ..clear()
        ..addAll(tailBytes);
      if (tailBytes.isEmpty) return;
    }
  }

  Future<void> _stopRxDispatcher() async {
    final old = _rxSub;
    _rxSub = null;
    await old?.cancel();
    _rxAccum.clear();
    _lastRxIncompleteLogLen = 0;
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
  Future<bool> setRegion(String region) async =>
      _sendCmd({'cmd': 'region', 'region': region});

  /// Установить никнейм (до 16 символов)
  Future<bool> setNickname(String nickname) async {
    if (utf8.encode(nickname).length > 32) return false;
    return _sendCmd({'cmd': 'nickname', 'nickname': nickname});
  }

  /// Установить канал (0–2) для EU/UK
  Future<bool> setChannel(int channel) async {
    if (channel < 0 || channel > 2) return false;
    return _sendCmd({'cmd': 'channel', 'channel': channel});
  }

  /// LoRa spreading factor (mesh), 7–12
  Future<bool> setSpreadingFactor(int sf) async {
    if (sf < 7 || sf > 12) return false;
    return _sendCmd({'cmd': 'sf', 'sf': sf});
  }

  /// Modem preset (0=Speed, 1=Normal, 2=Range, 3=MaxRange)
  Future<bool> setModemPreset(int preset) async {
    if (preset < 0 || preset > 3) return false;
    return _sendCmd({'cmd': 'modemPreset', 'preset': preset});
  }

  /// Custom modem: SF 7–12, BW kHz (62.5/125/250/500), CR 5–8
  Future<bool> setCustomModem(int sf, double bw, int cr) async {
    if (sf < 7 || sf > 12 || cr < 5 || cr > 8) return false;
    return _sendCmd({'cmd': 'modemCustom', 'sf': sf, 'bw': bw, 'cr': cr});
  }

  /// Отправить голосовое сообщение (Opus/AAC, base64 чанками)
  Future<bool> sendVoice({required String to, required List<String> chunks}) async {
    if (to.length < 8 || chunks.isEmpty) return false;
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
    if (from.length < 8 || msgId == 0) return false;
    return _sendCmd({'cmd': 'read', 'from': from, 'msgId': msgId});
  }

  /// Запросить маршруты (evt "routes")
  Future<bool> getRoutes() async => _sendCmd({'cmd': 'routes'});

  /// Запросить список групп
  Future<bool> getGroups() async => _sendCmd({'cmd': 'groups'});

  /// Добавить группу (ID 1–0xFFFFFFFF)
  Future<bool> addGroup(int groupId) async =>
      groupId <= 0 ? false : _sendCmd({'cmd': 'addGroup', 'group': groupId});

  /// Удалить группу
  Future<bool> removeGroup(int groupId) async =>
      groupId <= 0 ? false : _sendCmd({'cmd': 'removeGroup', 'group': groupId});

  /// Установить/обновить приватный ключ группы (base64, 32 байта).
  Future<bool> setGroupKey(int groupId, String keyB64, {int? keyVersion}) async =>
      groupId <= 0 || keyB64.trim().isEmpty
          ? false
          : _sendCmd({
              'cmd': 'setGroupKey',
              'group': groupId,
              'key': keyB64.trim(),
              if (keyVersion != null && keyVersion > 0) 'keyVersion': keyVersion,
            });

  /// Удалить приватный ключ группы (группа станет public).
  Future<bool> clearGroupKey(int groupId) async =>
      groupId <= 0 ? false : _sendCmd({'cmd': 'clearGroupKey', 'group': groupId});

  /// Запросить приватный ключ группы (ответ evt:groupKey).
  Future<bool> getGroupKey(int groupId) async =>
      groupId <= 0 ? false : _sendCmd({'cmd': 'getGroupKey', 'group': groupId});

  /// Отправить PING на узел (проверка связи)
  Future<bool> sendPing(String to) async =>
      to.length < 8 ? false : _sendCmd({'cmd': 'ping', 'to': to});

  // --- Radio Mode Switching (Time-sharing BLE ↔ WiFi) ---

  WifiTransport? _wifiTransport;
  StreamSubscription? _wifiRxSub;
  bool _isWifiMode = false;

  /// Текущий радио-режим: true = WiFi, false = BLE
  bool get isWifiMode => _isWifiMode;

  String? _wifiIp;
  /// IP address used for the current WiFi connection (null if BLE).
  String? get wifiIp => _wifiIp;

  /// Переключить в WiFi STA-режим (подключение к сети)
  Future<bool> switchToWifiSta({required String ssid, required String pass}) =>
      _sendCmd({'cmd': 'radioMode', 'mode': 'wifi', 'variant': 'sta', 'ssid': ssid, 'pass': pass});

  /// Переключить обратно в BLE
  Future<bool> switchToBle() async {
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
    _wifiRxSub = _wifiTransport!.rawJsonStream.listen((raw) {
      try {
        final decoded = jsonDecode(raw);
        if (decoded is Map) {
          _emitParsedJson(Map<String, dynamic>.from(decoded as Map));
        }
      } catch (e) {
        if (kDebugMode) debugPrint('RiftLinkBle: WiFi JSON parse error: $e');
      }
    });
    // Wi-Fi transport does not push initial state automatically like BLE connect flow.
    // Request baseline payload right after WS attach.
    await Future<void>.delayed(const Duration(milliseconds: 120));
    await getInfo(force: true);
    await Future<void>.delayed(const Duration(milliseconds: 80));
    await getGroups();
    await getRoutes();
    return true;
  }

  Future<void> _disconnectWifi() async {
    await _wifiRxSub?.cancel();
    _wifiRxSub = null;
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
    try {
      await _txChar!.write(data, withoutResponse: true);
      return true;
    } catch (_) {
      return false;
    }
  }

  /// BLE OTA: завершить OTA
  Future<bool> endBleOta() => _sendCmd({'cmd': 'bleOtaEnd'});

  /// BLE OTA: отменить OTA
  Future<bool> abortBleOta() => _sendCmd({'cmd': 'bleOtaAbort'});

  /// Выключить устройство (deep sleep, пробуждение кнопкой)
  Future<bool> shutdown() => _sendCmd({'cmd': 'shutdown'});

  /// Тест сигнала: пинг всех соседей (ответы приходят как evt:pong)
  Future<bool> signalTest() => _sendCmd({'cmd': 'signalTest'});

  /// Трассировка: запрос маршрута до узла (ответ evt:routes)
  Future<bool> traceroute(String to) async =>
      to.length < 8 ? false : _sendCmd({'cmd': 'traceroute', 'to': to});

  /// ESP-NOW: канал 1..13 (для WiFi-режима)
  Future<bool> setEspNowChannel(int channel) async {
    if (channel < 1 || channel > 13) return false;
    return _sendCmd({'cmd': 'espnowChannel', 'channel': channel});
  }

  /// ESP-NOW: адаптивный подбор канала
  Future<bool> setEspNowAdaptive(bool enabled) async =>
      _sendCmd({'cmd': 'espnowAdaptive', 'enabled': enabled});

  /// WiFi: SSID + пароль (переключает в WiFi STA)
  Future<bool> setWifi({required String ssid, required String pass}) async =>
      switchToWifiSta(ssid: ssid, pass: pass);

  /// GPS: вкл/выкл
  Future<bool> setGps(bool enabled) async =>
      _sendCmd({'cmd': 'gps', 'enabled': enabled});

  /// Powersave
  Future<bool> setPowersave(bool enabled) async =>
      _sendCmd({'cmd': 'powersave', 'enabled': enabled});

  /// Язык прошивки (ru/en)
  Future<bool> setLang(String lang) async =>
      _sendCmd({'cmd': 'lang', 'lang': lang});

  /// Создать E2E invite (evt "invite"), TTL ограничен на устройстве.
  Future<bool> createInvite({int ttlSec = 600}) async => _sendCmd({'cmd': 'invite', 'ttlSec': ttlSec});

  /// Принять invite (id + pubKey base64, опционально channelKey/inviteToken).
  Future<bool> acceptInvite({
    required String id,
    required String pubKey,
    String? channelKey,
    String? inviteToken,
  }) async {
    final payload = <String, dynamic>{'cmd': 'acceptInvite', 'id': id, 'pubKey': pubKey};
    if (channelKey != null && channelKey.isNotEmpty) payload['channelKey'] = channelKey;
    if (inviteToken != null && inviteToken.isNotEmpty) payload['inviteToken'] = inviteToken;
    return _sendCmd(payload);
  }

  /// Перегенерировать BLE PIN (passkey) — устройство покажет новый PIN на экране
  Future<bool> regeneratePin() async => _sendCmd({'cmd': 'regeneratePin'});

  /// Selftest (evt "selftest")
  Future<bool> selftest() async => _sendCmd({'cmd': 'selftest'});

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
    final payload = group != null && group > 0
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
            ? {
                'cmd': 'send',
                'to': to,
                'text': text,
                if (ttlMinutes > 0) 'ttl': ttlMinutes,
                if (lane != 'normal') 'lane': lane,
                if (trigger != null && trigger.isNotEmpty) 'trigger': trigger,
                if (triggerAtMs != null) 'triggerAtMs': triggerAtMs,
              }
            : {
                'cmd': 'send',
                'text': text,
                if (ttlMinutes > 0) 'ttl': ttlMinutes,
                if (lane != 'normal') 'lane': lane,
                if (trigger != null && trigger.isNotEmpty) 'trigger': trigger,
                if (triggerAtMs != null) 'triggerAtMs': triggerAtMs,
              };
    return _sendCmd(payload);
  }

  /// Emergency flood
  Future<bool> sendSos({String text = 'SOS'}) => _sendCmd({'cmd': 'sos', 'text': text});

  /// Все подписчики получают каждое событие (multicast). Длинные JSON склеиваются в [_drainRxAccum].
  Stream<RiftLinkEvent> get events => _eventBus.stream;
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

({List<String> objects, String tail}) _extractTopLevelJsonObjects(String s) {
  final out = <String>[];
  var inStr = false;
  var esc = false;
  var depth = 0;
  int? start;
  var lastConsumed = 0;

  for (var i = 0; i < s.length; i++) {
    final ch = s.codeUnitAt(i);
    if (inStr) {
      if (esc) {
        esc = false;
        continue;
      }
      if (ch == 0x5C) {
        esc = true;
      } else if (ch == 0x22) {
        inStr = false;
      }
      continue;
    }
    if (ch == 0x22) {
      inStr = true;
      continue;
    }
    if (ch == 0x7B) {
      if (depth == 0) start = i;
      depth++;
      continue;
    }
    if (ch == 0x7D && depth > 0) {
      depth--;
      if (depth == 0 && start != null) {
        out.add(s.substring(start, i + 1));
        lastConsumed = i + 1;
        start = null;
      }
    }
  }

  final tail = lastConsumed > 0 ? s.substring(lastConsumed) : s;
  return (objects: out, tail: tail);
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
String _tryEmitFromConcatenated(String s, void Function(Map<String, dynamic>) emit) {
  var tailStart = 0;
  var searchPos = 0;
  while (searchPos < s.length) {
    final pos = s.indexOf('{', searchPos);
    if (pos < 0) break;
    final end = _indexOfMatchingBrace(s, pos);
    if (end == null) {
      searchPos = pos + 1;
      continue;
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
      }
    } catch (_) {}
    searchPos = pos + 1;
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
    );
  }
  if (evt == 'read') {
    return RiftLinkReadEvent(
      from: json['from'] as String? ?? '',
      msgId: (json['msgId'] as num?)?.toInt() ?? 0,
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
  if (evt == 'info') {
    final nb = json['neighbors'];
    final neighbors = nb is List ? (nb as List).map((e) => e.toString()).toList() : <String>[];
    final rssiList = json['neighborsRssi'];
    final neighborsRssi = rssiList is List ? (rssiList as List).map(_jsonInt).toList() : <int>[];
    final hasKeyList = json['neighborsHasKey'];
    final neighborsHasKey = hasKeyList is List
        ? (hasKeyList as List).map((e) => e == true || e == 1).toList()
        : <bool>[];
    final grpList = json['groups'];
    final groups = grpList is List ? (grpList as List).map(_jsonInt).toList() : <int>[];
    final grpPrivList = json['groupsPrivate'];
    final groupsPrivate = grpPrivList is List
        ? (grpPrivList as List).map((e) => e == true || e == 1).toList()
        : <bool>[];
    final grpVerList = json['groupsKeyVersion'];
    final groupsKeyVersion = grpVerList is List ? (grpVerList as List).map(_jsonInt).toList() : <int>[];
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
      groups: groups,
      groupsPrivate: groupsPrivate,
      groupsKeyVersion: groupsKeyVersion,
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
    );
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
    final grpList = json['groups'];
    final grpPrivList = json['groupsPrivate'];
    final grpVerList = json['groupsKeyVersion'];
    return RiftLinkGroupsEvent(
      groups: grpList is List ? (grpList as List).map(_jsonInt).toList() : <int>[],
      groupsPrivate: grpPrivList is List
          ? (grpPrivList as List).map((e) => e == true || e == 1).toList()
          : <bool>[],
      groupsKeyVersion: grpVerList is List ? (grpVerList as List).map(_jsonInt).toList() : <int>[],
    );
  }
  if (evt == 'groupKey') {
    return RiftLinkGroupKeyEvent(
      group: _jsonIntDefault(json['group'], 0),
      key: json['key']?.toString() ?? '',
      keyVersion: _jsonIntDefault(json['keyVersion'], 0),
    );
  }
  if (evt == 'telemetry') {
    final bat = (json['battery'] as num?)?.toInt() ?? 0;
    final heap = (json['heapKb'] as num?)?.toInt() ?? 0;
    return RiftLinkTelemetryEvent(
      from: json['from'] as String? ?? '',
      batteryMv: bat,
      heapKb: heap,
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
    );
  }
  if (evt == 'region') {
    return RiftLinkRegionEvent(
      region: _regionCodeOrDefault(json['region']),
      freq: (json['freq'] as num?)?.toDouble() ?? 868.0,
      power: (json['power'] as num?)?.toInt() ?? 14,
      channel: (json['channel'] as num?)?.toInt(),
    );
  }
  if (evt == 'neighbors') {
    final list = json['neighbors'];
    final rssiList = json['rssi'];
    final rssi = rssiList is List ? (rssiList as List).map(_jsonInt).toList() : <int>[];
    final hasKeyList = json['hasKey'];
    final hasKey = hasKeyList is List ? (hasKeyList as List).map((e) => e == true).toList() : <bool>[];
    return RiftLinkNeighborsEvent(
      neighbors: list is List ? (list as List).map((e) => e.toString()).toList() : [],
      rssi: rssi,
      hasKey: hasKey,
    );
  }
  if (evt == 'pong') {
    return RiftLinkPongEvent(
      from: json['from'] as String? ?? '',
      rssi: _jsonIntNullable(json['rssi']),
    );
  }
  if (evt == 'error') {
    return RiftLinkErrorEvent(
      code: json['code'] as String? ?? 'unknown',
      msg: json['msg'] as String? ?? '',
    );
  }
  if (evt == 'voice') {
    return RiftLinkVoiceEvent(
      from: json['from'] as String? ?? '',
      chunk: (json['chunk'] as num?)?.toInt() ?? 0,
      total: (json['total'] as num?)?.toInt() ?? 1,
      data: json['data'] as String? ?? '',
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
  return null;
}

sealed class RiftLinkEvent {}

class RiftLinkMsgEvent extends RiftLinkEvent {
  final String from;
  final String text;
  final int? msgId;
  final int? rssi;
  final int? ttlMinutes;
  final String lane; // normal|critical
  final String type; // text|sos|...
  RiftLinkMsgEvent({
    required this.from,
    required this.text,
    this.msgId,
    this.rssi,
    this.ttlMinutes,
    this.lane = 'normal',
    this.type = 'text',
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
  RiftLinkDeliveredEvent({required this.from, required this.msgId});
}

class RiftLinkReadEvent extends RiftLinkEvent {
  final String from;
  final int msgId;
  RiftLinkReadEvent({required this.from, required this.msgId});
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
  final List<int> groups;
  final List<bool> groupsPrivate;
  final List<int> groupsKeyVersion;
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
  RiftLinkInfoEvent({
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
    this.groups = const [],
    this.groupsPrivate = const [],
    this.groupsKeyVersion = const [],
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
  RiftLinkGpsEvent({required this.present, required this.enabled, required this.hasFix});
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

class RiftLinkRoutesEvent extends RiftLinkEvent {
  final List<Map<String, dynamic>> routes;  // dest, nextHop, hops, rssi
  RiftLinkRoutesEvent({required this.routes});
}

class RiftLinkGroupsEvent extends RiftLinkEvent {
  final List<int> groups;
  final List<bool> groupsPrivate;
  final List<int> groupsKeyVersion;
  RiftLinkGroupsEvent({required this.groups, this.groupsPrivate = const [], this.groupsKeyVersion = const []});
}

class RiftLinkGroupKeyEvent extends RiftLinkEvent {
  final int group;
  final String key;
  final int keyVersion;
  RiftLinkGroupKeyEvent({required this.group, required this.key, this.keyVersion = 0});
}

class RiftLinkNeighborsEvent extends RiftLinkEvent {
  final List<String> neighbors;
  final List<int> rssi;
  final List<bool> hasKey;  // true = можно отправить
  RiftLinkNeighborsEvent({required this.neighbors, this.rssi = const [], this.hasKey = const []});
}

class RiftLinkRegionEvent extends RiftLinkEvent {
  final String region;
  final double freq;
  final int power;
  final int? channel;  // 0–2 для EU/UK
  RiftLinkRegionEvent({
    required this.region,
    required this.freq,
    required this.power,
    this.channel,
  });
}

class RiftLinkTelemetryEvent extends RiftLinkEvent {
  final String from;
  final int batteryMv;
  final int heapKb;
  RiftLinkTelemetryEvent({
    required this.from,
    required this.batteryMv,
    required this.heapKb,
  });
}

class RiftLinkLocationEvent extends RiftLinkEvent {
  final String from;
  final double lat;
  final double lon;
  final int alt;
  RiftLinkLocationEvent({
    required this.from,
    required this.lat,
    required this.lon,
    this.alt = 0,
  });
}

class RiftLinkPongEvent extends RiftLinkEvent {
  final String from;
  final int? rssi;
  RiftLinkPongEvent({required this.from, this.rssi});
}

class RiftLinkErrorEvent extends RiftLinkEvent {
  final String code;
  final String msg;
  RiftLinkErrorEvent({required this.code, required this.msg});
}

class RiftLinkVoiceEvent extends RiftLinkEvent {
  final String from;
  final int chunk;
  final int total;
  final String data;  // base64
  RiftLinkVoiceEvent({
    required this.from,
    required this.chunk,
    required this.total,
    required this.data,
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
