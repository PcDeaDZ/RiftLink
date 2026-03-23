import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:geolocator/geolocator.dart';

import '../../ble/riftlink_ble.dart';
import '../../chat/chat_models.dart';
import '../../chat/outgoing_status_policy.dart';
import '../../chat/chat_repository.dart';
import '../../connection/transport_reconnect_manager.dart';
import '../../mesh_constants.dart';
import '../../recent_devices/recent_devices_service.dart';
import '../../voice/voice_service.dart';
import 'chat_ui_models.dart';

ChatUiMessageStatus _mergeUiOutgoingStatus(ChatUiMessageStatus current, ChatUiMessageStatus incoming) {
  final merged = mergeOutgoingMessageStatus(
    _domainStatusFromUi(current),
    _domainStatusFromUi(incoming),
  );
  return _uiStatusFromDomain(merged, incoming);
}

MessageStatus _domainStatusFromUi(ChatUiMessageStatus status) {
  return MessageStatus.values.firstWhere(
    (v) => v.name == status.name,
    orElse: () => MessageStatus.sent,
  );
}

ChatUiMessageStatus _uiStatusFromDomain(MessageStatus status, ChatUiMessageStatus fallback) {
  return ChatUiMessageStatus.values.firstWhere(
    (v) => v.name == status.name,
    orElse: () => fallback,
  );
}

bool _hasValidStatusMsgId(int msgId) => msgId > 0;

/// Исходящие в группу/broadcast хранят `to == null`; delivered/read приходят с `from` = получатель.
bool _conversationIsGroupOrBroadcast(String? id) {
  if (id == null || id.isEmpty) return false;
  return id.startsWith('groupv2:') || id.startsWith('broadcast:');
}

class ChatBleHandlerDeps {
  final bool isMounted;
  final List<ChatUiMessage> messages;
  final String? conversationId;
  final String nodeId;
  final String broadcastTo;
  final AppLifecycleState appLifecycle;
  final Map<String, ChatUiVoiceRxAssembly> voiceChunks;
  final Set<String> pendingPings;
  final String Function(String raw) normalizeId;
  final bool Function(String? a, String? b) sameNodeId;
  final String Function(String key, [Map<String, String>? params]) tr;
  final ChatUiDecodedVoicePayload Function(List<int> rawBytes) decodeVoicePayload;
  final String Function(int code) voiceProfileLabel;
  final bool Function(ChatUiMessage m, ChatUiMessageStatus finalStatus) shouldEmitCriticalSummary;
  final ChatUiMessage Function(ChatUiMessage m, ChatUiMessageStatus finalStatus) buildCriticalSummaryMessage;
  final void Function(void Function() fn) setState;
  final void Function(String from, String text) showIncomingNotification;
  final void Function(String text, {bool isError, bool isSuccess, Duration duration}) showSnack;
  final void Function() sendReadForUnread;
  final void Function() scrollToBottom;
  final Future<void> Function(String conversationId) markConversationRead;
  final void Function(RiftLinkInfoEvent evt) onInfoEvent;
  final void Function(RiftLinkTelemetryEvent evt) onTelemetryEvent;
  final void Function(RiftLinkLocationEvent evt) onLocationEvent;
  final void Function(RiftLinkRegionEvent evt) onRegionEvent;
  final void Function(RiftLinkPongEvent evt) onPongEvent;
  final void Function(RiftLinkErrorEvent evt) onErrorEvent;
  final void Function(RiftLinkGroupSecurityErrorEvent evt) onGroupSecurityErrorEvent;
  final void Function() onWaitingKeyEvent;
  final void Function(RiftLinkWifiEvent evt) onWifiEvent;
  final void Function(RiftLinkGpsEvent evt) onGpsEvent;
  final void Function(RiftLinkInviteEvent evt) onInviteEvent;
  final void Function(RiftLinkSelftestEvent evt) onSelftestEvent;
  final void Function(RiftLinkTimeCapsuleQueuedEvent evt) onTimeCapsuleQueued;
  final void Function(RiftLinkTimeCapsuleReleasedEvent evt) onTimeCapsuleReleased;

  ChatBleHandlerDeps({
    required this.isMounted,
    required this.messages,
    required this.conversationId,
    required this.nodeId,
    required this.broadcastTo,
    required this.appLifecycle,
    required this.voiceChunks,
    required this.pendingPings,
    required this.normalizeId,
    required this.sameNodeId,
    required this.tr,
    required this.decodeVoicePayload,
    required this.voiceProfileLabel,
    required this.shouldEmitCriticalSummary,
    required this.buildCriticalSummaryMessage,
    required this.setState,
    required this.showIncomingNotification,
    required this.showSnack,
    required this.sendReadForUnread,
    required this.scrollToBottom,
    required this.markConversationRead,
    required this.onInfoEvent,
    required this.onTelemetryEvent,
    required this.onLocationEvent,
    required this.onRegionEvent,
    required this.onPongEvent,
    required this.onErrorEvent,
    required this.onGroupSecurityErrorEvent,
    required this.onWaitingKeyEvent,
    required this.onWifiEvent,
    required this.onGpsEvent,
    required this.onInviteEvent,
    required this.onSelftestEvent,
    required this.onTimeCapsuleQueued,
    required this.onTimeCapsuleReleased,
  });
}

class ChatScreenController {
  StreamSubscription<RiftLinkEvent>? _eventSub;
  StreamSubscription<BluetoothConnectionState>? _connectionSub;
  StreamSubscription<OnConnectionStateChangedEvent>? _connectionEventsSub;
  DateTime? _lastDisconnectHandledAt;

