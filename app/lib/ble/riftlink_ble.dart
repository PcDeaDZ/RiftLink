/// RiftLink BLE — связь с Heltec LoRa по GATT
/// Протокол: JSON {"cmd":"send","text":"..."} / {"evt":"msg","from":"...","text":"..."}

import 'dart:convert';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

class RiftLinkBle {
  static const serviceUuid = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
  static const charTxUuid = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
  static const charRxUuid = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';
  static const deviceName = 'RiftLink';

  BluetoothDevice? _device;
  BluetoothCharacteristic? _txChar;
  BluetoothCharacteristic? _rxChar;

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

  Future<bool> _sendCmd(Map<String, dynamic> payload) async {
    if (_txChar == null || !isConnected) return false;
    final json = jsonEncode(payload);
    await _txChar!.write(utf8.encode(json), withoutResponse: true);
    return true;
  }

  /// Подключение к устройству
  Future<bool> connect(BluetoothDevice dev) async {
    await disconnect();
    _device = dev;
    await dev.connect();
    await dev.discoverServices();

    final service = dev.servicesList
        .where((s) => s.uuid.toString().toLowerCase() == serviceUuid)
        .firstOrNull;
    if (service == null) return false;

    _txChar = service.characteristics
        .where((c) => c.uuid.toString().toLowerCase() == charTxUuid)
        .firstOrNull;
    _rxChar = service.characteristics
        .where((c) => c.uuid.toString().toLowerCase() == charRxUuid)
        .firstOrNull;

    if (_txChar == null || _rxChar == null) return false;
    getInfo();
    getGroups();
    getRoutes();
    return true;
  }

  /// Запросить info (evt "info")
  Future<bool> getInfo() async => _sendCmd({'cmd': 'info'});

  Future<void> disconnect() async {
    if (_device != null) {
      await _device!.disconnect();
      _device = null;
    }
    _txChar = null;
    _rxChar = null;
  }

  /// Отправка геолокации (broadcast)
  Future<bool> sendLocation({required double lat, required double lon, int alt = 0}) async =>
      _sendCmd({'cmd': 'location', 'lat': lat, 'lon': lon, 'alt': alt});

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

  /// Принять invite (id + pubKey base64)
  Future<bool> acceptInvite({required String id, required String pubKey}) async =>
      _sendCmd({'cmd': 'acceptInvite', 'id': id, 'pubKey': pubKey});

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

