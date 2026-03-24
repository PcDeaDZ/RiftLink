import 'dart:async';
import 'dart:collection';
import 'package:flutter/foundation.dart' show debugPrint;

import '../ble/riftlink_ble.dart';
import '../contacts/contacts_service.dart';
import '../mesh_constants.dart';
import 'chat_models.dart';
import 'chat_repository.dart';
import 'incoming_msg_resolution.dart';
import 'msg_ingress_dedup.dart';

class ChatEventIngestor {
  final RiftLinkBle ble;
  final ChatRepository repo;
  StreamSubscription<RiftLinkEvent>? _sub;
  final Queue<_QueuedBleEvent> _eventQueue = Queue<_QueuedBleEvent>();
  bool _processingQueue = false;
  static const int _dedupMaxEntries = 4096;
  final Set<String> _dedupKeys = <String>{};
  final Queue<String> _dedupFifo = Queue<String>();
  int _incomingNoIdSeq = 0;
  final Map<String, int> _statusCounters = <String, int>{};
  int _statusEventsSinceDump = 0;
  DateTime _lastStatusDumpAt = DateTime.fromMillisecondsSinceEpoch(0);
  DateTime _lastQueueLagLogAt = DateTime.fromMillisecondsSinceEpoch(0);

  /// Events re-emitted AFTER persistence is complete.
  /// UI subscribers get sequential consistency: the DB row exists by the time
  /// they receive the event. Replaces the dual-subscription pattern where
  /// ChatScreen listened on ble.events in parallel with the ingestor.
  final StreamController<RiftLinkEvent> _processedController =
      StreamController<RiftLinkEvent>.broadcast();
  Stream<RiftLinkEvent> get processedEvents => _processedController.stream;

  static ChatEventIngestor? _instance;
  static ChatEventIngestor? get instance => _instance;

  ChatEventIngestor({
    required this.ble,
    required this.repo,
  }) {
    _instance = this;
  }

  Future<void> start() async {
    await repo.init();
    _sub?.cancel();
    _sub = ble.events.listen(_enqueue);
  }

  Future<void> stop() async {
    await _sub?.cancel();
    _sub = null;
    _eventQueue.clear();
    _processingQueue = false;
    _dedupKeys.clear();
    _dedupFifo.clear();
    _incomingNoIdSeq = 0;
  }

  void dispose() {
    _processedController.close();
  }

  bool _dedupTryAdd(String key) {
    if (_dedupKeys.contains(key)) return false;
    _dedupKeys.add(key);
    _dedupFifo.addLast(key);
    while (_dedupFifo.length > _dedupMaxEntries) {
      final old = _dedupFifo.removeFirst();
      _dedupKeys.remove(old);
    }
    return true;
  }

  void _enqueue(RiftLinkEvent event) {
    _eventQueue.addLast(_QueuedBleEvent(event, DateTime.now()));
    _drainQueue();
  }

  void _maybeLogQueueLag(_QueuedBleEvent queued) {
    final lagMs = DateTime.now().difference(queued.enqueuedAt).inMilliseconds;
    if (lagMs < 150 && _eventQueue.length < 30) return;
    final now = DateTime.now();
    if (now.difference(_lastQueueLagLogAt) < const Duration(seconds: 2)) return;
    _lastQueueLagLogAt = now;
    debugPrint(
      '[BLE_CHAIN] stage=app_ingest action=queue_lag lag_ms=$lagMs depth=${_eventQueue.length} evt=${queued.event.runtimeType}',
    );
  }

  void _drainQueue() {
    if (_processingQueue) return;
    _processingQueue = true;
    unawaited(() async {
      try {
        while (_eventQueue.isNotEmpty) {
          final queued = _eventQueue.removeFirst();
          _maybeLogQueueLag(queued);
          await _handle(queued.event);
          if (!_processedController.isClosed) {
            _processedController.add(queued.event);
          }
        }
      } finally {
        _processingQueue = false;
        // In case new events arrived between the last loop check and finally.
        if (_eventQueue.isNotEmpty) {
          _drainQueue();
        }
      }
    }());
  }

  void _recordStatus(String evt, int? msgId, String peer) {
    _statusCounters[evt] = (_statusCounters[evt] ?? 0) + 1;
    _statusEventsSinceDump++;
    final now = DateTime.now();
    final shouldDump = _statusEventsSinceDump >= 15 || now.difference(_lastStatusDumpAt) >= const Duration(seconds: 30);
    if (!shouldDump) return;
    _statusEventsSinceDump = 0;
    _lastStatusDumpAt = now;
    debugPrint(
      '[BLE_CHAIN] stage=app_msg_state action=ingest evt=$evt msgId=${msgId ?? 0} peer=$peer '
      'sent=${_statusCounters['sent'] ?? 0} delivered=${_statusCounters['delivered'] ?? 0} '
      'read=${_statusCounters['read'] ?? 0} undelivered=${_statusCounters['undelivered'] ?? 0} '
      'broadcast_delivery=${_statusCounters['broadcast_delivery'] ?? 0}',
    );
  }