  void bindEvents({
    required RiftLinkBle ble,
    required bool Function() isMounted,
    required void Function(RiftLinkEvent evt) onEvent,
    void Function(RiftLinkInfoEvent info)? onLastInfoReplay,
  }) {
    _eventSub?.cancel();
    debugPrint('[BLE_CHAIN] stage=app_listener action=chat_subscribe');
    _eventSub = ble.events.listen((evt) {
      if (!isMounted()) return;
      debugPrint('[BLE_CHAIN] stage=app_listener action=chat_event evt=${evt.runtimeType}');
      onEvent(evt);
    });
    final li = ble.lastInfo;
    if (li != null && isMounted()) {
      debugPrint('[BLE_CHAIN] stage=app_listener action=chat_last_info_replay');
      onLastInfoReplay?.call(li);
    }
  }

  void sendReadForUnread(ChatReadDeps deps) {
    if (!deps.ble.isTransportConnected) return;
    for (final m in deps.messages) {
      if (m.isIncoming && m.msgId != null) {
        final key = '${m.from}_${m.msgId}';
        if (!deps.readSent.contains(key)) {
          deps.readSent.add(key);
          deps.ble.sendRead(from: m.from, msgId: m.msgId!);
        }
      }
    }
  }

  void bindConnectionState({
    required RiftLinkBle ble,
    required bool Function() shouldHandleDisconnect,
    required void Function() onDisconnected,
  }) {
    final dev = ble.device;
    if (dev == null) return;
    final trackedRemoteId = dev.remoteId.toString();
    _connectionSub?.cancel();
    _connectionEventsSub?.cancel();
    _connectionSub = dev.connectionState.listen((state) {
      if (!shouldHandleDisconnect()) return;
      if (state == BluetoothConnectionState.disconnected) {
        _notifyDisconnected(
          ble: ble,
          shouldHandleDisconnect: shouldHandleDisconnect,
          onDisconnected: onDisconnected,
        );
      }
    });
    _connectionEventsSub = FlutterBluePlus.events.onConnectionStateChanged.listen((event) {
      if (!shouldHandleDisconnect()) return;
      final eventRemoteId = event.device.remoteId.toString();
      if (!RiftLinkBle.remoteIdsMatch(eventRemoteId, trackedRemoteId)) return;
      if (event.connectionState == BluetoothConnectionState.disconnected) {
        _notifyDisconnected(
          ble: ble,
          shouldHandleDisconnect: shouldHandleDisconnect,
          onDisconnected: onDisconnected,
        );
      }
    });
  }

  void _notifyDisconnected({
    required RiftLinkBle ble,
    required bool Function() shouldHandleDisconnect,
    required void Function() onDisconnected,
  }) {
    if (!shouldHandleDisconnect()) return;
    if (ble.isWifiMode) return;
    final now = DateTime.now();
    final last = _lastDisconnectHandledAt;
    if (last != null && now.difference(last) < const Duration(milliseconds: 1200)) {
      return;
    }
    _lastDisconnectHandledAt = now;
    onDisconnected();
  }

