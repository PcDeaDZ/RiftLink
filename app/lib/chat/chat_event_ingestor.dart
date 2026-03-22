import 'dart:async';

import '../ble/riftlink_ble.dart';
import '../contacts/contacts_service.dart';
import 'chat_models.dart';
import 'chat_repository.dart';

class ChatEventIngestor {
  final RiftLinkBle ble;
  final ChatRepository repo;
  StreamSubscription<RiftLinkEvent>? _sub;
  final Set<String> _dedup = <String>{};

  ChatEventIngestor({
    required this.ble,
    required this.repo,
  });

  Future<void> start() async {
    await repo.init();
    _sub?.cancel();
    _sub = ble.events.listen(_handle);
  }

  Future<void> stop() async {
    await _sub?.cancel();
    _sub = null;
  }

  String _normalizeId(String raw) => raw.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();

  Future<String> _titleFromId(String id) async {
    final n = _normalizeId(id);
    if (n.isEmpty) return id;
    final nick = await ContactsService.getNickname(n);
    if (nick != null && nick.isNotEmpty) return nick;
    return n.length > 8 ? n.substring(0, 8) : n;
  }

  Future<void> _ensureConversationForDirect(String peerId) async {
    final n = _normalizeId(peerId);
    final id = ChatRepository.directConversationId(n);
    final title = await _titleFromId(n);
    await repo.ensureConversation(
      id: id,
      kind: ConversationKind.direct,
      peerRef: n,
      title: title,
    );
  }

  Future<void> _ensureConversationForGroupUid(String groupUid, {int? channelId32}) async {
    final uid = groupUid.trim().toUpperCase();
    if (uid.isEmpty) return;
    final id = ChatRepository.groupConversationIdByUid(uid);
    final title = (channelId32 != null && channelId32 > 1) ? 'Group $channelId32' : 'Group ${uid.substring(0, uid.length > 8 ? 8 : uid.length)}';
    await repo.ensureConversation(
      id: id,
      kind: ConversationKind.group,
      peerRef: ChatRepository.groupPeerRefByUid(uid),
      title: title,
    );
  }

  Future<void> _ensureBroadcastConversation() async {
    final id = ChatRepository.broadcastConversationId();
    await repo.ensureConversation(
      id: id,
      kind: ConversationKind.broadcast,
      peerRef: 'broadcast',
      title: 'Broadcast',
    );
  }

