import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:riftlink_app/ble/riftlink_ble.dart';
import 'package:riftlink_app/screens/chat_screen.dart';

/// Fake BLE для тестов (минимальный интерфейс)
class FakeRiftLinkBle implements RiftLinkBle {
  bool connected = false;
  @override
  bool get isConnected => connected;
  @override
  BluetoothDevice? get device => null;
  @override
  Stream<RiftLinkEvent> get events => const Stream.empty();
  @override
  Future<bool> connect(BluetoothDevice dev) async => false;
  @override
  Future<void> disconnect() async {}
  @override
  Future<bool> send({String? to, int? group, required String text, int ttlMinutes = 0}) async => false;
  @override
  Future<bool> sendLocation({required double lat, required double lon, int alt = 0}) async => false;
  @override
  Future<bool> setRegion(String region) async => false;
  @override
  Future<bool> setNickname(String nickname) async => false;
  @override
  Future<bool> setChannel(int channel) async => false;
  @override
  Future<bool> sendPing(String to) async => false;
  @override
  Future<bool> sendOta() async => false;
  @override
  Future<bool> sendVoice({required String to, required List<String> chunks}) async => false;
  @override
  Future<bool> sendRead({required String from, required int msgId}) async => false;
  @override
  Future<bool> getGroups() async => false;
  @override
  Future<bool> addGroup(int groupId) async => false;
  @override
  Future<bool> removeGroup(int groupId) async => false;
}

void main() {
  group('RiftLinkBle events', () {
    test('RiftLinkInfoEvent parses neighbors', () {
      final evt = RiftLinkInfoEvent(
        id: 'A1B2C3D4E5F60708',
        nickname: 'Alice',
        region: 'EU',
        freq: 868.1,
        power: 14,
        channel: 0,
        version: '0.5.0',
        neighbors: ['B2C3D4E5F6070819'],
      );
      expect(evt.id, 'A1B2C3D4E5F60708');
      expect(evt.nickname, 'Alice');
      expect(evt.neighbors.length, 1);
      expect(evt.neighbors.first, 'B2C3D4E5F6070819');
    });

    test('RiftLinkNeighborsEvent', () {
      final evt = RiftLinkNeighborsEvent(neighbors: ['A1', 'B2']);
      expect(evt.neighbors.length, 2);
    });
  });

  group('ChatScreen', () {
    testWidgets('renders when disconnected', (tester) async {
      final ble = FakeRiftLinkBle();
      ble.connected = false;

      await tester.pumpWidget(
        MaterialApp(
          home: ChatScreen(ble: ble),
        ),
      );

      expect(find.text('RiftLink'), findsOneWidget);
      expect(find.text('Отключено'), findsOneWidget);
    });

    testWidgets('shows message input and send button', (tester) async {
      final ble = FakeRiftLinkBle();
      ble.connected = true;

      await tester.pumpWidget(
        MaterialApp(
          home: ChatScreen(ble: ble),
        ),
      );

      expect(find.byType(TextField), findsOneWidget);
      expect(find.text('Отправить'), findsOneWidget);
    });
  });
}
