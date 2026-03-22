import 'package:flutter_test/flutter_test.dart';
import 'package:riftlink_app/chat/chat_models.dart';

void main() {
  test('relay peers json roundtrip', () {
    const peers = ['A1B2', 'C3D4'];
    final encoded = relayPeersToJson(peers);
    final decoded = relayPeersFromJson(encoded);
    expect(decoded, peers);
  });

  test('conversation copyWith updates selected fields', () {
    const c = ChatConversation(
      id: 'direct:ABC',
      kind: ConversationKind.direct,
      peerRef: 'ABC',
      title: 'ABC',
    );
    final next = c.copyWith(title: 'Renamed', unreadCount: 3, pinned: true);
    expect(next.title, 'Renamed');
    expect(next.unreadCount, 3);
    expect(next.pinned, true);
    expect(next.id, c.id);
  });
}

