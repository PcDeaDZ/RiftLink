import 'package:flutter/foundation.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';

class LocalNotificationsService {
  static final FlutterLocalNotificationsPlugin _plugin =
      FlutterLocalNotificationsPlugin();
  static bool _initialized = false;
  static DateTime? _lastLowBatteryAt;

  static Future<void> init() async {
    if (_initialized || kIsWeb) return;
    const settings = InitializationSettings(
      android: AndroidInitializationSettings('@mipmap/ic_launcher'),
      iOS: DarwinInitializationSettings(),
    );
    await _plugin.initialize(settings);
    await _plugin
        .resolvePlatformSpecificImplementation<
            AndroidFlutterLocalNotificationsPlugin>()
        ?.requestNotificationsPermission();
    await _plugin
        .resolvePlatformSpecificImplementation<
            IOSFlutterLocalNotificationsPlugin>()
        ?.requestPermissions(alert: true, badge: true, sound: true);
    _initialized = true;
  }

  static Future<void> showIncomingMessage({
    required String from,
    required String text,
  }) async {
    if (!_initialized || kIsWeb) return;
    const details = NotificationDetails(
      android: AndroidNotificationDetails(
        'riftlink_messages',
        'RiftLink messages',
        importance: Importance.high,
        priority: Priority.high,
      ),
      iOS: DarwinNotificationDetails(),
    );
    await _plugin.show(
      1001,
      'RiftLink',
      'Сообщение от $from: $text',
      details,
    );
  }

  static Future<void> showLowBattery({
    required int percent,
  }) async {
    if (!_initialized || kIsWeb) return;
    final now = DateTime.now();
    final last = _lastLowBatteryAt;
    if (last != null && now.difference(last).inMinutes < 20) return;
    _lastLowBatteryAt = now;
    const details = NotificationDetails(
      android: AndroidNotificationDetails(
        'riftlink_battery',
        'RiftLink battery',
        importance: Importance.high,
        priority: Priority.high,
      ),
      iOS: DarwinNotificationDetails(),
    );
    await _plugin.show(
      1002,
      'RiftLink',
      'Низкий заряд узла: $percent%',
      details,
    );
  }
}