  Future<void> _handle(RiftLinkEvent event) async {
    if (event is RiftLinkMsgEvent) {
      final from = _normalizeId(event.from);
      final dedupKey = 'msg:$from:${event.msgId ?? -1}:${event.text.hashCode}';
      if (!_dedup.add(dedupKey)) return;

      final conversationId = ChatRepository.directConversationId(from);
      await _ensureConversationForDirect(from);
      await repo.appendMessage(
        ChatMessage(
          conversationId: conversationId,
          from: from,
          text: event.text,
          type: event.type,
          lane: event.lane,
          direction: MessageDirection.incoming,
          status: MessageStatus.delivered,
          createdAtMs: DateTime.now().millisecondsSinceEpoch,
          msgId: event.msgId,
          rssi: event.rssi,
          deleteAtMs: event.ttlMinutes != null && event.ttlMinutes! > 0
              ? DateTime.now().add(Duration(minutes: event.ttlMinutes!)).millisecondsSinceEpoch
              : null,
        ),
        incrementUnread: true,
      );
      return;
    }

    if (event is RiftLinkSentEvent) {
      final to = _normalizeId(event.to);
      final conversationId = to == 'FFFFFFFFFFFFFFFF'
          ? ChatRepository.broadcastConversationId()
          : ChatRepository.directConversationId(to);
      if (to == 'FFFFFFFFFFFFFFFF') {
        await _ensureBroadcastConversation();
      } else {
        await _ensureConversationForDirect(to);
      }
      await repo.updateByMsgId(
        msgId: event.msgId,
        conversationId: conversationId,
        status: MessageStatus.sent,
      );
      return;
    }

    if (event is RiftLinkDeliveredEvent) {
      final from = _normalizeId(event.from);
      final conversationId = ChatRepository.directConversationId(from);
      await _ensureConversationForDirect(from);
      await repo.updateByMsgId(
        msgId: event.msgId,
        conversationId: conversationId,
        status: MessageStatus.delivered,
      );
      return;
    }

    if (event is RiftLinkReadEvent) {
      final from = _normalizeId(event.from);
      final conversationId = ChatRepository.directConversationId(from);
      await _ensureConversationForDirect(from);
      await repo.updateByMsgId(
        msgId: event.msgId,
        conversationId: conversationId,
        status: MessageStatus.read,
      );
      return;
    }

    if (event is RiftLinkUndeliveredEvent) {
      final to = _normalizeId(event.to);
      final conversationId = to == 'FFFFFFFFFFFFFFFF'
          ? ChatRepository.broadcastConversationId()
          : ChatRepository.directConversationId(to);
      await repo.updateByMsgId(
        msgId: event.msgId,
        conversationId: conversationId,
        status: MessageStatus.undelivered,
        delivered: event.delivered,
        total: event.total,
      );
      return;
    }

    if (event is RiftLinkBroadcastDeliveryEvent) {
      final conversationId = ChatRepository.broadcastConversationId();
      await _ensureBroadcastConversation();
      await repo.updateByMsgId(
        msgId: event.msgId,
        conversationId: conversationId,
        status: event.delivered > 0 ? MessageStatus.delivered : MessageStatus.undelivered,
        delivered: event.delivered,
        total: event.total,
      );
      return;
    }

    if (event is RiftLinkVoiceEvent) {
      final from = _normalizeId(event.from);
      final conversationId = ChatRepository.directConversationId(from);
      await _ensureConversationForDirect(from);
      await repo.appendMessage(
        ChatMessage(
          conversationId: conversationId,
          from: from,
          text: 'Voice message',
          type: 'voice',
          direction: MessageDirection.incoming,
          status: MessageStatus.delivered,
          createdAtMs: DateTime.now().millisecondsSinceEpoch,
        ),
        incrementUnread: true,
      );
      return;
    }

    if (event is RiftLinkLocationEvent) {
      final from = _normalizeId(event.from);
      final conversationId = ChatRepository.directConversationId(from);
      await _ensureConversationForDirect(from);
      await repo.appendMessage(
        ChatMessage(
          conversationId: conversationId,
          from: from,
          text: '${event.lat.toStringAsFixed(5)}, ${event.lon.toStringAsFixed(5)}',
          type: 'location',
          direction: MessageDirection.incoming,
          status: MessageStatus.delivered,
          createdAtMs: DateTime.now().millisecondsSinceEpoch,
        ),
        incrementUnread: true,
      );
      return;
    }

    if (event is RiftLinkGroupRekeyProgressEvent) {
      await _ensureConversationForGroupUid(event.groupUid);
      final now = DateTime.now().millisecondsSinceEpoch;
      await repo.upsertGroupRekeySession(
        groupUid: event.groupUid,
        rekeyOpId: event.rekeyOpId,
        keyVersion: event.keyVersion,
        createdAt: now,
        status: event.failed > 0
            ? 'failed'
            : (event.pending > 0 ? 'pending' : 'applied'),
      );
      return;
    }

    if (event is RiftLinkGroupMemberKeyStateEvent) {
      await _ensureConversationForGroupUid(event.groupUid);
      await repo.upsertGroupMemberKeyState(
        groupUid: event.groupUid,
        memberId: event.memberId,
        status: event.status,
        ackAt: event.ackAt,
        lastTryAt: DateTime.now().millisecondsSinceEpoch,
      );
      return;
    }

    if (event is RiftLinkGroupStatusEvent) {
      await _ensureConversationForGroupUid(event.groupUid, channelId32: event.channelId32);
      // Snapshot local role in grants table for thin-device projection in app.
      await repo.upsertGroupGrant(
        groupUid: event.groupUid,
        subjectId: 'SELF',
        role: event.myRole,
        grantVersion: event.keyVersion > 0 ? event.keyVersion : 1,
        revoked: event.myRole == 'none',
      );
      return;
    }

    // Do not auto-create chats from node info/groups snapshots.
    // We only create conversations by explicit user action or real messages.
  }
}