  void handleBleEvent(RiftLinkEvent evt, ChatBleHandlerDeps deps) {
    if (!deps.isMounted) return;
    if (evt is RiftLinkMsgEvent) {
      final fromNorm = deps.normalizeId(evt.from);
      final groupId = evt.group ?? 0;
      final rawGroupUid = evt.groupUid?.trim().toUpperCase();
      final hasGroupUid = rawGroupUid != null && rawGroupUid.isNotEmpty;
      final hasGroupId = groupId > kMeshBroadcastGroupId;
      final isBroadcastByGroupId = !hasGroupUid && groupId == kMeshBroadcastGroupId;
      final incomingConv = isBroadcastByGroupId
          ? ChatRepository.broadcastConversationId()
          : (hasGroupUid
              ? ChatRepository.groupConversationIdByUid(rawGroupUid!)
              : (hasGroupId
                  ? ChatRepository.groupConversationIdByUid('UNRESOLVED_$groupId')
                  : ChatRepository.directConversationId(fromNorm)));
      final active = deps.conversationId;
      if (active == null || active.isEmpty) {
        // Guard early init window: do not paint events into an unbound chat UI.
        if (deps.appLifecycle != AppLifecycleState.resumed) {
          deps.showIncomingNotification(evt.from, evt.text);
        }
        return;
      }
      if (active != incomingConv) {
        if (deps.appLifecycle != AppLifecycleState.resumed) {
          deps.showIncomingNotification(evt.from, evt.text);
        }
        return;
      }
      deps.setState(() {
        final ttl = evt.ttlMinutes ?? 0;
        deps.messages.add(ChatUiMessage(
          from: evt.from,
          text: evt.text,
          isIncoming: true,
          msgId: evt.msgId,
          rssi: evt.rssi,
          lane: evt.lane,
          type: evt.type,
          deleteAt: ttl > 0 ? DateTime.now().add(Duration(minutes: ttl)) : null,
        ));
      });
      deps.markConversationRead(active);
      if (deps.appLifecycle != AppLifecycleState.resumed) {
        deps.showIncomingNotification(evt.from, evt.text);
      }
      if (incomingConv.startsWith('direct:')) {
        deps.sendReadForUnread();
      }
      deps.scrollToBottom();
      return;
    }
    if (evt is RiftLinkSentEvent) {
      if (!_hasValidStatusMsgId(evt.msgId)) {
        debugPrint('[BLE_CHAIN] stage=app_msg_state action=drop reason=invalid_msg_id evt=sent msgId=${evt.msgId}');
        return;
      }
      deps.setState(() {
        final toMatch = evt.to.isEmpty ? null : evt.to;
        var matched = false;
        // Match the most recent pending outgoing first.
        for (var i = deps.messages.length - 1; i >= 0; i--) {
          final m = deps.messages[i];
          final sameTo = (m.to == null && toMatch == null) || deps.sameNodeId(m.to, toMatch);
          if (!m.isIncoming && m.msgId == null && sameTo) {
            deps.messages[i] = m.copyWith(
              msgId: evt.msgId,
              to: evt.to,
              status: _mergeUiOutgoingStatus(m.status, ChatUiMessageStatus.sent),
            );
            matched = true;
            break;
          }
        }
        if (!matched) {
          debugPrint(
            '[BLE_CHAIN] stage=app_msg_state action=mismatch evt=sent msgId=${evt.msgId} to=${evt.to}',
          );
        }
      });
      return;
    }
    if (evt is RiftLinkDeliveredEvent) {
      if (!_hasValidStatusMsgId(evt.msgId)) {
        debugPrint('[BLE_CHAIN] stage=app_msg_state action=drop reason=invalid_msg_id evt=delivered msgId=${evt.msgId}');
        return;
      }
      deps.setState(() {
        var matched = false;
        for (var i = 0; i < deps.messages.length; i++) {
          final m = deps.messages[i];
          if (!m.isIncoming && deps.sameNodeId(m.to, evt.from) && m.msgId == evt.msgId) {
            final nextStatus = _mergeUiOutgoingStatus(m.status, ChatUiMessageStatus.delivered);
            var updated = m.copyWith(status: nextStatus);
            if (nextStatus != m.status && deps.shouldEmitCriticalSummary(updated, nextStatus)) {
              updated = updated.copyWith(relaySummarySent: true);
              deps.messages.add(deps.buildCriticalSummaryMessage(updated, nextStatus));
            }
            deps.messages[i] = updated;
            matched = true;
            break;
          }
        }
        if (!matched) {
          // Fallback: delivered/read can be observed even if sent mapping was delayed.
          for (var i = deps.messages.length - 1; i >= 0; i--) {
            final m = deps.messages[i];
            if (m.isIncoming || m.msgId != null || !deps.sameNodeId(m.to, evt.from)) continue;
            final nextStatus = _mergeUiOutgoingStatus(m.status, ChatUiMessageStatus.delivered);
            var updated = m.copyWith(msgId: evt.msgId, status: nextStatus);
            if (nextStatus != m.status && deps.shouldEmitCriticalSummary(updated, nextStatus)) {
              updated = updated.copyWith(relaySummarySent: true);
              deps.messages.add(deps.buildCriticalSummaryMessage(updated, nextStatus));
            }
            deps.messages[i] = updated;
            matched = true;
            break;
          }
        }
        if (!matched && _conversationIsGroupOrBroadcast(deps.conversationId)) {
          for (var i = 0; i < deps.messages.length; i++) {
            final m = deps.messages[i];
            if (m.isIncoming || m.msgId != evt.msgId) continue;
            final nextStatus = _mergeUiOutgoingStatus(m.status, ChatUiMessageStatus.delivered);
            var updated = m.copyWith(status: nextStatus);
            if (nextStatus != m.status && deps.shouldEmitCriticalSummary(updated, nextStatus)) {
              updated = updated.copyWith(relaySummarySent: true);
              deps.messages.add(deps.buildCriticalSummaryMessage(updated, nextStatus));
            }
            deps.messages[i] = updated;
            matched = true;
            break;
          }
        }
        if (!matched) {
          debugPrint(
            '[BLE_CHAIN] stage=app_msg_state action=mismatch evt=delivered msgId=${evt.msgId} from=${evt.from}',
          );
        }
      });
      return;
    }
    if (evt is RiftLinkReadEvent) {
      if (!_hasValidStatusMsgId(evt.msgId)) {
        debugPrint('[BLE_CHAIN] stage=app_msg_state action=drop reason=invalid_msg_id evt=read msgId=${evt.msgId}');
        return;
      }
      deps.setState(() {
        var matched = false;
        for (var i = 0; i < deps.messages.length; i++) {
          final m = deps.messages[i];
          if (!m.isIncoming && deps.sameNodeId(m.to, evt.from) && m.msgId == evt.msgId) {
            final nextStatus = _mergeUiOutgoingStatus(m.status, ChatUiMessageStatus.read);
            var updated = m.copyWith(status: nextStatus);
            if (nextStatus != m.status && deps.shouldEmitCriticalSummary(updated, nextStatus)) {
              updated = updated.copyWith(relaySummarySent: true);
              deps.messages.add(deps.buildCriticalSummaryMessage(updated, nextStatus));
            }
            deps.messages[i] = updated;
            matched = true;
            break;
          }
        }
        if (!matched) {
          for (var i = deps.messages.length - 1; i >= 0; i--) {
            final m = deps.messages[i];
            if (m.isIncoming || m.msgId != null || !deps.sameNodeId(m.to, evt.from)) continue;
            final nextStatus = _mergeUiOutgoingStatus(m.status, ChatUiMessageStatus.read);
            var updated = m.copyWith(msgId: evt.msgId, status: nextStatus);
            if (nextStatus != m.status && deps.shouldEmitCriticalSummary(updated, nextStatus)) {
              updated = updated.copyWith(relaySummarySent: true);
              deps.messages.add(deps.buildCriticalSummaryMessage(updated, nextStatus));
            }
            deps.messages[i] = updated;
            matched = true;
            break;
          }
        }
        if (!matched && _conversationIsGroupOrBroadcast(deps.conversationId)) {
          for (var i = 0; i < deps.messages.length; i++) {
            final m = deps.messages[i];
            if (m.isIncoming || m.msgId != evt.msgId) continue;
            final nextStatus = _mergeUiOutgoingStatus(m.status, ChatUiMessageStatus.read);
            var updated = m.copyWith(status: nextStatus);
            if (nextStatus != m.status && deps.shouldEmitCriticalSummary(updated, nextStatus)) {
              updated = updated.copyWith(relaySummarySent: true);
              deps.messages.add(deps.buildCriticalSummaryMessage(updated, nextStatus));
            }
            deps.messages[i] = updated;
            matched = true;
            break;
          }
        }
        if (!matched) {
          debugPrint(
            '[BLE_CHAIN] stage=app_msg_state action=mismatch evt=read msgId=${evt.msgId} from=${evt.from}',
          );
        }
      });
      return;
    }
    if (evt is RiftLinkUndeliveredEvent) {
      if (!_hasValidStatusMsgId(evt.msgId)) {
        debugPrint(
          '[BLE_CHAIN] stage=app_msg_state action=drop reason=invalid_msg_id evt=undelivered msgId=${evt.msgId}',
        );
        return;
      }
      deps.setState(() {
        var matched = false;
        for (var i = 0; i < deps.messages.length; i++) {
          final m = deps.messages[i];
          if (!m.isIncoming && m.msgId == evt.msgId) {
            final isBroadcast = evt.to.isEmpty || deps.sameNodeId(m.to, deps.broadcastTo);
            if (isBroadcast || deps.sameNodeId(m.to, evt.to)) {
              final nextStatus = _mergeUiOutgoingStatus(m.status, ChatUiMessageStatus.undelivered);
              var updated = m.copyWith(
                status: nextStatus,
                delivered: evt.delivered ?? 0,
                total: evt.total ?? 0,
              );
              if (nextStatus != m.status && deps.shouldEmitCriticalSummary(updated, nextStatus)) {
                updated = updated.copyWith(relaySummarySent: true);
                deps.messages.add(deps.buildCriticalSummaryMessage(updated, nextStatus));
              }
              deps.messages[i] = updated;
              matched = true;
              break;
            }
          }
        }
        if (!matched) {
          for (var i = deps.messages.length - 1; i >= 0; i--) {
            final m = deps.messages[i];
            if (m.isIncoming || m.msgId != null) continue;
            final isBroadcast = evt.to.isEmpty || deps.sameNodeId(m.to, deps.broadcastTo);
            if (!isBroadcast && !deps.sameNodeId(m.to, evt.to)) continue;
            final nextStatus = _mergeUiOutgoingStatus(m.status, ChatUiMessageStatus.undelivered);
            var updated = m.copyWith(
              msgId: evt.msgId,
              status: nextStatus,
              delivered: evt.delivered ?? 0,
              total: evt.total ?? 0,
            );
            if (nextStatus != m.status && deps.shouldEmitCriticalSummary(updated, nextStatus)) {
              updated = updated.copyWith(relaySummarySent: true);
              deps.messages.add(deps.buildCriticalSummaryMessage(updated, nextStatus));
            }
            deps.messages[i] = updated;
            matched = true;
            break;
          }
        }
        if (!matched) {
          debugPrint(
            '[BLE_CHAIN] stage=app_msg_state action=mismatch evt=undelivered msgId=${evt.msgId} to=${evt.to}',
          );
        }
      });
      return;
    }
    if (evt is RiftLinkBroadcastDeliveryEvent) {
      if (!_hasValidStatusMsgId(evt.msgId)) {
        debugPrint(
          '[BLE_CHAIN] stage=app_msg_state action=drop reason=invalid_msg_id evt=broadcast_delivery msgId=${evt.msgId}',
        );
        return;
      }
      deps.setState(() {
        for (var i = 0; i < deps.messages.length; i++) {
          final m = deps.messages[i];
          if (!m.isIncoming && m.msgId == evt.msgId && (m.to == deps.broadcastTo || m.to == null)) {
            final st = evt.delivered > 0 ? ChatUiMessageStatus.delivered : ChatUiMessageStatus.undelivered;
            final nextStatus = _mergeUiOutgoingStatus(m.status, st);
            var updated = m.copyWith(status: nextStatus, delivered: evt.delivered, total: evt.total);
            if (nextStatus != m.status && deps.shouldEmitCriticalSummary(updated, nextStatus)) {
              updated = updated.copyWith(relaySummarySent: true);
              deps.messages.add(deps.buildCriticalSummaryMessage(updated, nextStatus));
            }
            deps.messages[i] = updated;
            break;
          }
        }
      });
      return;
    }
    if (evt is RiftLinkRelayProofEvent) {
      deps.setState(() {
        for (var i = deps.messages.length - 1; i >= 0; i--) {
          final m = deps.messages[i];
          if (m.isIncoming || m.msgId == null || m.to == null) continue;
          if (!deps.sameNodeId(m.to, evt.to)) continue;
          final pktId = m.msgId! & 0xFFFF;
          if (pktId == evt.pktId) {
            final shortRelay = deps.normalizeId(evt.relayedBy);
            final nextPeers = List<String>.from(m.relayPeers);
            if (shortRelay.isNotEmpty && !nextPeers.contains(shortRelay) && nextPeers.length < 5) {
              nextPeers.add(shortRelay);
            }
            deps.messages[i] = m.copyWith(
              relayCount: (m.relayCount ?? 0) + 1,
              relayPeers: nextPeers,
            );
            break;
          }
        }
        deps.messages.add(ChatUiMessage(
          from: evt.relayedBy,
          text: deps.tr('relay_proof_line', {
            'from': deps.normalizeId(evt.from),
            'to': deps.normalizeId(evt.to),
            'pkt': '${evt.pktId}',
          }),
          isIncoming: true,
          lane: 'normal',
          type: 'relayProof',
        ));
      });
      deps.scrollToBottom();
      return;
    }
    if (evt is RiftLinkTimeCapsuleQueuedEvent) {
      deps.onTimeCapsuleQueued(evt);
      return;
    }
    if (evt is RiftLinkTimeCapsuleReleasedEvent) {
      deps.onTimeCapsuleReleased(evt);
      return;
    }
    if (evt is RiftLinkVoiceEvent) {
      final assembly = deps.voiceChunks.putIfAbsent(
        evt.from,
        () => ChatUiVoiceRxAssembly(
          total: evt.total,
          startedAt: DateTime.now(),
          lastUpdated: DateTime.now(),
        ),
      );
      if (assembly.total != evt.total) {
        assembly.total = evt.total;
        assembly.parts.clear();
      }
      assembly.parts[evt.chunk] = evt.data;
      assembly.lastUpdated = DateTime.now();
      if (assembly.parts.length == evt.total) {
        final parts = List.generate(evt.total, (i) => assembly.parts[i] ?? '');
        try {
          final bytes = base64Decode(parts.join());
          final decoded = deps.decodeVoicePayload(bytes);
          deps.voiceChunks.remove(evt.from);
          final profileLabel = decoded.voiceProfileCode != null
              ? ' [${deps.voiceProfileLabel(decoded.voiceProfileCode!)}]'
              : '';
          deps.setState(() {
            deps.messages.add(ChatUiMessage(
              from: evt.from,
              text: '🎤 ${deps.tr('voice')}$profileLabel',
              isIncoming: true,
              isVoice: true,
              voiceData: decoded.bytes,
              deleteAt: decoded.deleteAt,
              voiceProfileCode: decoded.voiceProfileCode,
            ));
          });
          final active = deps.conversationId;
          if (active != null && active == ChatRepository.directConversationId(deps.normalizeId(evt.from))) {
            deps.markConversationRead(active);
          }
          deps.scrollToBottom();
        } catch (_) {
          deps.voiceChunks.remove(evt.from);
          deps.setState(() {
            deps.messages.add(ChatUiMessage(
              from: evt.from,
              text: deps.tr('voice_decode_error'),
              isIncoming: true,
              lane: 'normal',
              type: 'voiceLoss',
            ));
          });
          deps.scrollToBottom();
        }
      }
      return;
    }
    if (evt is RiftLinkInfoEvent) {
      deps.onInfoEvent(evt);
      return;
    }
    if (evt is RiftLinkTelemetryEvent) {
      deps.onTelemetryEvent(evt);
      return;
    }
    if (evt is RiftLinkLocationEvent) {
      deps.onLocationEvent(evt);
      return;
    }
    if (evt is RiftLinkRegionEvent) {
      deps.onRegionEvent(evt);
      return;
    }
    if (evt is RiftLinkPongEvent) {
      deps.onPongEvent(evt);
      return;
    }
    if (evt is RiftLinkErrorEvent) {
      deps.onErrorEvent(evt);
      return;
    }
    if (evt is RiftLinkGroupSecurityErrorEvent) {
      deps.onGroupSecurityErrorEvent(evt);
      return;
    }
    if (evt is RiftLinkWaitingKeyEvent) {
      deps.onWaitingKeyEvent();
      return;
    }
    if (evt is RiftLinkWifiEvent) {
      deps.onWifiEvent(evt);
      return;
    }
    if (evt is RiftLinkGpsEvent) {
      deps.onGpsEvent(evt);
      return;
    }
    if (evt is RiftLinkInviteEvent) {
      deps.onInviteEvent(evt);
      return;
    }
    if (evt is RiftLinkSelftestEvent) {
      deps.onSelftestEvent(evt);
    }
  }

