import 'package:flutter_test/flutter_test.dart';
import 'package:riftlink_app/chat/msg_ingress_dedup.dart';

void main() {
  test('same msgId>0 yields same dedup key', () {
    final a = buildMsgIngressDedupKey(
      fromNormalized: 'A1B2C3D4E5F6A7B8',
      scopeTag: 'direct',
      msgId: 42,
      noIdSequence: 0,
    );
    final b = buildMsgIngressDedupKey(
      fromNormalized: 'A1B2C3D4E5F6A7B8',
      scopeTag: 'direct',
      msgId: 42,
      noIdSequence: 999,
    );
    expect(a, b);
  });

  test('missing msgId: different noIdSequence => different keys (same text would not collapse)', () {
    final a = buildMsgIngressDedupKey(
      fromNormalized: 'A1B2C3D4E5F6A7B8',
      scopeTag: 'direct',
      msgId: null,
      noIdSequence: 1,
    );
    final b = buildMsgIngressDedupKey(
      fromNormalized: 'A1B2C3D4E5F6A7B8',
      scopeTag: 'direct',
      msgId: null,
      noIdSequence: 2,
    );
    expect(a, isNot(b));
  });

  test('msgId 0 uses noId branch like null', () {
    final a = buildMsgIngressDedupKey(
      fromNormalized: 'AA',
      scopeTag: 'broadcast',
      msgId: 0,
      noIdSequence: 3,
    );
    expect(a, 'msg:AA:noid:3:broadcast');
  });
}
