import '../ble/riftlink_ble.dart';
import '../mesh_constants.dart';
import 'chat_repository.dart';

/// Результат маршрутизации входящего [evt] `msg` в [conversationId] и контекст группы.
class IncomingMsgRoute {
  final String conversationId;
  /// `groupUid` или `UNRESOLVED_<channel>`, как в [ChatMessage.groupUid]; для direct/broadcast — null.
  final String? groupContext;
  final bool isBroadcastMesh;

  const IncomingMsgRoute({
    required this.conversationId,
    required this.groupContext,
    required this.isBroadcastMesh,
  });
}

/// Сопоставляет входящее сообщение с беседой: broadcast (`group==1` без uid), группа (`group>1` или uid), direct.
///
/// Если прошивка не передала `groupUid`, но в [localGroups] есть канал с тем же [groupId], подставляем uid
/// (тот же поток чата, что и при полном JSON).
IncomingMsgRoute resolveIncomingMsgRoute({
  required String fromNormalized,
  required RiftLinkMsgEvent evt,
  List<RiftLinkGroupInfo>? localGroups,
}) {
  var groupId = evt.group ?? 0;
  var rawUid = evt.groupUid?.trim().toUpperCase();
  if ((rawUid == null || rawUid.isEmpty) && groupId > kMeshBroadcastGroupId && localGroups != null) {
    for (final g in localGroups) {
      if (g.channelId32 == groupId && g.groupUid.trim().isNotEmpty) {
        rawUid = g.groupUid.trim().toUpperCase();
        break;
      }
    }
  }
  final hasGroupUid = rawUid != null && rawUid.isNotEmpty;
  final hasGroupId = groupId > kMeshBroadcastGroupId;
  final isBroadcastByGroupId = !hasGroupUid && groupId == kMeshBroadcastGroupId;
  final groupContext = hasGroupUid
      ? rawUid!
      : (hasGroupId ? 'UNRESOLVED_$groupId' : null);
  final conversationId = isBroadcastByGroupId
      ? ChatRepository.broadcastConversationId()
      : (groupContext != null
          ? ChatRepository.groupConversationIdByUid(groupContext)
          : ChatRepository.directConversationId(fromNormalized));
  return IncomingMsgRoute(
    conversationId: conversationId,
    groupContext: groupContext,
    isBroadcastMesh: isBroadcastByGroupId,
  );
}
