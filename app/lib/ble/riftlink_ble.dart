/// RiftLink BLE — связь с Heltec LoRa по GATT
/// Протокол: JSON {"cmd":"send","text":"..."} / {"evt":"msg","from":"...","text":"..."}

import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart' show debugPrint, kIsWeb;
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

class RiftLinkBle {
  static const serviceUuid = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
  static const charTxUuid = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
  static const charRxUuid = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';
  static const deviceName = 'RiftLink';

  BluetoothDevice? _device;
  BluetoothCharacteristic? _txChar;
  BluetoothCharacteristic? _rxChar;

  /// Один приёмник notify + broadcast: несколько `events.listen` (чат, настройки, группы…)
  /// получают одни и те же события. Длинные JSON склеиваются из нескольких пакетов ([onValueReceived]).
  ///
  /// Поток живёт всё время жизни [RiftLinkBle]: при отключении RX не закрывается — иначе подписка,
  /// созданная до connect() или до повторного connect(), остаётся на [Stream.empty] / закрытом stream.
  final StreamController<RiftLinkEvent> _eventBus = StreamController<RiftLinkEvent>.broadcast();
  StreamSubscription<OnCharacteristicReceivedEvent>? _rxSub;
  /// Склейка фрагментов notify (длинный `info` / MTU): каждый chunk может быть не целым JSON.
  final List<int> _rxAccum = [];
  DateTime? _lastInfoRequestAt;
  Timer? _queuedInfoTimer;
  bool _hasQueuedInfoRequest = false;

  /// Последний успешно распарсенный `evt:info` (ответ на connect/getInfo может прийти
  /// до подписки экрана на [events] — без кэша UI и «недавние» теряют данные).
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

  /// Короткий ID из имени BLE (`RL-XXXXXXXX`) — пока нет полного `evt.info.id`.
  static String? nodeIdHintFromDevice(BluetoothDevice? dev) {
    if (dev == null) return null;
    final name = dev.platformName.isNotEmpty ? dev.platformName : dev.advName;
    final m = RegExp(r'RL-([0-9A-Fa-f]{8})').firstMatch(name);
    return m != null ? m.group(1)!.toUpperCase() : null;
  }

