import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:riftlink_app/transport/transport_response_router.dart';

void main() {
  group('TransportResponseRouter', () {
    test('completes on expected event with matching cmdId', () async {
      final responses = StreamController<Map<String, dynamic>>.broadcast();
      final sentPayloads = <Map<String, dynamic>>[];
      final router = TransportResponseRouter(
        sendCommand: (payload) async {
          sentPayloads.add(Map<String, dynamic>.from(payload));
          return true;
        },
        responses: responses.stream,
      );

      final future = router.sendRequest(
        cmd: 'info',
        expectedEvents: const {'info'},
        timeout: const Duration(milliseconds: 200),
      );
      await Future<void>.delayed(const Duration(milliseconds: 10));
      final cmdId = sentPayloads.single['cmdId'] as int;
      responses.add({'evt': 'info', 'cmdId': cmdId, 'id': 'NODE'});

      final result = await future;
      expect(result['evt'], 'info');
      expect(result['cmdId'], cmdId);

      await router.dispose();
      await responses.close();
    });

    test('completes with error when evt:error is received', () async {
      final responses = StreamController<Map<String, dynamic>>.broadcast();
      final sentPayloads = <Map<String, dynamic>>[];
      final router = TransportResponseRouter(
        sendCommand: (payload) async {
          sentPayloads.add(Map<String, dynamic>.from(payload));
          return true;
        },
        responses: responses.stream,
      );

      final future = router.sendRequest(
        cmd: 'routes',
        expectedEvents: const {'routes'},
        timeout: const Duration(milliseconds: 200),
      );
      await Future<void>.delayed(const Duration(milliseconds: 10));
      final cmdId = sentPayloads.single['cmdId'] as int;
      responses.add({'evt': 'error', 'cmdId': cmdId, 'code': 'payload_too_long'});

      await expectLater(
        future,
        throwsA(isA<StateError>()),
      );

      await router.dispose();
      await responses.close();
    });

    test('cmd:info resets deadline when routes arrive before node (sliding window)', () async {
      final responses = StreamController<Map<String, dynamic>>.broadcast();
      final sentPayloads = <Map<String, dynamic>>[];
      final router = TransportResponseRouter(
        sendCommand: (payload) async {
          sentPayloads.add(Map<String, dynamic>.from(payload));
          return true;
        },
        responses: responses.stream,
      );

      final future = router.sendRequest(
        cmd: 'info',
        expectedEvents: const {'node', 'neighbors'},
        timeout: const Duration(milliseconds: 150),
      );
      await Future<void>.delayed(const Duration(milliseconds: 5));
      final cmdId = sentPayloads.single['cmdId'] as int;
      // Без продления таймера node пришёл бы после первого окна 150 мс.
      responses.add({'evt': 'routes', 'cmdId': cmdId, 'routes': <dynamic>[]});
      await Future<void>.delayed(const Duration(milliseconds: 120));
      responses.add({'evt': 'node', 'cmdId': cmdId, 'id': 'AABBCCDDEEFF0011'});

      final result = await future;
      expect(result['evt'], 'node');

      await router.dispose();
      await responses.close();
    });

    test('new cmd:info supersedes older pending info (firmware keeps one cmdId)', () async {
      final responses = StreamController<Map<String, dynamic>>.broadcast();
      final router = TransportResponseRouter(
        sendCommand: (payload) async => true,
        responses: responses.stream,
      );

      final t1 = router.sendTrackedRequest(
        cmd: 'info',
        expectedEvents: const {'node'},
        timeout: const Duration(seconds: 30),
      );
      await Future<void>.delayed(const Duration(milliseconds: 5));
      final t2 = router.sendTrackedRequest(
        cmd: 'info',
        expectedEvents: const {'node'},
        timeout: const Duration(seconds: 30),
      );

      Object? err1;
      try {
        await t1.response;
      } catch (e) {
        err1 = e;
      }
      expect(err1, isA<StateError>());
      expect((err1 as StateError).message, startsWith('info_superseded'));

      await Future<void>.delayed(const Duration(milliseconds: 5));
      final cmdId2 = t2.cmdId;
      responses.add({'evt': 'node', 'cmdId': cmdId2, 'id': 'AABBCCDDEEFF0011'});
      final r2 = await t2.response;
      expect(r2['evt'], 'node');

      await router.dispose();
      await responses.close();
    });

    test('cmd:info with node+neighbors does not complete on routes alone', () async {
      final responses = StreamController<Map<String, dynamic>>.broadcast();
      final sentPayloads = <Map<String, dynamic>>[];
      final router = TransportResponseRouter(
        sendCommand: (payload) async {
          sentPayloads.add(Map<String, dynamic>.from(payload));
          return true;
        },
        responses: responses.stream,
      );

      final future = router.sendRequest(
        cmd: 'info',
        expectedEvents: const {'node', 'neighbors'},
        timeout: const Duration(milliseconds: 120),
      );
      await Future<void>.delayed(const Duration(milliseconds: 10));
      final cmdId = sentPayloads.single['cmdId'] as int;
      responses.add({'evt': 'routes', 'cmdId': cmdId, 'routes': <dynamic>[]});
      await Future<void>.delayed(const Duration(milliseconds: 10));
      responses.add({'evt': 'node', 'cmdId': cmdId, 'id': 'AABBCCDDEEFF0011'});

      final result = await future;
      expect(result['evt'], 'node');
      expect(result['id'], 'AABBCCDDEEFF0011');

      await router.dispose();
      await responses.close();
    });

    test('ignores non-expected evt and times out', () async {
      final responses = StreamController<Map<String, dynamic>>.broadcast();
      final sentPayloads = <Map<String, dynamic>>[];
      final router = TransportResponseRouter(
        sendCommand: (payload) async {
          sentPayloads.add(Map<String, dynamic>.from(payload));
          return true;
        },
        responses: responses.stream,
      );

      final future = router.sendRequest(
        cmd: 'routes',
        expectedEvents: const {'routes'},
        timeout: const Duration(milliseconds: 40),
      );
      await Future<void>.delayed(const Duration(milliseconds: 10));
      final cmdId = sentPayloads.single['cmdId'] as int;
      responses.add({'evt': 'neighbors', 'cmdId': cmdId});

      await expectLater(
        future,
        throwsA(isA<TimeoutException>()),
      );

      await router.dispose();
      await responses.close();
    });

    test('pong with matching cmdId completes pending ping', () async {
      final responses = StreamController<Map<String, dynamic>>.broadcast();
      final sentPayloads = <Map<String, dynamic>>[];
      final router = TransportResponseRouter(
        sendCommand: (payload) async {
          sentPayloads.add(Map<String, dynamic>.from(payload));
          return true;
        },
        responses: responses.stream,
      );

      final ticket = router.sendTrackedRequest(
        cmd: 'ping',
        payload: <String, dynamic>{'to': '0011223344556677'},
        expectedEvents: const {'pong'},
        timeout: const Duration(milliseconds: 400),
      );
      await Future<void>.delayed(const Duration(milliseconds: 10));
      final cmdId = sentPayloads.single['cmdId'] as int;
      responses.add(<String, dynamic>{
        'evt': 'pong',
        'cmdId': cmdId,
        'from': '0011223344556677',
        'rssi': -70,
      });

      final result = await ticket.response;
      expect(result['evt'], 'pong');
      expect(result['cmdId'], cmdId);
      expect(result['from'], '0011223344556677');

      await router.dispose();
      await responses.close();
    });

    test('repeats send up to sendAttempts when BLE write fails', () async {
      final responses = StreamController<Map<String, dynamic>>.broadcast();
      var sendCount = 0;
      final router = TransportResponseRouter(
        sendCommand: (payload) async {
          sendCount++;
          return false;
        },
        responses: responses.stream,
      );

      final future = router.sendRequest(
        cmd: 'routes',
        expectedEvents: const {'routes'},
        timeout: const Duration(milliseconds: 500),
        sendAttempts: 3,
      );

      await expectLater(
        future,
        throwsA(isA<StateError>()),
      );
      expect(sendCount, 3);

      await router.dispose();
      await responses.close();
    });
  });
}
