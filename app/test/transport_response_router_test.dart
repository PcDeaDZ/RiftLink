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

    test('pong without cmdId completes pending ping matched by from vs to', () async {
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
  });
}
