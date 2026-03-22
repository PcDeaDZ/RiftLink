import 'dart:convert';

enum ConversationKind { direct, group, broadcast }

enum MessageDirection { incoming, outgoing }

enum MessageStatus { pending, sent, delivered, read, undelivered }

class ChatConversation {
  final String id;
  final ConversationKind kind;
  final String peerRef;
  final String title;
  final String? subtitle;
  final String? lastMessagePreview;
  final int? lastMessageAtMs;
  final int unreadCount;
  final bool archived;
  final bool pinned;
  final bool muted;
  final String folderId;

  const ChatConversation({
    required this.id,
    required this.kind,
    required this.peerRef,
    required this.title,
    this.subtitle,
    this.lastMessagePreview,
    this.lastMessageAtMs,
    this.unreadCount = 0,
    this.archived = false,
    this.pinned = false,
    this.muted = false,
    this.folderId = 'all',
  });

  ChatConversation copyWith({
    String? title,
    String? subtitle,
    String? lastMessagePreview,
    int? lastMessageAtMs,
    int? unreadCount,
    bool? archived,
    bool? pinned,
    bool? muted,
    String? folderId,
  }) {
    return ChatConversation(
      id: id,
      kind: kind,
      peerRef: peerRef,
      title: title ?? this.title,
      subtitle: subtitle ?? this.subtitle,
      lastMessagePreview: lastMessagePreview ?? this.lastMessagePreview,
      lastMessageAtMs: lastMessageAtMs ?? this.lastMessageAtMs,
      unreadCount: unreadCount ?? this.unreadCount,
      archived: archived ?? this.archived,
      pinned: pinned ?? this.pinned,
      muted: muted ?? this.muted,
      folderId: folderId ?? this.folderId,
    );
  }
}

class ChatMessage {
  final int? id;
  final String conversationId;
  final String from;
  final String? to;
  final int? groupId;
  final String? groupUid;
  final String text;
  final String type;
  final String lane;
  final MessageDirection direction;
  final MessageStatus status;
  final int createdAtMs;
  final int? msgId;
  final int? rssi;
  final int? delivered;
  final int? total;
  final int? deleteAtMs;
  final List<String> relayPeers;
  final int relayCount;

  const ChatMessage({
    this.id,
    required this.conversationId,
    required this.from,
    this.to,
    this.groupId,
    this.groupUid,
    required this.text,
    this.type = 'text',
    this.lane = 'normal',
    required this.direction,
    this.status = MessageStatus.pending,
    required this.createdAtMs,
    this.msgId,
    this.rssi,
    this.delivered,
    this.total,
    this.deleteAtMs,
    this.relayPeers = const [],
    this.relayCount = 0,
  });

  ChatMessage copyWith({
    int? id,
    String? conversationId,
    String? from,
    String? to,
    int? groupId,
    String? groupUid,
    String? text,
    String? type,
    String? lane,
    MessageDirection? direction,
    MessageStatus? status,
    int? createdAtMs,
    int? msgId,
    int? rssi,
    int? delivered,
    int? total,
    int? deleteAtMs,
    List<String>? relayPeers,
    int? relayCount,
  }) {
    return ChatMessage(
      id: id ?? this.id,
      conversationId: conversationId ?? this.conversationId,
      from: from ?? this.from,
      to: to ?? this.to,
      groupId: groupId ?? this.groupId,
      groupUid: groupUid ?? this.groupUid,
      text: text ?? this.text,
      type: type ?? this.type,
      lane: lane ?? this.lane,
      direction: direction ?? this.direction,
      status: status ?? this.status,
      createdAtMs: createdAtMs ?? this.createdAtMs,
      msgId: msgId ?? this.msgId,
      rssi: rssi ?? this.rssi,
      delivered: delivered ?? this.delivered,
      total: total ?? this.total,
      deleteAtMs: deleteAtMs ?? this.deleteAtMs,
      relayPeers: relayPeers ?? this.relayPeers,
      relayCount: relayCount ?? this.relayCount,
    );
  }
}

String relayPeersToJson(List<String> peers) => jsonEncode(peers);

List<String> relayPeersFromJson(String? raw) {
  if (raw == null || raw.isEmpty) return const [];
  try {
    final decoded = jsonDecode(raw);
    if (decoded is List) {
      return decoded.whereType<String>().toList();
    }
  } catch (_) {}
  return const [];
}