  String _normalizeId(String raw) => raw.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();
  bool _hasValidStatusMsgId(int msgId) => msgId > 0;

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

  Future<void> _ensureConversationForGroupUid(
    String groupUid, {
    int? channelId32,
    String? canonicalName,
  }) async {
    final uid = groupUid.trim().toUpperCase();
    if (uid.isEmpty) return;
    final id = ChatRepository.groupConversationIdByUid(uid);
    final title = (canonicalName != null && canonicalName.trim().isNotEmpty)
        ? canonicalName.trim()
        : ((channelId32 != null && channelId32 > 1)
              ? 'Group $channelId32'
              : 'Group ${uid.substring(0, uid.length > 8 ? 8 : uid.length)}');
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
      final route = resolveIncomingMsgRoute(
        fromNormalized: from,
        evt: event,
        localGroups: ble.lastInfo?.groups,
      );
      final groupId = event.group ?? 0;
      final hasGroupId = groupId > kMeshBroadcastGroupId;
      final scopeTag = route.isBroadcastMesh ? 'broadcast' : (route.groupContext ?? 'direct');
      final dedupKey = buildMsgIngressDedupKey(
        fromNormalized: from,
        scopeTag: scopeTag,
        msgId: event.msgId,
        noIdSequence: (event.msgId != null && event.msgId! > 0) ? 0 : ++_incomingNoIdSeq,
      );
      if (!_dedupTryAdd(dedupKey)) return;

      final conversationId = route.conversationId;
      debugPrint(
        '[BLE_CHAIN] stage=app_msg_state action=ingest evt=msg msgId=${event.msgId ?? 0} '
        'from=$from conv=$conversationId scope=$scopeTag',
      );
      if (route.isBroadcastMesh) {
        await _ensureBroadcastConversation();
      } else if (route.groupContext != null) {
        if (hasGroupId && !route.groupContext!.startsWith('UNRESOLVED_')) {
          await repo.migrateUnresolvedGroupConversation(
            channelId32: groupId,
            groupUid: route.groupContext!,
          );
        }
        await _ensureConversationForGroupUid(
          route.groupContext!,
          channelId32: groupId > 0 ? groupId : null,
        );
      } else {
        await _ensureConversationForDirect(from);
      }
      await repo.appendMessage(
        ChatMessage(
          conversationId: conversationId,
          from: from,
          text: event.text,
          groupId: hasGroupId ? groupId : (route.isBroadcastMesh ? kMeshBroadcastGroupId : null),
          groupUid: route.groupContext,
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
      if (!_hasValidStatusMsgId(event.msgId)) {
        debugPrint('[BLE_CHAIN] stage=app_msg_state action=drop reason=invalid_msg_id evt=sent msgId=${event.msgId}');
        return;
      }
      final to = _normalizeId(event.to);
      _recordStatus('sent', event.msgId, to);
      final conversationId = to == 'FFFFFFFFFFFFFFFF'
          ? ChatRepository.broadcastConversationId()
          : ChatRepository.directConversationId(to);
      if (to == 'FFFFFFFFFFFFFFFF') {
        await _ensureBroadcastConversation();
      } else {
        await _ensureConversationForDirect(to);
      }
      var updated = await repo.updateByMsgId(
        msgId: event.msgId,
        conversationId: conversationId,
        status: MessageStatus.sent,
      );
      if (updated == 0) {
        // Fallback: sent can arrive before local outgoing row gets msg_id.
        updated = await repo.bindMsgIdToLatestOutgoing(
          conversationId: conversationId,
          msgId: event.msgId,
          status: MessageStatus.sent,
        );
      }
      if (updated == 0) {
        await repo.updateByMsgIdAnyConversation(
          msgId: event.msgId,
          status: MessageStatus.sent,
        );
      }
      return;
    }

    if (event is RiftLinkDeliveredEvent) {
      if (!_hasValidStatusMsgId(event.msgId)) {
        debugPrint('[BLE_CHAIN] stage=app_msg_state action=drop reason=invalid_msg_id evt=delivered msgId=${event.msgId}');
        return;
      }
      final from = _normalizeId(event.from);
      _recordStatus('delivered', event.msgId, from);
      final conversationId = ChatRepository.directConversationId(from);
      await _ensureConversationForDirect(from);
      var updated = await repo.updateByMsgId(
        msgId: event.msgId,
        conversationId: conversationId,
        status: MessageStatus.delivered,
      );
      if (updated == 0) {
        updated = await repo.bindMsgIdToLatestOutgoing(
          conversationId: conversationId,
          msgId: event.msgId,
          status: MessageStatus.delivered,
        );
      }
      if (updated == 0) {
        updated = await repo.updateByMsgIdAnyConversation(
          msgId: event.msgId,
          status: MessageStatus.delivered,
        );
      }
      return;
    }

    // Личка: `from` — узел получателя (читателя); чат direct:<from>. Цепочка при серой галочке при прочтении:
    // прошивка notifyRead → BLE evt:read → сюда → updateByMsgId(direct) / bind / updateByMsgIdAnyConversation.
    if (event is RiftLinkReadEvent) {
      if (!_hasValidStatusMsgId(event.msgId)) {
        debugPrint('[BLE_CHAIN] stage=app_msg_state action=drop reason=invalid_msg_id evt=read msgId=${event.msgId}');
        return;
      }
      final from = _normalizeId(event.from);
      _recordStatus('read', event.msgId, from);
      final conversationId = ChatRepository.directConversationId(from);
      await _ensureConversationForDirect(from);
      var updated = await repo.updateByMsgId(
        msgId: event.msgId,
        conversationId: conversationId,
        status: MessageStatus.read,
      );
      if (updated == 0) {
        updated = await repo.bindMsgIdToLatestOutgoing(
          conversationId: conversationId,
          msgId: event.msgId,
          status: MessageStatus.read,
        );
      }
      if (updated == 0) {
        updated = await repo.updateByMsgIdAnyConversation(
          msgId: event.msgId,
          status: MessageStatus.read,
        );
      }
      if (updated == 0) {
        debugPrint(
          '[BLE_CHAIN] stage=app_msg_state action=no_row evt=read msgId=${event.msgId} peer=$from '
          '(нет исходящего с этим msg_id в SQLite — проверить sent, совпадение msg_id с прошивкой, RX OP_READ→notifyRead)',
        );
      }
      return;
    }

    if (event is RiftLinkUndeliveredEvent) {
      if (!_hasValidStatusMsgId(event.msgId)) {
        debugPrint('[BLE_CHAIN] stage=app_msg_state action=drop reason=invalid_msg_id evt=undelivered msgId=${event.msgId}');
        return;
      }
      final to = _normalizeId(event.to);
      _recordStatus('undelivered', event.msgId, to);
      final conversationId = to == 'FFFFFFFFFFFFFFFF'
          ? ChatRepository.broadcastConversationId()
          : ChatRepository.directConversationId(to);
      var updated = await repo.updateByMsgId(
        msgId: event.msgId,
        conversationId: conversationId,
        status: MessageStatus.undelivered,
        delivered: event.delivered,
        total: event.total,
      );
      if (updated == 0) {
        updated = await repo.bindMsgIdToLatestOutgoing(
          conversationId: conversationId,
          msgId: event.msgId,
          status: MessageStatus.undelivered,
          delivered: event.delivered,
          total: event.total,
        );
      }
      if (updated == 0) {
        updated = await repo.updateByMsgIdAnyConversation(
          msgId: event.msgId,
          status: MessageStatus.undelivered,
          delivered: event.delivered,
          total: event.total,
        );
      }
      return;
    }

    if (event is RiftLinkBroadcastDeliveryEvent) {
      if (!_hasValidStatusMsgId(event.msgId)) {
        debugPrint(
          '[BLE_CHAIN] stage=app_msg_state action=drop reason=invalid_msg_id evt=broadcast_delivery msgId=${event.msgId}',
        );
        return;
      }
      _recordStatus('broadcast_delivery', event.msgId, 'FFFFFFFFFFFFFFFF');
      final conversationId = ChatRepository.broadcastConversationId();
      await _ensureBroadcastConversation();
      var updated = await repo.updateByMsgId(
        msgId: event.msgId,
        conversationId: conversationId,
        status: event.delivered > 0 ? MessageStatus.delivered : MessageStatus.undelivered,
        delivered: event.delivered,
        total: event.total,
      );
      if (updated == 0) {
        await repo.updateByMsgIdAnyConversation(
          msgId: event.msgId,
          status: event.delivered > 0 ? MessageStatus.delivered : MessageStatus.undelivered,
          delivered: event.delivered,
          total: event.total,
        );
      }
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
      await _ensureConversationForGroupUid(
        event.groupUid,
        channelId32: event.channelId32,
        canonicalName: event.canonicalName,
      );
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

class _QueuedBleEvent {
  final RiftLinkEvent event;
  final DateTime enqueuedAt;

  const _QueuedBleEvent(this.event, this.enqueuedAt);
}

