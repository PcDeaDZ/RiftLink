enum ChatContextType { direct, group, broadcast }

class ChatFeatureMatrix {
  final bool canSendText;
  final bool canVoice;
  final bool canCritical;
  final bool canTimeCapsule;
  final bool canSos;
  final bool canLocation;
  final bool canPingPeer;
  final bool canChangeRecipient;

  const ChatFeatureMatrix({
    required this.canSendText,
    required this.canVoice,
    required this.canCritical,
    required this.canTimeCapsule,
    required this.canSos,
    required this.canLocation,
    required this.canPingPeer,
    required this.canChangeRecipient,
  });
}

const ChatFeatureMatrix _directMatrix = ChatFeatureMatrix(
  canSendText: true,
  canVoice: true,
  canCritical: true,
  canTimeCapsule: true,
  canSos: false,
  canLocation: false,
  canPingPeer: true,
  canChangeRecipient: true,
);

const ChatFeatureMatrix _groupMatrix = ChatFeatureMatrix(
  canSendText: true,
  canVoice: false,
  canCritical: true,
  canTimeCapsule: true,
  canSos: false,
  canLocation: false,
  canPingPeer: false,
  canChangeRecipient: true,
);

const ChatFeatureMatrix _broadcastMatrix = ChatFeatureMatrix(
  canSendText: true,
  canVoice: false,
  canCritical: true,
  canTimeCapsule: false,
  canSos: true,
  canLocation: true,
  canPingPeer: false,
  canChangeRecipient: true,
);

ChatFeatureMatrix featureMatrixFor(ChatContextType type) {
  switch (type) {
    case ChatContextType.direct:
      return _directMatrix;
    case ChatContextType.group:
      return _groupMatrix;
    case ChatContextType.broadcast:
      return _broadcastMatrix;
  }
}