  Future<void> sendTextMessage(
    ChatActionDeps deps, {
    int ttlMinutes = 0,
    String lane = 'normal',
    String? trigger,
    int? triggerAtMs,
  }) async {
    final text = deps.textController.text.trim();
    if (text.isEmpty) return;
    deps.textController.clear();
    final conversationId = deps.activeConversationId();
    deps.setConversationId(conversationId);
    final toNorm = deps.unicastTo != null ? deps.normalizeId(deps.unicastTo!) : null;
    if (conversationId.startsWith('direct:') && toNorm != null) {
      await deps.repo.ensureConversation(
        id: conversationId,
        kind: ConversationKind.direct,
        peerRef: toNorm,
        title: deps.contactNicknames[toNorm] ?? toNorm,
      );
    } else if (conversationId.startsWith('groupv2:')) {
      await deps.repo.ensureConversation(
        id: conversationId,
        kind: ConversationKind.group,
        peerRef: deps.groupUid != null && deps.groupUid!.isNotEmpty
            ? ChatRepository.groupPeerRefByUid(deps.groupUid!)
            : '${deps.group}',
        title: deps.groupTitle,
      );
    } else {
      await deps.repo.ensureConversation(
        id: conversationId,
        kind: ConversationKind.broadcast,
        peerRef: 'broadcast',
        title: 'Broadcast',
      );
    }

    final isGroupSend = deps.group > 1;
    final toForMsg = isGroupSend ? null : (deps.unicastTo ?? deps.broadcastTo);
    var localMessageIndex = -1;
    deps.setState(() {
      deps.messages.add(ChatUiMessage(
        from: deps.nodeId,
        text: text,
        isIncoming: false,
        to: toForMsg,
        lane: lane,
        type: trigger == null ? 'text' : 'timeCapsule',
      ));
      localMessageIndex = deps.messages.length - 1;
    });
    deps.scrollToBottom();
    await deps.repo.setDraft(conversationId, '');
    await deps.repo.appendMessage(
      ChatMessage(
        conversationId: conversationId,
        from: deps.nodeId,
        to: toForMsg,
        groupId: isGroupSend ? deps.group : null,
        groupUid: deps.groupUid,
        text: text,
        type: trigger == null ? 'text' : 'timeCapsule',
        lane: lane,
        direction: MessageDirection.outgoing,
        status: MessageStatus.pending,
        createdAtMs: DateTime.now().millisecondsSinceEpoch,
      ),
    );
    final ok = await deps.ble.send(
      text: text,
      to: deps.unicastTo,
      group: isGroupSend ? deps.group : null,
      ttlMinutes: ttlMinutes,
      lane: lane,
      trigger: trigger,
      triggerAtMs: triggerAtMs,
    );
    if (!ok) {
      debugPrint(
        '[BLE_CHAIN] stage=app_tx action=send_fail chat_send to=${toForMsg ?? 'broadcast'} lane=$lane conversation=$conversationId',
      );
      if (deps.isMounted()) {
        deps.setState(() {
          if (localMessageIndex >= 0 && localMessageIndex < deps.messages.length) {
            final m = deps.messages[localMessageIndex];
            if (!m.isIncoming && m.msgId == null) {
              deps.messages[localMessageIndex] = m.copyWith(
                status: ChatUiMessageStatus.undelivered,
                delivered: 0,
                total: 0,
              );
            }
          }
        });
      }
      deps.showSnack(deps.tr('chat_send_failed'), isError: true);
    }
  }

