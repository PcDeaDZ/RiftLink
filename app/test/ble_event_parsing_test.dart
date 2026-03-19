import 'dart:convert';
import 'package:flutter_test/flutter_test.dart';
import 'package:riftlink_app/ble/riftlink_ble.dart';

/// Синтетические тесты парсинга BLE событий (JSON -> RiftLinkEvent).
/// Логика парсинга дублирована для изоляции от BLE.
RiftLinkEvent? parseEvent(Map<String, dynamic> json) {
  final evt = json['evt'] as String?;
  if (evt == 'msg') {
    return RiftLinkMsgEvent(
      from: json['from'] as String? ?? '',
      text: json['text'] as String? ?? '',
      msgId: (json['msgId'] as num?)?.toInt(),
      rssi: (json['rssi'] as num?)?.toInt(),
      ttlMinutes: (json['ttl'] as num?)?.toInt(),
    );
  }
  if (evt == 'invite') {
    return RiftLinkInviteEvent(
      id: json['id'] as String? ?? '',
      pubKey: json['pubKey'] as String? ?? '',
      channelKey: json['channelKey'] as String?,
    );
  }
  if (evt == 'info') {
    final neighborsRssiRaw = json['neighborsRssi'] ?? json['rssi'];
    List<int> neighborsRssi = [];
    if (neighborsRssiRaw is List) {
      for (final x in neighborsRssiRaw) {
        if (x is num) neighborsRssi.add(x.toInt());
      }
    }
    return RiftLinkInfoEvent(
      id: json['id'] as String? ?? '',
      nickname: json['nickname'] as String?,
      region: json['region'] as String? ?? 'EU',
      neighbors: (json['neighbors'] as List<dynamic>?)?.cast<String>() ?? [],
      neighborsRssi: neighborsRssi,
    );
  }
  if (evt == 'error') {
    return RiftLinkErrorEvent(
      code: json['code'] as String? ?? 'unknown',
      msg: json['msg'] as String? ?? '',
    );
  }
  return null;
}

void main() {
  group('parseEvent: msg', () {
    test('parses msg event', () {
      final json = jsonDecode('{"evt":"msg","from":"A1B2C3D4","text":"Hello","msgId":42}') as Map<String, dynamic>;
      final e = parseEvent(json);
      expect(e, isA<RiftLinkMsgEvent>());
      final m = e as RiftLinkMsgEvent;
      expect(m.from, 'A1B2C3D4');
      expect(m.text, 'Hello');
      expect(m.msgId, 42);
    });

    test('parses msg with missing fields', () {
      final json = jsonDecode('{"evt":"msg"}') as Map<String, dynamic>;
      final e = parseEvent(json);
      expect(e, isA<RiftLinkMsgEvent>());
      final m = e as RiftLinkMsgEvent;
      expect(m.from, '');
      expect(m.text, '');
      expect(m.msgId, isNull);
    });
  });

  group('parseEvent: invite', () {
    test('parses invite with channelKey', () {
      final json = jsonDecode('{"evt":"invite","id":"A1B2C3D4","pubKey":"pk","channelKey":"ck"}') as Map<String, dynamic>;
      final e = parseEvent(json);
      expect(e, isA<RiftLinkInviteEvent>());
      final i = e as RiftLinkInviteEvent;
      expect(i.id, 'A1B2C3D4');
      expect(i.pubKey, 'pk');
      expect(i.channelKey, 'ck');
    });

    test('parses invite without channelKey', () {
      final json = jsonDecode('{"evt":"invite","id":"X","pubKey":"Y"}') as Map<String, dynamic>;
      final e = parseEvent(json);
      expect(e, isA<RiftLinkInviteEvent>());
      final i = e as RiftLinkInviteEvent;
      expect(i.channelKey, isNull);
    });
  });

  group('parseEvent: info', () {
    test('parses info with neighborsRssi', () {
      final json = jsonDecode('{"evt":"info","id":"N1","neighbors":["A","B"],"neighborsRssi":[-45,-60]}') as Map<String, dynamic>;
      final e = parseEvent(json);
      expect(e, isA<RiftLinkInfoEvent>());
      final i = e as RiftLinkInfoEvent;
      expect(i.neighbors, ['A', 'B']);
      expect(i.neighborsRssi, [-45, -60]);
    });

    test('parses info with rssi fallback', () {
      final json = jsonDecode('{"evt":"info","id":"N1","neighbors":["A"],"rssi":[-50]}') as Map<String, dynamic>;
      final e = parseEvent(json);
      expect(e, isA<RiftLinkInfoEvent>());
      final i = e as RiftLinkInfoEvent;
      expect(i.neighborsRssi, [-50]);
    });
  });

  group('parseEvent: error', () {
    test('parses error event', () {
      final json = jsonDecode('{"evt":"error","code":"ble","msg":"Connection lost"}') as Map<String, dynamic>;
      final e = parseEvent(json);
      expect(e, isA<RiftLinkErrorEvent>());
      final err = e as RiftLinkErrorEvent;
      expect(err.code, 'ble');
      expect(err.msg, 'Connection lost');
    });
  });

  group('parseEvent: unknown', () {
    test('returns null for unknown evt', () {
      final json = jsonDecode('{"evt":"unknown"}') as Map<String, dynamic>;
      expect(parseEvent(json), isNull);
    });

    test('returns null for missing evt', () {
      final json = jsonDecode('{"foo":"bar"}') as Map<String, dynamic>;
      expect(parseEvent(json), isNull);
    });
  });
}
