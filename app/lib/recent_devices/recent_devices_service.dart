/// RiftLink — недавно подключённые устройства (remoteId, nodeId, nickname)

import 'dart:convert';
import 'package:shared_preferences/shared_preferences.dart';

class RecentDevice {
  final String remoteId;
  final String nodeId;
  final String? nickname;
  final DateTime lastConnected;

  const RecentDevice({
    required this.remoteId,
    required this.nodeId,
    this.nickname,
    required this.lastConnected,
  });

  String get displayName => (nickname != null && nickname!.isNotEmpty) ? nickname! : nodeId;

  Map<String, dynamic> toJson() => {
        'remoteId': remoteId,
        'nodeId': nodeId,
        'nickname': nickname,
        'lastConnected': lastConnected.toIso8601String(),
      };

  factory RecentDevice.fromJson(Map<String, dynamic> json) => RecentDevice(
        remoteId: json['remoteId'] as String? ?? '',
        nodeId: json['nodeId'] as String? ?? '',
        nickname: json['nickname'] as String?,
        lastConnected: DateTime.tryParse(json['lastConnected'] as String? ?? '') ?? DateTime.now(),
      );
}

class RecentDevicesService {
  static const _key = 'riftlink_recent_devices';
  static const _wifiIpKey = 'riftlink_recent_wifi_ips';
  static const _maxCount = 10;
  static const _maxWifiIpCount = 6;
  static final RegExp _ipv4 = RegExp(
    r'^((25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.){3}(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)$',
  );

  static Future<List<RecentDevice>> load() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_key);
    if (raw == null || raw.isEmpty) return [];
    try {
      final list = jsonDecode(raw) as List<dynamic>?;
      if (list == null) return [];
      return list
          .map((e) => RecentDevice.fromJson(e as Map<String, dynamic>))
          .where((d) => d.remoteId.isNotEmpty)
          .toList()
        ..sort((a, b) => b.lastConnected.compareTo(a.lastConnected));
    } catch (_) {
      return [];
    }
  }

  static Future<void> addOrUpdate({
    required String remoteId,
    required String nodeId,
    String? nickname,
  }) async {
    final list = await load();
    final now = DateTime.now();
    final idx = list.indexWhere((d) => d.remoteId == remoteId);
    final updated = RecentDevice(
      remoteId: remoteId,
      nodeId: nodeId,
      nickname: nickname ?? (idx >= 0 ? list[idx].nickname : null),
      lastConnected: now,
    );
    if (idx >= 0) {
      list[idx] = updated;
    } else {
      list.insert(0, updated);
    }
    final trimmed = list.take(_maxCount).toList();
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_key, jsonEncode(trimmed.map((d) => d.toJson()).toList()));
  }

  static Future<void> remove(String remoteId) async {
    final list = await load();
    final filtered = list.where((d) => d.remoteId != remoteId).toList();
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_key, jsonEncode(filtered.map((d) => d.toJson()).toList()));
  }

  static Future<void> updateNickname(String remoteId, String? nickname) async {
    final list = await load();
    final idx = list.indexWhere((d) => d.remoteId == remoteId);
    if (idx < 0) return;
    list[idx] = RecentDevice(
      remoteId: list[idx].remoteId,
      nodeId: list[idx].nodeId,
      nickname: nickname,
      lastConnected: list[idx].lastConnected,
    );
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_key, jsonEncode(list.map((d) => d.toJson()).toList()));
  }

  // --- WiFi recent IPs (legacy simple list, still readable) ---

  static Future<List<String>> loadRecentWifiIps() async {
    final prefs = await SharedPreferences.getInstance();
    final list = prefs.getStringList(_wifiIpKey) ?? const <String>[];
    final cleaned = list
        .map((ip) => ip.trim())
        .where((ip) => ip.isNotEmpty && _ipv4.hasMatch(ip))
        .toList();
    if (cleaned.length != list.length) {
      await prefs.setStringList(_wifiIpKey, cleaned.take(_maxWifiIpCount).toList());
    }
    return cleaned;
  }

  static Future<void> addRecentWifiIp(String ip) async {
    final clean = ip.trim();
    if (clean.isEmpty || !_ipv4.hasMatch(clean)) return;
    final prefs = await SharedPreferences.getInstance();
    final list = (prefs.getStringList(_wifiIpKey) ?? <String>[])
        .map((v) => v.trim())
        .where((v) => v.isNotEmpty && _ipv4.hasMatch(v) && v != clean)
        .map((v) => v.trim())
        .toList();
    list.insert(0, clean);
    final trimmed = list.take(_maxWifiIpCount).toList();
    await prefs.setStringList(_wifiIpKey, trimmed);
  }

  static Future<void> removeRecentWifiIp(String ip) async {
    final clean = ip.trim();
    final prefs = await SharedPreferences.getInstance();
    final list = (prefs.getStringList(_wifiIpKey) ?? <String>[])
        .map((v) => v.trim())
        .where((v) => v.isNotEmpty && v != clean)
        .toList();
    await prefs.setStringList(_wifiIpKey, list);
  }

  // --- WiFi device metadata (node ID / nickname per IP, deduplication) ---
  static const _wifiMetaKey = 'riftlink_wifi_meta';

  static Future<Map<String, WifiDeviceMeta>> _loadWifiMeta() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_wifiMetaKey);
    if (raw == null || raw.isEmpty) return {};
    try {
      final map = jsonDecode(raw) as Map<String, dynamic>? ?? {};
      return map.map((k, v) => MapEntry(k, WifiDeviceMeta.fromJson(v as Map<String, dynamic>)));
    } catch (_) {
      return {};
    }
  }

  static Future<void> _saveWifiMeta(Map<String, WifiDeviceMeta> meta) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_wifiMetaKey, jsonEncode(meta.map((k, v) => MapEntry(k, v.toJson()))));
  }

  /// Associate nodeId/nickname with an IP. If the same nodeId already exists
  /// on a different IP, the old entry is removed (dedup).
  static Future<void> associateWifiNode({
    required String ip,
    required String nodeId,
    String? nickname,
  }) async {
    final meta = await _loadWifiMeta();
    final dupeKey = meta.entries
        .where((e) => e.key != ip && (e.value.nodeId == nodeId || (nickname != null && nickname.isNotEmpty && e.value.nickname == nickname)))
        .map((e) => e.key)
        .toList();
    for (final k in dupeKey) {
      meta.remove(k);
      await removeRecentWifiIp(k);
    }
    meta[ip] = WifiDeviceMeta(nodeId: nodeId, nickname: nickname);
    await _saveWifiMeta(meta);
  }

  /// Get metadata (nodeId/nickname) for a WiFi IP if available.
  static Future<WifiDeviceMeta?> getWifiMeta(String ip) async {
    final meta = await _loadWifiMeta();
    return meta[ip];
  }

  /// Get all WiFi metadata.
  static Future<Map<String, WifiDeviceMeta>> loadAllWifiMeta() async => _loadWifiMeta();
}

class WifiDeviceMeta {
  final String nodeId;
  final String? nickname;
  const WifiDeviceMeta({required this.nodeId, this.nickname});

  String get displayName => (nickname != null && nickname!.isNotEmpty) ? nickname! : nodeId;

  Map<String, dynamic> toJson() => {'nodeId': nodeId, 'nickname': nickname};
  factory WifiDeviceMeta.fromJson(Map<String, dynamic> json) => WifiDeviceMeta(
    nodeId: json['nodeId'] as String? ?? '',
    nickname: json['nickname'] as String?,
  );
}