  /// Подписка на уведомления (входящие сообщения)
  Stream<RiftLinkEvent> get events async* {
    if (_rxChar == null) return;

    await _rxChar!.setNotifyValue(true);

    await for (final value in _rxChar!.lastValueStream) {
      if (value.isEmpty) continue;
      try {
        final json = jsonDecode(utf8.decode(value)) as Map<String, dynamic>;
        final evt = json['evt'] as String?;
        if (evt == 'msg') {
          yield RiftLinkMsgEvent(
            from: json['from'] as String? ?? '',
            text: json['text'] as String? ?? '',
            msgId: (json['msgId'] as num?)?.toInt(),
            rssi: (json['rssi'] as num?)?.toInt(),
            ttlMinutes: (json['ttl'] as num?)?.toInt(),
          );
        } else if (evt == 'sent') {
          yield RiftLinkSentEvent(
            to: json['to'] as String? ?? '',
            msgId: (json['msgId'] as num?)?.toInt() ?? 0,
          );
        } else if (evt == 'delivered') {
          yield RiftLinkDeliveredEvent(
            from: json['from'] as String? ?? '',
            msgId: (json['msgId'] as num?)?.toInt() ?? 0,
          );
        } else if (evt == 'read') {
          yield RiftLinkReadEvent(
            from: json['from'] as String? ?? '',
            msgId: (json['msgId'] as num?)?.toInt() ?? 0,
          );
        } else if (evt == 'info') {
          final nb = json['neighbors'];
          final neighbors = nb is List ? (nb as List).map((e) => e.toString()).toList() : <String>[];
          final rssiList = json['neighborsRssi'];
          final neighborsRssi = rssiList is List ? (rssiList as List).map((e) => (e as num).toInt()).toList() : <int>[];
          final hasKeyList = json['neighborsHasKey'];
          final neighborsHasKey = hasKeyList is List ? (hasKeyList as List).map((e) => e == true).toList() : <bool>[];
          final grpList = json['groups'];
          final groups = grpList is List ? (grpList as List).map((e) => (e as num).toInt()).toList() : <int>[];
          final routesList = json['routes'];
          final routes = <Map<String, dynamic>>[];
          if (routesList is List) {
            for (final r in routesList) {
              if (r is Map<String, dynamic>) {
                routes.add({
                  'dest': r['dest'] as String? ?? '',
                  'nextHop': r['nextHop'] as String? ?? '',
                  'hops': (r['hops'] as num?)?.toInt() ?? 0,
                  'rssi': (r['rssi'] as num?)?.toInt() ?? 0,
                });
              }
            }
          }
          yield RiftLinkInfoEvent(
            id: json['id'] as String? ?? '',
            nickname: json['nickname'] as String?,
            region: json['region'] as String? ?? 'EU',
            freq: (json['freq'] as num?)?.toDouble() ?? 868.0,
            power: (json['power'] as num?)?.toInt() ?? 14,
            channel: (json['channel'] as num?)?.toInt(),
            version: json['version'] as String?,
            neighbors: neighbors,
            neighborsRssi: neighborsRssi,
            neighborsHasKey: neighborsHasKey,
            groups: groups,
            routes: routes,
            sf: (json['sf'] as num?)?.toInt(),
            offlinePending: (json['offlinePending'] as num?)?.toInt(),
            gpsPresent: json['gpsPresent'] == true,
            gpsEnabled: json['gpsEnabled'] == true,
            gpsFix: json['gpsFix'] == true,
            powersave: json['powersave'] == true,
          );
        } else if (evt == 'routes') {
          final routesList = json['routes'];
          final routes = <Map<String, dynamic>>[];
          if (routesList is List) {
            for (final r in routesList) {
              if (r is Map<String, dynamic>) {
                routes.add({
                  'dest': r['dest'] as String? ?? '',
                  'nextHop': r['nextHop'] as String? ?? '',
                  'hops': (r['hops'] as num?)?.toInt() ?? 0,
                  'rssi': (r['rssi'] as num?)?.toInt() ?? 0,
                });
              }
            }
          }
          yield RiftLinkRoutesEvent(routes: routes);
        } else if (evt == 'groups') {
          final grpList = json['groups'];
          yield RiftLinkGroupsEvent(
            groups: grpList is List ? (grpList as List).map((e) => (e as num).toInt()).toList() : <int>[],
          );
        } else if (evt == 'telemetry') {
          final bat = (json['battery'] as num?)?.toInt() ?? 0;
          final heap = (json['heapKb'] as num?)?.toInt() ?? 0;
          yield RiftLinkTelemetryEvent(
            from: json['from'] as String? ?? '',
            batteryMv: bat,
            heapKb: heap,
          );
        } else if (evt == 'location') {
          final lat = (json['lat'] as num?)?.toDouble() ?? 0.0;
          final lon = (json['lon'] as num?)?.toDouble() ?? 0.0;
          final alt = (json['alt'] as num?)?.toInt() ?? 0;
          yield RiftLinkLocationEvent(
            from: json['from'] as String? ?? '',
            lat: lat,
            lon: lon,
            alt: alt,
          );
        } else if (evt == 'region') {
          yield RiftLinkRegionEvent(
            region: json['region'] as String? ?? 'EU',
            freq: (json['freq'] as num?)?.toDouble() ?? 868.0,
            power: (json['power'] as num?)?.toInt() ?? 14,
            channel: (json['channel'] as num?)?.toInt(),
          );
        } else if (evt == 'ota') {
          yield RiftLinkOtaEvent(
            ip: json['ip'] as String? ?? '192.168.4.1',
            ssid: json['ssid'] as String? ?? 'RiftLink-OTA',
            password: json['password'] as String? ?? 'riftlink123',
          );
        } else if (evt == 'neighbors') {
          final list = json['neighbors'];
          final rssiList = json['rssi'];
          final rssi = rssiList is List ? (rssiList as List).map((e) => (e as num).toInt()).toList() : <int>[];
          final hasKeyList = json['hasKey'];
          final hasKey = hasKeyList is List ? (hasKeyList as List).map((e) => e == true).toList() : <bool>[];
          yield RiftLinkNeighborsEvent(
            neighbors: list is List ? (list as List).map((e) => e.toString()).toList() : [],
            rssi: rssi,
            hasKey: hasKey,
          );
        } else if (evt == 'pong') {
          yield RiftLinkPongEvent(from: json['from'] as String? ?? '');
        } else if (evt == 'error') {
          yield RiftLinkErrorEvent(
            code: json['code'] as String? ?? 'unknown',
            msg: json['msg'] as String? ?? '',
          );
        } else if (evt == 'voice') {
          yield RiftLinkVoiceEvent(
            from: json['from'] as String? ?? '',
            chunk: (json['chunk'] as num?)?.toInt() ?? 0,
            total: (json['total'] as num?)?.toInt() ?? 1,
            data: json['data'] as String? ?? '',
          );
        } else if (evt == 'wifi') {
          yield RiftLinkWifiEvent(
            connected: json['connected'] == true,
            ssid: json['ssid'] as String? ?? '',
            ip: json['ip'] as String? ?? '',
          );
        } else if (evt == 'gps') {
          yield RiftLinkGpsEvent(
            present: json['present'] == true,
            enabled: json['enabled'] == true,
            hasFix: json['hasFix'] == true,
          );
        } else if (evt == 'invite') {
          yield RiftLinkInviteEvent(
            id: json['id'] as String? ?? '',
            pubKey: json['pubKey'] as String? ?? '',
          );
        } else if (evt == 'selftest') {
          yield RiftLinkSelftestEvent(
            radioOk: json['radioOk'] == true,
            displayOk: json['displayOk'] == true,
            batteryMv: (json['batteryMv'] as num?)?.toInt() ?? 0,
            heapFree: (json['heapFree'] as num?)?.toInt() ?? 0,
          );
        }
      } catch (_) {}
    }
  }
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

class RiftLinkInfoEvent extends RiftLinkEvent {
  final String id;
  final String? nickname;
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
  final bool gpsPresent;
  final bool gpsEnabled;
  final bool gpsFix;
  final bool powersave;
  RiftLinkInfoEvent({
    required this.id,
    this.nickname,
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
  RiftLinkInviteEvent({required this.id, required this.pubKey});
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
