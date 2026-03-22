class ChatUiDecodedVoicePayload {
  final List<int> bytes;
  final int? voiceProfileCode;
  final DateTime? deleteAt;

  const ChatUiDecodedVoicePayload({
    required this.bytes,
    required this.voiceProfileCode,
    required this.deleteAt,
  });
}

enum ChatUiMessageStatus { sent, delivered, read, undelivered }

class ChatUiVoiceAdaptivePlan {
  final int profileCode;
  final int bitRate;
  final int maxBytes;
  final int chunkSize;

  const ChatUiVoiceAdaptivePlan({
    required this.profileCode,
    required this.bitRate,
    required this.maxBytes,
    required this.chunkSize,
  });
}

class ChatUiVoiceRxAssembly {
  int total;
  final DateTime startedAt;
  DateTime lastUpdated;
  final Map<int, String> parts = <int, String>{};

  ChatUiVoiceRxAssembly({
    required this.total,
    required this.startedAt,
    required this.lastUpdated,
  });
}

class ChatUiMessage {
  final String from;
  final String text;
  final bool isIncoming;
  final DateTime at;
  final bool isLocation;
  final bool isVoice;
  final List<int>? voiceData;
  final int? voiceProfileCode;
  final int? msgId;
  final String? to;
  final int? rssi;
  final DateTime? deleteAt;
  final ChatUiMessageStatus status;
  final int? delivered;
  final int? total;
  final int? relayCount;
  final List<String> relayPeers;
  final bool relaySummarySent;
  final String lane;
  final String type;

  ChatUiMessage({
    required this.from,
    required this.text,
    required this.isIncoming,
    DateTime? at,
    this.isLocation = false,
    this.isVoice = false,
    this.voiceData,
    this.voiceProfileCode,
    this.msgId,
    this.to,
    this.rssi,
    this.deleteAt,
    this.status = ChatUiMessageStatus.sent,
    this.delivered,
    this.total,
    this.relayCount,
    this.relayPeers = const [],
    this.relaySummarySent = false,
    this.lane = 'normal',
    this.type = 'text',
  }) : at = at ?? DateTime.now();

  ChatUiMessage copyWith({
    int? msgId,
    String? to,
    ChatUiMessageStatus? status,
    int? delivered,
    int? total,
    int? relayCount,
    List<String>? relayPeers,
    bool? relaySummarySent,
    String? lane,
    String? type,
    int? voiceProfileCode,
  }) =>
      ChatUiMessage(
        from: from,
        text: text,
        isIncoming: isIncoming,
        at: at,
        isLocation: isLocation,
        isVoice: isVoice,
        voiceData: voiceData,
        voiceProfileCode: voiceProfileCode ?? this.voiceProfileCode,
        msgId: msgId ?? this.msgId,
        to: to ?? this.to,
        rssi: rssi,
        deleteAt: deleteAt,
        status: status ?? this.status,
        delivered: delivered ?? this.delivered,
        total: total ?? this.total,
        relayCount: relayCount ?? this.relayCount,
        relayPeers: relayPeers ?? this.relayPeers,
        relaySummarySent: relaySummarySent ?? this.relaySummarySent,
        lane: lane ?? this.lane,
        type: type ?? this.type,
      );
}
