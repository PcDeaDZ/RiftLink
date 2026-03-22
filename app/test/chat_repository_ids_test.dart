import 'package:flutter_test/flutter_test.dart';
import 'package:riftlink_app/chat/chat_repository.dart';

void main() {
  test('conversation id builders are stable', () {
    expect(ChatRepository.directConversationId('ABCDEF'), 'direct:ABCDEF');
    expect(ChatRepository.groupConversationIdByUid('abc42'), 'groupv2:ABC42');
    expect(ChatRepository.groupPeerRefByUid('abc42'), 'uid:ABC42');
    expect(ChatRepository.broadcastConversationId(), 'broadcast:all');
  });
}

