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
      lane: json['lane'] as String? ?? 'normal',
      type: json['type'] as String? ?? 'text',
    );
  }
  if (evt == 'invite') {
    return RiftLinkInviteEvent(
      id: json['id'] as String? ?? '',
      pubKey: json['pubKey'] as String? ?? '',
      channelKey: json['channelKey'] as String?,
      inviteToken: json['inviteToken'] as String?,
      inviteTtlMs: (json['inviteTtlMs'] as num?)?.toInt(),
    );
  }
  if (evt == 'relayProof') {
    return RiftLinkRelayProofEvent(
      relayedBy: json['relayedBy'] as String? ?? '',
      from: json['from'] as String? ?? '',
      to: json['to'] as String? ?? '',
      pktId: (json['pktId'] as num?)?.toInt() ?? 0,
      opcode: (json['opcode'] as num?)?.toInt() ?? 0,
    );
  }
  if (evt == 'timeCapsuleQueued') {
    return RiftLinkTimeCapsuleQueuedEvent(
      to: json['to'] as String?,
      trigger: json['trigger'] as String? ?? '',
      triggerAtMs: (json['triggerAtMs'] as num?)?.toInt(),
    );
  }
  if (evt == 'timeCapsuleReleased') {
    return RiftLinkTimeCapsuleReleasedEvent(
      to: json['to'] as String? ?? '',
      msgId: (json['msgId'] as num?)?.toInt() ?? 0,
      trigger: json['trigger'] as String? ?? '',
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
      hasOfflinePendingField: json.containsKey('offlinePending'),
      hasOfflineCourierPendingField: json.containsKey('offlineCourierPending'),
      hasOfflineDirectPendingField: json.containsKey('offlineDirectPending'),
      region: json['region'] as String? ?? 'EU',
      neighbors: (json['neighbors'] as List<dynamic>?)?.cast<String>() ?? [],
      neighborsRssi: neighborsRssi,
      offlinePending: (json['offlinePending'] as num?)?.toInt(),
      offlineCourierPending: (json['offlineCourierPending'] as num?)?.toInt(),
      offlineDirectPending: (json['offlineDirectPending'] as num?)?.toInt(),
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
      expect(m.lane, 'normal');
      expect(m.type, 'text');
    });

    test('parses msg with missing fields', () {
      final json = jsonDecode('{"evt":"msg"}') as Map<String, dynamic>;
      final e = parseEvent(json);
      expect(e, isA<RiftLinkMsgEvent>());
      final m = e as RiftLinkMsgEvent;
      expect(m.from, '');
      expect(m.text, '');
      expect(m.msgId, isNull);
      expect(m.lane, 'normal');
      expect(m.type, 'text');
    });

    test('parses msg lane/type', () {
      final json = jsonDecode('{"evt":"msg","from":"A1","text":"SOS","lane":"critical","type":"sos"}') as Map<String, dynamic>;
      final e = parseEvent(json) as RiftLinkMsgEvent;
      expect(e.lane, 'critical');
      expect(e.type, 'sos');
    });
  });

  group('parseEvent: invite', () {
    test('parses invite with channelKey', () {
      final json = jsonDecode('{"evt":"invite","id":"A1B2C3D4","pubKey":"pk","channelKey":"ck","inviteToken":"AABBCCDD00112233","inviteTtlMs":600000}') as Map<String, dynamic>;
      final e = parseEvent(json);
      expect(e, isA<RiftLinkInviteEvent>());
      final i = e as RiftLinkInviteEvent;
      expect(i.id, 'A1B2C3D4');
      expect(i.pubKey, 'pk');
      expect(i.channelKey, 'ck');
      expect(i.inviteToken, 'AABBCCDD00112233');
      expect(i.inviteTtlMs, 600000);
    });

    test('parses invite without channelKey', () {
      final json = jsonDecode('{"evt":"invite","id":"X","pubKey":"Y"}') as Map<String, dynamic>;
      final e = parseEvent(json);
      expect(e, isA<RiftLinkInviteEvent>());
      final i = e as RiftLinkInviteEvent;
      expect(i.channelKey, isNull);
    });
  });

  group('parseEvent: relay/timecapsule', () {
    test('parses relayProof', () {
      final json = jsonDecode('{"evt":"relayProof","relayedBy":"R1","from":"A1","to":"B2","pktId":7,"opcode":1}') as Map<String, dynamic>;
      final e = parseEvent(json);
      expect(e, isA<RiftLinkRelayProofEvent>());
      final r = e as RiftLinkRelayProofEvent;
      expect(r.pktId, 7);
      expect(r.opcode, 1);
    });

    test('parses timeCapsuleQueued', () {
      final json = jsonDecode('{"evt":"timeCapsuleQueued","to":"A1","trigger":"deliver_after_time","triggerAtMs":123}') as Map<String, dynamic>;
      final e = parseEvent(json);
      expect(e, isA<RiftLinkTimeCapsuleQueuedEvent>());
      final q = e as RiftLinkTimeCapsuleQueuedEvent;
      expect(q.trigger, 'deliver_after_time');
      expect(q.triggerAtMs, 123);
    });

    test('parses timeCapsuleReleased', () {
      final json = jsonDecode('{"evt":"timeCapsuleReleased","to":"A1","msgId":77,"trigger":"target_online"}') as Map<String, dynamic>;
      final e = parseEvent(json);
      expect(e, isA<RiftLinkTimeCapsuleReleasedEvent>());
      final r = e as RiftLinkTimeCapsuleReleasedEvent;
      expect(r.to, 'A1');
      expect(r.msgId, 77);
      expect(r.trigger, 'target_online');
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

    test('parses info offline queue split fields', () {
      final json = jsonDecode('{"evt":"info","id":"N1","offlinePending":5,"offlineCourierPending":2,"offlineDirectPending":3}') as Map<String, dynamic>;
      final e = parseEvent(json) as RiftLinkInfoEvent;
      expect(e.offlinePending, 5);
      expect(e.offlineCourierPending, 2);
      expect(e.offlineDirectPending, 3);
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