  Future<void> sendWithTtlPipeline(
    ChatActionDeps deps, {
    required Future<int?> Function() pickTtl,
  }) async {
    final text = deps.textController.text.trim();
    if (text.isEmpty) return;
    final ttl = await pickTtl();
    if (ttl != null && deps.isMounted()) {
      await sendTextMessage(deps, ttlMinutes: ttl);
    }
  }

  Future<void> sendCriticalIfAny(ChatActionDeps deps) async {
    if (deps.textController.text.trim().isEmpty) return;
    await sendTextMessage(deps, lane: 'critical');
  }

  Future<void> sendTimeCapsuleByChoice(
    ChatActionDeps deps, {
    required String choice,
    required int nowMs,
  }) async {
    if (choice == 'target_online') {
      await sendTextMessage(deps, trigger: 'target_online');
      return;
    }
    if (choice == 'deliver_after_time') {
      await sendTextMessage(
        deps,
        trigger: 'deliver_after_time',
        triggerAtMs: nowMs + 5 * 60 * 1000,
      );
    }
  }

  Future<void> sendSosQuick(ChatActionDeps deps) async {
    final ok = await deps.ble.sendSos(text: 'SOS');
    if (!deps.isMounted()) return;
    if (ok) {
      deps.setState(() {
        deps.messages.add(ChatUiMessage(
          from: deps.nodeId,
          text: 'SOS',
          isIncoming: false,
          lane: 'critical',
          type: 'sos',
        ));
      });
      deps.scrollToBottom();
      return;
    }
    deps.showSnack(deps.tr('sos_send_failed'), isError: true);
  }