  Future<bool> _sendCmd(Map<String, dynamic> payload) async {
    if (_txChar == null || !isConnected) return false;
    try {
      final json = jsonEncode(payload);
      await _txChar!.write(utf8.encode(json), withoutResponse: true);
      return true;
    } catch (_) {
      return false;
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
    _queuedInfoTimer?.cancel();
    _queuedInfoTimer = null;
    _hasQueuedInfoRequest = false;
    _lastInfoRequestAt = null;
    _stopRxDispatcher();
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
    _rxSub?.cancel();
    _rxSub = null;
    _rxAccum.clear();

    final devId = _device!.remoteId;
    final rx = _rxChar!;

    if (!kIsWeb) {
      await Future<void>.delayed(const Duration(milliseconds: 120));
    }

    // Подписка до setNotifyValue: иначе первые notify после включения CCCD могут прийти до listen().
    _rxSub = FlutterBluePlus.events.onCharacteristicReceived.listen(
      (OnCharacteristicReceivedEvent event) {
        // Не отбрасывать полезную нагрузку, если платформа пометила success=false.
        if (event.error != null && event.value.isEmpty) return;
        final c = event.characteristic;
        if (!RiftLinkBle.remoteIdsMatch(c.remoteId.str, devId.str)) return;
        if (c.characteristicUuid != rx.characteristicUuid) return;
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
      _rxAccum.clear();
      return;
    }
    _drainRxAccum();
  }

  void _emitParsedJson(Map<String, dynamic> json) {
    RiftLinkEvent? evt;
    try {
      evt = _jsonToEvent(json);
    } catch (e, _) {
      assert(() {
        debugPrint('RiftLinkBle: _jsonToEvent: $e');
        return true;
      }());
      return;
    }
    if (evt is RiftLinkInfoEvent) {
      _lastInfo = evt;
    }
    if (evt != null && !_eventBus.isClosed) {
      _eventBus.add(evt);
    }
  }

  /// Разбор RX: сначала один целый JSON (основной путь), иначе — несколько объектов подряд.
  /// Разбор по скобкам оставлен как запасной; он может ошибаться на `\"`/`\\` внутри строк.
  void _drainRxAccum() {
    for (var iter = 0; iter < 32; iter++) {
      final s = _decodeUtf8OrNull(_rxAccum);
      if (s == null) return;

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
      // Один полный объект в буфере (самый частый случай).
      try {
        final decoded = jsonDecode(t);
        if (decoded is Map) {
          _emitParsedJson(Map<String, dynamic>.from(decoded as Map));
          _rxAccum.clear();
          return;
        }
      } catch (_) {
        // Неполный JSON или несколько объектов подряд — ниже.
      }

      final objs = _extractTopLevelJsonObjects(s);
      if (objs.objects.isEmpty) {
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
      for (final raw in objs.objects) {
        try {
          final decoded = jsonDecode(raw);
          if (decoded is! Map) continue;
          _emitParsedJson(Map<String, dynamic>.from(decoded as Map));
        } catch (_) {}
      }
      final tailBytes = utf8.encode(objs.tail);
      _rxAccum
        ..clear()
        ..addAll(tailBytes);
      if (tailBytes.isEmpty) return;
    }
  }

  void _stopRxDispatcher() {
    _rxSub?.cancel();
    _rxSub = null;
    _rxAccum.clear();
  }

  /// Отправка геолокации (broadcast)
  Future<bool> sendLocation({required double lat, required double lon, int alt = 0}) async =>
      _sendCmd({'cmd': 'location', 'lat': lat, 'lon': lon, 'alt': alt});

  /// GPS sync от телефона: UTC ms, lat, lon, alt — для beacon-sync (устройство без GPS)
  Future<bool> sendGpsSync({required int utcMs, required double lat, required double lon, int alt = 0}) async =>
      utcMs != 0 ? _sendCmd({'cmd': 'gps_sync', 'utc_ms': utcMs, 'lat': lat, 'lon': lon, 'alt': alt}) : Future.value(false);

  /// Установить регион (EU, RU, UK, US, AU)
  Future<bool> setRegion(String region) async =>
      _sendCmd({'cmd': 'region', 'region': region});

  /// Установить никнейм (до 16 символов)
  Future<bool> setNickname(String nickname) async {
    if (nickname.length > 16) return false;
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

  /// Отправить голосовое сообщение (Opus/AAC, base64 чанками)
  Future<bool> sendVoice({required String to, required List<String> chunks}) async {
    if (to.length < 8 || chunks.isEmpty) return false;
    for (var i = 0; i < chunks.length; i++) {
      await _sendCmd({
        'cmd': 'voice',
        'to': to,
        'chunk': i,
        'total': chunks.length,
        'data': chunks[i],
      });
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

  /// Отправить PING на узел (проверка связи)
  Future<bool> sendPing(String to) async =>
      to.length < 8 ? false : _sendCmd({'cmd': 'ping', 'to': to});

  /// Запуск OTA режима (WiFi AP + ArduinoOTA)
  Future<bool> sendOta() async => _sendCmd({'cmd': 'ota'});

  /// WiFi: SSID + пароль
  Future<bool> setWifi({required String ssid, required String pass}) async =>
      _sendCmd({'cmd': 'wifi', 'ssid': ssid, 'pass': pass});

  /// GPS: вкл/выкл
  Future<bool> setGps(bool enabled) async =>
      _sendCmd({'cmd': 'gps', 'enabled': enabled});

  /// Powersave
  Future<bool> setPowersave(bool enabled) async =>
      _sendCmd({'cmd': 'powersave', 'enabled': enabled});

  /// Язык прошивки (ru/en)
  Future<bool> setLang(String lang) async =>
      _sendCmd({'cmd': 'lang', 'lang': lang});

  /// Создать E2E invite (evt "invite")
  Future<bool> createInvite() async => _sendCmd({'cmd': 'invite'});

  /// Принять invite (id + pubKey base64, опционально channelKey base64)
  Future<bool> acceptInvite({required String id, required String pubKey, String? channelKey}) async {
    final payload = <String, dynamic>{'cmd': 'acceptInvite', 'id': id, 'pubKey': pubKey};
    if (channelKey != null && channelKey.isNotEmpty) payload['channelKey'] = channelKey;
    return _sendCmd(payload);
  }

  /// Selftest (evt "selftest")
  Future<bool> selftest() async => _sendCmd({'cmd': 'selftest'});

  /// Отправка сообщения (broadcast, unicast или в группу). ttlMinutes: 0 = постоянное
  Future<bool> send({String? to, int? group, required String text, int ttlMinutes = 0}) async {
    final payload = group != null && group > 0
        ? {'cmd': 'send', 'group': group, 'text': text, if (ttlMinutes > 0) 'ttl': ttlMinutes}
        : to != null
            ? {'cmd': 'send', 'to': to, 'text': text, if (ttlMinutes > 0) 'ttl': ttlMinutes}
            : {'cmd': 'send', 'text': text, if (ttlMinutes > 0) 'ttl': ttlMinutes};
    return _sendCmd(payload);
  }

  /// Все подписчики получают один и тот же поток (см. [_eventBus]).
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

String? _decodeUtf8OrNull(List<int> bytes) {
  try {
    return utf8.decode(bytes, allowMalformed: false);
  } catch (_) {
    return null;
  }
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
    final routesList = json['routes'];
    final routes = <Map<String, dynamic>>[];
    if (routesList is List) {
      for (final r in routesList) {
        if (r is Map) {
          final m = Map<String, dynamic>.from(r as Map);
          routes.add({
            'dest': m['dest']?.toString() ?? '',
            'nextHop': m['nextHop']?.toString() ?? '',
            'hops': _jsonIntDefault(m['hops'], 0),
            'rssi': _jsonIntDefault(m['rssi'], 0),
          });
        }
      }
    }
    final idRaw = json['id'] ?? json['nodeId'];
    final idStr = idRaw == null ? '' : idRaw.toString();
    return RiftLinkInfoEvent(
      id: idStr,
      nickname: _trimmedStringOrNull(json['nickname']),
      /// Прошивка опускает ключи, если значение «пустое» (см. ble.cpp notifyInfo):
      /// без флагов приложение затирало UI при null из отсутствующего поля.
      hasNicknameField: json.containsKey('nickname'),
      hasChannelField: json.containsKey('channel'),
      hasOfflinePendingField: json.containsKey('offlinePending'),
      region: _regionCodeOrDefault(json['region']),
      freq: _jsonDouble(json['freq']),
      power: _jsonIntDefault(json['power'], 14),
      channel: _jsonIntNullable(json['channel']),
      version: json['version']?.toString(),
      neighbors: neighbors,
      neighborsRssi: neighborsRssi,
      neighborsHasKey: neighborsHasKey,
      groups: groups,
      routes: routes,
      sf: _jsonIntNullable(json['sf']),
      offlinePending: _jsonIntNullable(json['offlinePending']),
      batteryMv: _jsonIntNullable(json['batteryMv']) ?? _jsonIntNullable(json['battery']),
      gpsPresent: json['gpsPresent'] == true || json['gpsPresent'] == 1,
      gpsEnabled: json['gpsEnabled'] == true || json['gpsEnabled'] == 1,
      gpsFix: json['gpsFix'] == true || json['gpsFix'] == 1,
      powersave: json['powersave'] == true || json['powersave'] == 1,
    );
  }
  if (evt == 'routes') {
    final routesList = json['routes'];
    final routes = <Map<String, dynamic>>[];
    if (routesList is List) {
      for (final r in routesList) {
        if (r is Map) {
          final m = Map<String, dynamic>.from(r as Map);
          routes.add({
            'dest': m['dest'] as String? ?? '',
            'nextHop': m['nextHop'] as String? ?? '',
            'hops': (m['hops'] as num?)?.toInt() ?? 0,
            'rssi': (m['rssi'] as num?)?.toInt() ?? 0,
          });
        }
      }
    }
    return RiftLinkRoutesEvent(routes: routes);
  }
  if (evt == 'groups') {
    final grpList = json['groups'];
    return RiftLinkGroupsEvent(
      groups: grpList is List ? (grpList as List).map(_jsonInt).toList() : <int>[],
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
  if (evt == 'ota') {
    return RiftLinkOtaEvent(
      ip: json['ip'] as String? ?? '192.168.4.1',
      ssid: json['ssid'] as String? ?? 'RiftLink-OTA',
      password: json['password'] as String? ?? 'riftlink123',
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
    return RiftLinkPongEvent(from: json['from'] as String? ?? '');
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
    );
  }
  if (evt == 'selftest') {
    return RiftLinkSelftestEvent(
      radioOk: json['radioOk'] == true,
      displayOk: json['displayOk'] == true,
      batteryMv: (json['batteryMv'] as num?)?.toInt() ?? 0,
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
  RiftLinkMsgEvent({required this.from, required this.text, this.msgId, this.rssi, this.ttlMinutes});
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
  /// Ключ `nickname` есть в JSON (прошивка не шлёт ключ, если ник пустой).
  final bool hasNicknameField;
  /// Ключ `channel` есть в JSON (для RU/US и т.д. может отсутствовать).
  final bool hasChannelField;
  /// Ключ `offlinePending` есть в JSON (прошивка не шлёт, если 0).
  final bool hasOfflinePendingField;
  final String region;
  final double freq;
  final int power;
  final int? channel;  // 0–2 для EU/UK
  final String? version;
  final List<String> neighbors;
  final List<int> neighborsRssi;
  final List<bool> neighborsHasKey;  // true = можно отправить (ключ есть)
  final List<int> groups;
  final List<Map<String, dynamic>> routes;
  final int? sf;
  final int? offlinePending;
  final int? batteryMv;
  final bool gpsPresent;
  final bool gpsEnabled;
  final bool gpsFix;
  final bool powersave;
  RiftLinkInfoEvent({
    required this.id,
    this.nickname,
    this.hasNicknameField = false,
    this.hasChannelField = false,
    this.hasOfflinePendingField = false,
    this.region = 'EU',
    this.freq = 868.0,
    this.power = 14,
    this.channel,
    this.version,
    this.neighbors = const [],
    this.neighborsRssi = const [],
    this.neighborsHasKey = const [],
    this.groups = const [],
    this.routes = const [],
    this.sf,
    this.offlinePending,
    this.batteryMv,
    this.gpsPresent = false,
    this.gpsEnabled = false,
    this.gpsFix = false,
    this.powersave = false,
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
  RiftLinkInviteEvent({required this.id, required this.pubKey, this.channelKey});
}

class RiftLinkSelftestEvent extends RiftLinkEvent {
  final bool radioOk;
  final bool displayOk;
  final int batteryMv;
  final int heapFree;
  RiftLinkSelftestEvent({
    required this.radioOk,
    required this.displayOk,
    required this.batteryMv,
    required this.heapFree,
  });
}

class RiftLinkRoutesEvent extends RiftLinkEvent {
  final List<Map<String, dynamic>> routes;  // dest, nextHop, hops, rssi
  RiftLinkRoutesEvent({required this.routes});
}

class RiftLinkGroupsEvent extends RiftLinkEvent {
  final List<int> groups;
  RiftLinkGroupsEvent({required this.groups});
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

class RiftLinkOtaEvent extends RiftLinkEvent {
  final String ip;
  final String ssid;
  final String password;
  RiftLinkOtaEvent({
    required this.ip,
    required this.ssid,
    required this.password,
  });
}

class RiftLinkPongEvent extends RiftLinkEvent {
  final String from;
  RiftLinkPongEvent({required this.from});
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
