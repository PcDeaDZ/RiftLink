import 'package:flutter_test/flutter_test.dart';
import 'package:riftlink_app/contacts/contacts_service.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  setUp(() async {
    SharedPreferences.setMockInitialValues({});
  });

  group('ContactsService', () {
    test('load returns empty when no data', () async {
      final list = await ContactsService.load();
      expect(list, isEmpty);
    });

    test('add and load', () async {
      await ContactsService.add(const Contact(id: 'A1B2C3D4', nickname: 'Alice'));
      final list = await ContactsService.load();
      expect(list.length, 1);
      expect(list.first.id, 'A1B2C3D4');
      expect(list.first.nickname, 'Alice');
    });

    test('add updates existing contact', () async {
      await ContactsService.add(const Contact(id: 'A1B2C3D4', nickname: 'Alice'));
      await ContactsService.add(const Contact(id: 'A1B2C3D4', nickname: 'Alice2'));
      final list = await ContactsService.load();
      expect(list.length, 1);
      expect(list.first.nickname, 'Alice2');
    });

    test('remove', () async {
      await ContactsService.add(const Contact(id: 'A1B2C3D4', nickname: 'Alice'));
      await ContactsService.remove('A1B2C3D4');
      final list = await ContactsService.load();
      expect(list, isEmpty);
    });

    test('getNickname', () async {
      await ContactsService.add(const Contact(id: 'A1B2C3D4', nickname: 'Bob'));
      final nick = await ContactsService.getNickname('A1B2C3D4');
      expect(nick, 'Bob');
    });

    test('getNickname returns null for unknown', () async {
      final nick = await ContactsService.getNickname('DEADBEEF');
      expect(nick, isNull);
    });
  });

  group('Contact', () {
    test('toJson/fromJson roundtrip', () {
      const c = Contact(id: 'A1B2C3D4', nickname: 'Test');
      final json = c.toJson();
      final restored = Contact.fromJson(json);
      expect(restored.id, c.id);
      expect(restored.nickname, c.nickname);
    });
  });
}
