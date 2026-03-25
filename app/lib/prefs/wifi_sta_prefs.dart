/// Кэш SSID/пароля STA Wi‑Fi на телефоне (по ID узла) — подстановка в настройки после перезагрузки.
/// Пароль в NVS на устройстве; по BLE пароль не передаётся — только локальное хранение.

import 'dart:convert';

import 'package:shared_preferences/shared_preferences.dart';

class WifiStaPrefs {
  static String _key(String nodeIdHex) => 'riftlink_wifi_sta_$nodeIdHex';

  static Future<void> save({
    required String nodeIdHex,
    required String ssid,
    required String pass,
  }) async {
    if (nodeIdHex.isEmpty) return;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(
      _key(nodeIdHex),
      jsonEncode({'ssid': ssid, 'pass': pass}),
    );
  }

  static Future<({String ssid, String pass})?> load(String nodeIdHex) async {
    if (nodeIdHex.isEmpty) return null;
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_key(nodeIdHex));
    if (raw == null || raw.isEmpty) return null;
    try {
      final m = jsonDecode(raw) as Map<String, dynamic>;
      final s = m['ssid'] as String? ?? '';
      final p = m['pass'] as String? ?? '';
      return (ssid: s, pass: p);
    } catch (_) {
      return null;
    }
  }
}
