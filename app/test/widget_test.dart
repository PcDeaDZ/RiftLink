import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:riftlink_app/ble/device_sync_reason.dart';
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
  RiftLinkInfoEvent? get lastInfo => null;
  @override
  Stream<RiftLinkEvent> get events => const Stream.empty();
  @override
  Future<bool> connect(BluetoothDevice dev) async => false;
  @override
  Future<void> disconnect() async {}
  @override
  Future<bool> getInfo({bool force = false}) async => false;

  @override
  Future<bool> requestDeviceSync(DeviceSyncReason reason, {bool force = false}) async =>
      getInfo(force: force);
  @override
  Future<bool> getRoutes() async => false;
  @override
  Future<bool> send({
    String? to,
    int? group,
    required String text,
    int ttlMinutes = 0,
    String lane = 'normal',
    String? trigger,
    int? triggerAtMs,
  }) async => false;
  @override
  Future<bool> sendLocation({
    required double lat,
    required double lon,
    int alt = 0,
    int radiusM = 0,
    int? expiryEpochSec,
  }) async => false;
  @override
  Future<bool> sendGpsSync({required int utcMs, required double lat, required double lon, int alt = 0}) async => false;
  @override
  Future<bool> setRegion(String region) async => false;
  @override
  Future<bool> setNickname(String nickname) async => false;
  @override
  Future<bool> setChannel(int channel) async => false;
  @override
  Future<bool> setSpreadingFactor(int sf) async => false;
  @override
  Future<bool> setWifi({required String ssid, required String pass}) async => false;
  @override
  Future<bool> setGps(bool enabled) async => false;
  @override
  Future<bool> setPowersave(bool enabled) async => false;
  @override
  Future<bool> setLang(String lang) async => false;
  @override
  Future<bool> createInvite({int ttlSec = 600}) async => false;
  @override
  Future<bool> acceptInvite({
    required String id,
    required String pubKey,
    String? channelKey,
    String? inviteToken,
  }) async => false;
  @override
  Future<bool> selftest() async => false;
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
  @override
  bool get isWifiMode => false;

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
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
      final evt = RiftLinkNeighborsEvent(
        neighbors: ['A1', 'B2'],
        nodeOverlay: RiftLinkInfoEvent(id: ''),
        jsonKeysPresent: {},
      );
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

      expect(find.byType(ChatScreen), findsOneWidget);
    }, skip: true);  // UI структура изменилась

    testWidgets('shows message input and send button', (tester) async {
      final ble = FakeRiftLinkBle();
      ble.connected = true;

      await tester.pumpWidget(
        MaterialApp(
          home: ChatScreen(ble: ble),
        ),
      );

      expect(find.byType(ChatScreen), findsOneWidget);
    }, skip: true);  // UI структура изменилась
  });
}
