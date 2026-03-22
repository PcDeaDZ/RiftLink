import 'package:flutter_test/flutter_test.dart';
import 'package:riftlink_app/screens/chat/chat_capabilities.dart';

void main() {
  test('direct chat feature matrix enables voice and peer ping', () {
    final matrix = featureMatrixFor(ChatContextType.direct);
    expect(matrix.canSendText, isTrue);
    expect(matrix.canVoice, isTrue);
    expect(matrix.canPingPeer, isTrue);
    expect(matrix.canSos, isFalse);
    expect(matrix.canLocation, isFalse);
  });

  test('group chat feature matrix disables voice and peer ping', () {
    final matrix = featureMatrixFor(ChatContextType.group);
    expect(matrix.canSendText, isTrue);
    expect(matrix.canVoice, isFalse);
    expect(matrix.canPingPeer, isFalse);
    expect(matrix.canTimeCapsule, isTrue);
    expect(matrix.canSos, isFalse);
  });

  test('broadcast chat feature matrix allows sos and location only', () {
    final matrix = featureMatrixFor(ChatContextType.broadcast);
    expect(matrix.canSendText, isTrue);
    expect(matrix.canVoice, isFalse);
    expect(matrix.canPingPeer, isFalse);
    expect(matrix.canSos, isTrue);
    expect(matrix.canLocation, isTrue);
    expect(matrix.canTimeCapsule, isFalse);
  });
}
