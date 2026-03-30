import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:riftlink_app/ble/riftlink_ble.dart';

void main() {
  group('RiftLinkBle.exceedsBleAttLimit', () {
    test('accepts payload when JSON+LF fits in 512 bytes (max JSON 511 B)', () {
      var textLen = 0;
      var json = jsonEncode({'cmd': 'send', 'text': ''});
      // NDJSON на провод: utf8(json) + \\n <= 512
      while (utf8.encode(json).length + 1 <= RiftLinkBle.bleAttMaxJsonBytes) {
        textLen++;
        json = jsonEncode({'cmd': 'send', 'text': 'A' * textLen});
      }
      textLen--;
      json = jsonEncode({'cmd': 'send', 'text': 'A' * textLen});
      expect(utf8.encode(json).length + 1, RiftLinkBle.bleAttMaxJsonBytes);
      expect(RiftLinkBle.exceedsBleAttLimit(json), isFalse);
    });

    test('rejects payload larger than 512 bytes', () {
      final base = jsonEncode({'cmd': 'send', 'text': 'A' * 500});
      final oversizeText = 'A' * (500 + (RiftLinkBle.bleAttMaxJsonBytes - utf8.encode(base).length) + 2);
      final json = jsonEncode({'cmd': 'send', 'text': oversizeText});
      expect(utf8.encode(json).length, greaterThan(RiftLinkBle.bleAttMaxJsonBytes));
      expect(RiftLinkBle.exceedsBleAttLimit(json), isTrue);
    });

    test('counts UTF-8 bytes, not characters', () {
      final payload = {
        'cmd': 'send',
        'text': 'Привет' * 40,
      };
      final json = jsonEncode(payload);
      expect(
        RiftLinkBle.exceedsBleAttLimit(json),
        utf8.encode(json).length + 1 > RiftLinkBle.bleAttMaxJsonBytes,
      );
    });
  });
}
