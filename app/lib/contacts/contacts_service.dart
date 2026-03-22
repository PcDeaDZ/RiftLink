/// RiftLink Contacts — сохранённые контакты (ID + никнейм)

import 'dart:convert';
import 'package:shared_preferences/shared_preferences.dart';

class Contact {
  final String id;   // full node id hex16
  final String nickname;
  final bool legacy;

  const Contact({required this.id, required this.nickname, this.legacy = false});

  Map<String, dynamic> toJson() => {'id': id, 'nickname': nickname, if (legacy) 'legacy': true};

  factory Contact.fromJson(Map<String, dynamic> json) => Contact(
        id: json['id'] as String? ?? '',
        nickname: json['nickname'] as String? ?? '',
        legacy: json['legacy'] == true,
      );
}

class ContactsService {
  static const _key = 'riftlink_contacts';
  static String _normalizeId(String raw) =>
      raw.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();

  static bool _isFullId(String id) => id.length == 16;
  static bool _isLegacyShortId(String id) => id.length == 8;

  static Future<List<Contact>> load() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_key);
    if (raw == null || raw.isEmpty) return [];
    try {
      final list = jsonDecode(raw) as List<dynamic>?;
      if (list == null) return [];
      return list
          .map((e) => Contact.fromJson(e as Map<String, dynamic>))
          .map((c) => Contact(id: _normalizeId(c.id), nickname: c.nickname, legacy: c.legacy))
          .where((c) => _isFullId(c.id) || _isLegacyShortId(c.id))
          .map((c) => Contact(id: c.id, nickname: c.nickname, legacy: _isLegacyShortId(c.id) || c.legacy))
          .toList();
    } catch (_) {
      return [];
    }
  }

  static Future<void> save(List<Contact> contacts) async {
    final prefs = await SharedPreferences.getInstance();
    final raw = jsonEncode(contacts.map((c) => c.toJson()).toList());
    await prefs.setString(_key, raw);
  }

  static Future<void> add(Contact contact) async {
    final normalized = _normalizeId(contact.id);
    if (!_isFullId(normalized)) return;
    final list = await load();
    final idx = list.indexWhere((c) => _normalizeId(c.id) == normalized);
    final updated = Contact(id: normalized, nickname: contact.nickname, legacy: false);
    if (idx >= 0) {
      list[idx] = updated;
    } else {
      list.add(updated);
    }
    await save(list);
  }

  /// Миграция legacy short-id -> full-id, когда в рантайме появился полный ID узла.
  static Future<void> promoteLegacy(String fullId) async {
    final normalizedFull = _normalizeId(fullId);
    if (!_isFullId(normalizedFull)) return;
    final short = normalizedFull.substring(0, 8);
    final list = await load();
    final fullIdx = list.indexWhere((c) => _normalizeId(c.id) == normalizedFull);
    final legacyIdx = list.indexWhere((c) => _normalizeId(c.id) == short);
    if (legacyIdx < 0) return;
    final legacy = list[legacyIdx];
    if (fullIdx >= 0) {
      final cur = list[fullIdx];
      if (cur.nickname.trim().isEmpty && legacy.nickname.trim().isNotEmpty) {
        list[fullIdx] = Contact(id: normalizedFull, nickname: legacy.nickname.trim(), legacy: false);
      }
      list.removeAt(legacyIdx);
    } else {
      list[legacyIdx] = Contact(id: normalizedFull, nickname: legacy.nickname, legacy: false);
    }
    await save(list);
  }

  static Future<void> remove(String id) async {
    final normalized = _normalizeId(id);
    final list = await load();
    list.removeWhere((c) => _normalizeId(c.id) == normalized);
    await save(list);
  }

  static Future<String?> getNickname(String id) async {
    final normalized = _normalizeId(id);
    final list = await load();
    final c = list.where((x) => _normalizeId(x.id) == normalized).firstOrNull;
    return c?.nickname.isNotEmpty == true ? c!.nickname : null;
  }

  static Map<String, String> buildNicknameMap(Iterable<Contact> contacts) {
    final map = <String, String>{};
    for (final c in contacts) {
      final nick = c.nickname.trim();
      if (nick.isEmpty) continue;
      final id = _normalizeId(c.id);
      if (id.isEmpty) continue;
      map[id] = nick;
      if (id.length >= 8) {
        map[id.substring(0, 8)] = nick;
      }
    }
    return map;
  }

  static String displayNodeLabel(String nodeId, Map<String, String> nickById) {
    final id = _normalizeId(nodeId);
    if (id.isEmpty) return nodeId.toUpperCase();
    final nick = nickById[id] ?? (id.length >= 8 ? nickById[id.substring(0, 8)] : null);
    return (nick != null && nick.isNotEmpty) ? nick : id;
  }
}