  Future<void> sendLocation(ChatActionDeps deps) async {
    if (!deps.ble.isTransportConnected) return;
    deps.onLocationLoading(true);
    try {
      var perm = await Geolocator.checkPermission();
      if (perm == LocationPermission.denied) perm = await Geolocator.requestPermission();
      if (perm == LocationPermission.denied || perm == LocationPermission.deniedForever) {
        if (deps.isMounted()) deps.showSnack(deps.tr('loc_denied'));
        return;
      }
      final pos = await Geolocator.getCurrentPosition();
      await deps.ble.sendLocation(
        lat: pos.latitude,
        lon: pos.longitude,
        alt: pos.altitude.toInt(),
      );
      if (deps.isMounted()) {
        deps.setState(() {
          deps.messages.add(ChatUiMessage(
            from: deps.nodeId,
            text: '📍 ${pos.latitude.toStringAsFixed(5)}, ${pos.longitude.toStringAsFixed(5)}',
            isIncoming: false,
            isLocation: true,
          ));
        });
        deps.scrollToBottom();
      }
    } catch (e) {
      if (deps.isMounted()) deps.showSnack(e.toString());
    } finally {
      if (deps.isMounted()) deps.onLocationLoading(false);
    }
  }

  Future<void> sendGpsSyncFromPhone(ChatActionDeps deps) async {
    if (!deps.isMounted() || !deps.ble.isTransportConnected) return;
    try {
      final perm = await Geolocator.checkPermission();
      if (perm == LocationPermission.denied || perm == LocationPermission.deniedForever) return;
      final pos = await Geolocator.getCurrentPosition();
      final utcMs = DateTime.now().millisecondsSinceEpoch;
      await deps.ble.sendGpsSync(
        utcMs: utcMs,
        lat: pos.latitude,
        lon: pos.longitude,
        alt: pos.altitude.toInt(),
      );
    } catch (_) {}
  }

  Future<void> stopVoiceAndSend(ChatActionDeps deps) async {
    final bytes = await VoiceService.stopRecord(maxBytes: deps.voicePlan.maxBytes);
    if (!deps.isMounted() || bytes == null || bytes.isEmpty) {
      if (deps.isMounted()) deps.showSnack(deps.tr('voice_mic_error'));
      return;
    }
    final to = deps.directVoiceTarget;
    if (to == null || to.isEmpty) {
      deps.showSnack(deps.tr('voice_only_direct_chat'), isError: true);
      return;
    }
    List<int> payload = bytes;
    if (deps.voiceTtlMinutes > 0) payload = [0xFF, deps.voiceTtlMinutes, ...payload];
    payload = [0xFE, deps.voiceProfileCode, ...payload];
    final chunkSize = deps.voicePlan.chunkSize;
    final chunks = <String>[];
    for (var i = 0; i < payload.length; i += chunkSize) {
      chunks.add(base64Encode(payload.sublist(
        i,
        (i + chunkSize < payload.length) ? i + chunkSize : payload.length,
      )));
    }
    final ok = await deps.ble.sendVoice(to: to, chunks: chunks);
    if (!deps.isMounted()) return;
    if (ok) {
      deps.setState(() {
        deps.messages.add(ChatUiMessage(
          from: deps.nodeId,
          text: '🎤 ${deps.tr('voice')} [${deps.voiceProfileLabel(deps.voiceProfileCode)}]',
          isIncoming: false,
          isVoice: true,
          voiceProfileCode: deps.voiceProfileCode,
        ));
      });
      deps.scrollToBottom();
      return;
    }
    deps.showSnack(deps.tr('voice_send_error'));
  }

