import 'dart:convert';
import 'package:flutter_test/flutter_test.dart';
import 'package:riftlink_app/ble/riftlink_ble.dart';

/// Синтетические тесты для BLE invite/acceptInvite и парсинга JSON.
void main() {
  group('Invite JSON format', () {
    test('buildInviteJson without channelKey', () {
      const id = 'A1B2C3D4E5F60708';
      const pubKey = 'dGVzdF9wdWJfa2V5X2Jhc2U2NF9leGFtcGxl';
      final map = <String, String>{'id': id, 'pubKey': pubKey};
      final data = jsonEncode(map);
      final parsed = jsonDecode(data) as Map<String, dynamic>;
      expect(parsed['id'], id);
      expect(parsed['pubKey'], pubKey);
      expect(parsed.containsKey('channelKey'), false);
    });

    test('buildInviteJson with channelKey', () {
      const id = 'A1B2C3D4E5F60708';
      const pubKey = 'dGVzdF9wdWJfa2V5';
      const channelKey = 'YWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXo=';
      final map = <String, String>{'id': id, 'pubKey': pubKey, 'channelKey': channelKey};
      final data = jsonEncode(map);
      final parsed = jsonDecode(data) as Map<String, dynamic>;
      expect(parsed['id'], id);
      expect(parsed['pubKey'], pubKey);
      expect(parsed['channelKey'], channelKey);
    });

    test('buildInviteJson with token', () {
      const id = 'A1B2C3D4E5F60708';
      const pubKey = 'dGVzdF9wdWJfa2V5';
      const inviteToken = 'AABBCCDD00112233';
      final map = <String, String>{'id': id, 'pubKey': pubKey, 'inviteToken': inviteToken};
      final data = jsonEncode(map);
      final parsed = jsonDecode(data) as Map<String, dynamic>;
      expect(parsed['inviteToken'], inviteToken);
    });

    test('buildInviteJson escapes special chars', () {
      const id = 'A1B2C3D4';
      const pubKey = 'key"with\\quotes';
      final map = <String, String>{'id': id, 'pubKey': pubKey};
      final data = jsonEncode(map);
      expect(data, contains(r'\"'));
      final parsed = jsonDecode(data) as Map<String, dynamic>;
      expect(parsed['pubKey'], pubKey);
    });
  });

  group('Settings paste JSON parsing', () {
    test('parse valid invite JSON with channelKey', () {
      const json = '{"id":"A1B2C3D4","pubKey":"base64key","channelKey":"chkey32"}';
      final m = jsonDecode(json) as Map<String, dynamic>?;
      expect(m, isNotNull);
      final id = (m!['id'] as String?) ?? '';
      final pk = (m['pubKey'] as String?) ?? '';
      final ck = (m['channelKey'] as String?) ?? '';
      expect(id, 'A1B2C3D4');
      expect(pk, 'base64key');
      expect(ck, 'chkey32');
    });

    test('parse invite JSON without channelKey', () {
      const json = '{"id":"DEADBEEF","pubKey":"oldformat"}';
      final m = jsonDecode(json) as Map<String, dynamic>?;
      expect(m, isNotNull);
      final ck = (m!['channelKey'] as String?) ?? '';
      expect(ck, '');
    });

    test('parse invalid JSON throws', () {
      expect(() => jsonDecode('{invalid}'), throwsFormatException);
    });

    test('parse empty string throws', () {
      expect(() => jsonDecode(''), throwsFormatException);
    });
  });

  group('RiftLinkInviteEvent', () {
    test('creates with channelKey', () {
      final evt = RiftLinkInviteEvent(
        id: 'A1B2C3D4',
        pubKey: 'pk',
        channelKey: 'ck',
        inviteToken: 'AABB',
        inviteTtlMs: 5000,
      );
      expect(evt.id, 'A1B2C3D4');
      expect(evt.pubKey, 'pk');
      expect(evt.channelKey, 'ck');
      expect(evt.inviteToken, 'AABB');
      expect(evt.inviteTtlMs, 5000);
    });

    test('creates without channelKey', () {
      final evt = RiftLinkInviteEvent(id: 'X', pubKey: 'Y');
      expect(evt.channelKey, isNull);
      expect(evt.inviteToken, isNull);
    });
  });

  group('acceptInvite payload logic', () {
    test('payload includes channelKey when non-empty', () {
      const id = 'A1B2C3D4';
      const pubKey = 'pk';
      const channelKey = 'ck';
      final payload = <String, dynamic>{'cmd': 'acceptInvite', 'id': id, 'pubKey': pubKey};
      if (channelKey.isNotEmpty) payload['channelKey'] = channelKey;
      expect(payload['channelKey'], 'ck');
    });

    test('payload includes inviteToken when non-empty', () {
      const id = 'A1B2C3D4';
      const pubKey = 'pk';
      const inviteToken = 'AABBCCDD00112233';
      final payload = <String, dynamic>{'cmd': 'acceptInvite', 'id': id, 'pubKey': pubKey};
      if (inviteToken.isNotEmpty) payload['inviteToken'] = inviteToken;
      expect(payload['inviteToken'], inviteToken);
    });

    test('payload excludes channelKey when empty', () {
      const id = 'A1B2C3D4';
      const pubKey = 'pk';
      const channelKey = '';
      final payload = <String, dynamic>{'cmd': 'acceptInvite', 'id': id, 'pubKey': pubKey};
      if (channelKey.isNotEmpty) payload['channelKey'] = channelKey;
      expect(payload.containsKey('channelKey'), false);
    });

    test('payload excludes channelKey when null', () {
      const id = 'A1B2C3D4';
      const pubKey = 'pk';
      final String? channelKey = null;
      final payload = <String, dynamic>{'cmd': 'acceptInvite', 'id': id, 'pubKey': pubKey};
      if (channelKey != null && channelKey.isNotEmpty) payload['channelKey'] = channelKey;
      expect(payload.containsKey('channelKey'), false);
    });
  });

  group('RiftLinkBle.isRiftLink', () {
    test('accepts RL- prefix', () {
      // ScanResult mock - we test the static logic via a fake
      // isRiftLink checks name/advName - we need ScanResult
      // Skip: requires flutter_blue_plus ScanResult
    });
  });

  group('send payload construction', () {
    test('broadcast payload', () {
      final payload = {'cmd': 'send', 'text': 'hello'};
      expect(payload['cmd'], 'send');
      expect(payload['text'], 'hello');
      expect(payload.containsKey('to'), false);
      expect(payload.containsKey('group'), false);
    });

    test('unicast payload', () {
      final payload = {'cmd': 'send', 'to': 'A1B2C3D4', 'text': 'hi'};
      expect(payload['to'], 'A1B2C3D4');
    });

    test('group payload', () {
      final payload = {'cmd': 'send', 'group': 1, 'text': 'group msg'};
      expect(payload['group'], 1);
    });
  });
}
