import 'package:flutter_test/flutter_test.dart';
import 'package:riftlink_app/utils/group_invite_normalize.dart';

void main() {
  group('normalizeGroupInvitePayload', () {
    test('strips internal whitespace and fixes padding', () {
      const core = 'abcdABCD';
      final withBreaks = '  abcd\n\r ABCD  ';
      final out = normalizeGroupInvitePayload(withBreaks);
      expect(out, 'abcdABCD');
    });

    test('url-safe base64', () {
      const urlSafe = 'ab-_cd';
      final out = normalizeGroupInvitePayload(urlSafe);
      expect(out, 'ab+/cd==');
    });
  });
}
