import 'chat_models.dart';

MessageStatus mergeOutgoingMessageStatus(MessageStatus current, MessageStatus incoming) {
  // Prevent regressions from terminal/stronger states when events arrive out of order.
  if (current == MessageStatus.read && incoming != MessageStatus.read) return current;
  if (current == MessageStatus.delivered && incoming == MessageStatus.sent) return current;
  if (current == MessageStatus.undelivered &&
      (incoming == MessageStatus.sent || incoming == MessageStatus.delivered)) {
    return current;
  }
  return incoming;
}
