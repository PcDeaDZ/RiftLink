/// RiftLink Contacts — сохранённые контакты (ID + никнейм)

import 'dart:convert';
import 'package:shared_preferences/shared_preferences.dart';

class Contact {
  final String id;   // hex8
  final String nickname;

  const Contact({required this.id, required this.nickname});

  Map<String, dynamic> toJson() => {'id': id, 'nickname': nickname};

  factory Contact.fromJson(Map<String, dynamic> json) => Contact(
        id: json['id'] as String? ?? '',
        nickname: json['nickname'] as String? ?? '',
      );
}

class ContactsService {
  static const _key = 'riftlink_contacts';

  static Future<List<Contact>> load() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_key);
    if (raw == null || raw.isEmpty) return [];
    try {
      final list = jsonDecode(raw) as List<dynamic>?;
      if (list == null) return [];
      return list
          .map((e) => Contact.fromJson(e as Map<String, dynamic>))
          .where((c) => c.id.length >= 8)
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
    final list = await load();
    final idx = list.indexWhere((c) => c.id == contact.id);
    if (idx >= 0) {
      list[idx] = contact;
    } else {
      list.add(contact);
    }
    await save(list);
  }

  static Future<void> remove(String id) async {
    final list = await load();
    list.removeWhere((c) => c.id == id);
    await save(list);
  }

  static Future<String?> getNickname(String id) async {
    final list = await load();
    final c = list.where((x) => x.id == id).firstOrNull;
    return c?.nickname.isNotEmpty == true ? c!.nickname : null;
  }
}