  Future<bool> startVoiceRecord(ChatUiVoiceAdaptivePlan plan) async {
    return VoiceService.startRecord(bitRate: plan.bitRate);
  }

  Future<void> cancelVoiceRecord() async {
    await VoiceService.cancelRecord();
  }

  Future<void> reconnectWithRetry(ChatReconnectDeps deps) async {
    if (deps.isReconnecting() || !deps.isMounted()) return;
    final isWifi = deps.ble.isWifiMode;
    final remoteId = deps.currentRemoteId() ?? deps.ble.device?.remoteId.toString();
    final wifiIp = deps.ble.lastInfo?.wifiIp?.trim();
    if (!isWifi && (remoteId == null || remoteId.isEmpty)) {
      await deps.onReconnectFailed();
      return;
    }
    if (isWifi && (wifiIp == null || wifiIp.isEmpty)) {
      await deps.onReconnectFailed();
      return;
    }
    deps.setReconnectState(reconnecting: true, attempt: 1);
    for (var attempt = 1; attempt <= 3; attempt++) {
      if (!deps.isMounted()) return;
      deps.setReconnectState(reconnecting: true, attempt: attempt);
      deps.showReconnectAttempt(attempt);
      try {
        await deps.ble.disconnect();
        await Future<void>.delayed(const Duration(milliseconds: 500));
        bool ok = false;
        if (isWifi) {
          ok = await deps.ble.connectWifi(wifiIp!);
        } else {
          final device = BluetoothDevice.fromId(remoteId!);
          ok = await deps.ble.connect(device);
        }
        if (deps.isMounted() && ok) {
          await deps.onReconnectSuccess(isWifi ? (wifiIp ?? '') : remoteId!);
          deps.setReconnectState(reconnecting: false, attempt: attempt);
          deps.showReconnectSuccess();
          return;
        }
      } catch (_) {}
      if (attempt < 3) await Future<void>.delayed(const Duration(seconds: 2));
    }
    if (!deps.isMounted()) return;
    deps.setReconnectState(reconnecting: false, attempt: null);
    await deps.ble.disconnect();
    if (!deps.isMounted()) return;
    await deps.onReconnectFailed();
  }

  List<RecentDevice> filterSwitchTargets(
    List<RecentDevice> recentDevices,
    String? currentRemoteId,
  ) {
    return recentDevices
        .where((d) => currentRemoteId == null || !RiftLinkBle.remoteIdsMatch(d.remoteId, currentRemoteId))
        .toList();
  }

  Future<void> handleConnectMenuSelection(ChatConnectDeps deps, String value) async {
    if (value.startsWith('forget:')) return;
    if (value == 'disconnect') {
      deps.setIntentionalDisconnect(true);
      transportReconnectManager?.suppressAutoReconnectUntilNextConnection();
      await deps.ble.disconnect();
      if (!deps.isMounted()) return;
      await deps.goToScan();
      return;
    }
    if (value.startsWith('switch:')) {
      final remoteId = value.substring(7);
      deps.setIntentionalDisconnect(true);
      transportReconnectManager?.suppressAutoReconnectUntilNextConnection();
      await _switchToDevice(deps, remoteId);
    }
  }

  Future<void> _switchToDevice(ChatConnectDeps deps, String remoteId) async {
    deps.setIntentionalDisconnect(true);
    final dev = deps.recentDevices
        .where((d) => RiftLinkBle.remoteIdsMatch(d.remoteId, remoteId))
        .firstOrNull;
    final name = dev?.displayName ?? remoteId;
    deps.showSnack(deps.tr('connecting_to', {'name': name}));
    await deps.ble.disconnect();
    await RiftLinkBle.stopScan();
    await Future<void>.delayed(const Duration(milliseconds: 800));
    if (!deps.isMounted()) return;
    final found = await _scanForRemote(remoteId, const Duration(seconds: 12));
    if (!deps.isMounted()) return;
    if (found != null) {
      final ok = await deps.ble.connect(found);
      if (deps.isMounted() && ok) {
        deps.setIntentionalDisconnect(false);
        await deps.switchChatRepoProfile(remoteId);
        if (!deps.isMounted()) return;
        await deps.goToChatsList();
      } else {
        deps.setIntentionalDisconnect(false);
        await deps.goToScan(deps.tr('ble_no_service'));
      }
      return;
    }
    deps.setIntentionalDisconnect(false);
    await deps.goToScan(deps.tr('ble_timeout'));
  }

  Future<BluetoothDevice?> _scanForRemote(String remoteId, Duration scanDuration) async {
    StreamSubscription? scanSub;
    final foundCompleter = Completer<BluetoothDevice?>();
    scanSub = FlutterBluePlus.scanResults.listen((results) {
      if (foundCompleter.isCompleted) return;
      final r = results.where(RiftLinkBle.isRiftLink).toList();
      for (final r0 in r) {
        if (RiftLinkBle.remoteIdsMatch(r0.device.remoteId.toString(), remoteId)) {
          scanSub?.cancel();
          RiftLinkBle.stopScan();
          if (!foundCompleter.isCompleted) foundCompleter.complete(r0.device);
          return;
        }
      }
    });
    try {
      await RiftLinkBle.startScan(timeout: scanDuration);
      await Future.any([
        foundCompleter.future,
        Future<void>.delayed(scanDuration, () {
          if (!foundCompleter.isCompleted) foundCompleter.complete(null);
        }),
      ]);
    } catch (_) {
      if (!foundCompleter.isCompleted) foundCompleter.complete(null);
    }
    await scanSub.cancel();
    await RiftLinkBle.stopScan();
    return foundCompleter.future;
  }

  Future<void> sendPingAndTrack(ChatPingDeps deps, String id) async {
    final idNorm = deps.normalizeNodeId(id);
    if (idNorm.isEmpty) return;
    // До await: иначе быстрый pong обрабатывается до add в pending (как в списке чатов / direct ping).
    deps.pendingPings.add(idNorm);
    final ok = await deps.ble.sendPing(id);
    if (!deps.isMounted()) {
      deps.pendingPings.remove(idNorm);
      return;
    }
    if (!ok) {
      deps.pendingPings.remove(idNorm);
      deps.showSnack(deps.tr('error'));
      return;
    }
    deps.showSnack(deps.tr('ping_sent', {'id': id}));
    Future.delayed(const Duration(seconds: 20), () {
      if (!deps.isMounted()) return;
      if (deps.pendingPings.remove(idNorm)) {
        deps.showSnack(
          deps.tr('ping_timeout', {'id': id}),
          isError: true,
          duration: const Duration(seconds: 4),
        );
      }
    });
  }

  void dispose() {
    _eventSub?.cancel();
    _connectionSub?.cancel();
    _connectionEventsSub?.cancel();
    _eventSub = null;
    _connectionSub = null;
    _connectionEventsSub = null;
  }
}

class ChatActionDeps {
  final RiftLinkBle ble;
  final ChatRepository repo;
  final TextEditingController textController;
  final List<ChatUiMessage> messages;
  final String nodeId;
  final String? unicastTo;
  final int group;
  final String? groupUid;
  final String groupTitle;
  final String broadcastTo;
  final Map<String, String> contactNicknames;
  final List<String> neighbors;
  final ChatUiVoiceAdaptivePlan voicePlan;
  final int voiceTtlMinutes;
  final int voiceProfileCode;
  final String Function(String) normalizeId;
  final String Function(String key, [Map<String, String>? params]) tr;
  final String Function(int code) voiceProfileLabel;
  final bool Function() isMounted;
  final String Function() activeConversationId;
  final String? directVoiceTarget;
  final void Function(String id) setConversationId;
  final void Function(void Function()) setState;
  final void Function() scrollToBottom;
  final void Function(String text, {bool isError, bool isSuccess, Duration duration}) showSnack;
  final void Function(bool value) onLocationLoading;

  ChatActionDeps({
    required this.ble,
    required this.repo,
    required this.textController,
    required this.messages,
    required this.nodeId,
    required this.unicastTo,
    required this.group,
    required this.groupUid,
    required this.groupTitle,
    required this.broadcastTo,
    required this.contactNicknames,
    required this.neighbors,
    required this.voicePlan,
    required this.voiceTtlMinutes,
    required this.voiceProfileCode,
    required this.normalizeId,
    required this.tr,
    required this.voiceProfileLabel,
    required this.isMounted,
    required this.activeConversationId,
    required this.directVoiceTarget,
    required this.setConversationId,
    required this.setState,
    required this.scrollToBottom,
    required this.showSnack,
    required this.onLocationLoading,
  });
}

class ChatReadDeps {
  final RiftLinkBle ble;
  final List<ChatUiMessage> messages;
  final Set<String> readSent;

  ChatReadDeps({
    required this.ble,
    required this.messages,
    required this.readSent,
  });
}

class ChatReconnectDeps {
  final RiftLinkBle ble;
  final bool Function() isMounted;
  final bool Function() isReconnecting;
  final String? Function() currentRemoteId;
  final void Function({required bool reconnecting, int? attempt}) setReconnectState;
  final void Function(int attempt) showReconnectAttempt;
  final void Function() showReconnectSuccess;
  final Future<void> Function(String remoteId) onReconnectSuccess;
  final Future<void> Function() onReconnectFailed;

  ChatReconnectDeps({
    required this.ble,
    required this.isMounted,
    required this.isReconnecting,
    required this.currentRemoteId,
    required this.setReconnectState,
    required this.showReconnectAttempt,
    required this.showReconnectSuccess,
    required this.onReconnectSuccess,
    required this.onReconnectFailed,
  });
}

class ChatConnectDeps {
  final RiftLinkBle ble;
  final List<RecentDevice> recentDevices;
  final bool Function() isMounted;
  final void Function(bool value) setIntentionalDisconnect;
  final String Function(String key, [Map<String, String>? params]) tr;
  final void Function(String text, {bool isError, bool isSuccess, Duration duration}) showSnack;
  final Future<void> Function([String? message]) goToScan;
  final Future<void> Function() goToChatsList;
  final Future<void> Function(String remoteId) switchChatRepoProfile;

  ChatConnectDeps({
    required this.ble,
    required this.recentDevices,
    required this.isMounted,
    required this.setIntentionalDisconnect,
    required this.tr,
    required this.showSnack,
    required this.goToScan,
    required this.goToChatsList,
    required this.switchChatRepoProfile,
  });
}

class ChatPingDeps {
  final RiftLinkBle ble;
  final Set<String> pendingPings;
  final bool Function() isMounted;
  final String Function(String key, [Map<String, String>? params]) tr;
  final String Function(String raw) normalizeNodeId;
  final void Function(String text, {bool isError, bool isSuccess, Duration duration}) showSnack;

  ChatPingDeps({
    required this.ble,
    required this.pendingPings,
    required this.isMounted,
    required this.tr,
    required this.normalizeNodeId,
    required this.showSnack,
  });
}
