import 'dart:async';
import 'dart:convert';
import 'dart:math';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../ble/riftlink_ble.dart';
import '../contacts/contacts_service.dart';
import '../l10n/app_localizations.dart';
import '../app_navigator.dart';
import '../app_lifecycle_bridge.dart';
import '../theme/app_theme.dart';
import '../theme/design_tokens.dart';
import '../widgets/app_primitives.dart';
import '../widgets/mesh_background.dart';
import '../chat/chat_models.dart';
import '../chat/chat_repository.dart';
import '../mesh_constants.dart';
import 'contacts_groups_hub_screen.dart';
import 'chat_screen.dart';
import 'map_screen.dart';
import 'mesh_screen.dart';
import 'scan_screen.dart';
import 'settings_hub_screen.dart';

enum _ChatsTab { all, personal, groups, neighbors, archived }

const double _kMenuItemIconSize = 18;
const double _kMenuItemTitleSize = 14.5;
const double _kMenuItemSubtitleSize = 12.5;
const FontWeight _kMenuItemTitleWeight = FontWeight.w600;
const double _kMenuItemLineHeight = 1.2;
const double _kMenuItemMinHeight = 38;
const EdgeInsets _kMenuItemPadding = EdgeInsets.symmetric(horizontal: 10);
const VisualDensity _kMenuItemDensity = VisualDensity(horizontal: 0, vertical: -1);

class ChatsListScreen extends StatefulWidget {
  final RiftLinkBle ble;

  const ChatsListScreen({super.key, required this.ble});

  @override
  State<ChatsListScreen> createState() => _ChatsListScreenState();
}

class _ChatsListScreenState extends State<ChatsListScreen> {
  final _repo = ChatRepository.instance;
  final _searchController = TextEditingController();
  final _searchFocusNode = FocusNode();
  final _scaffoldKey = GlobalKey<ScaffoldState>();
  StreamSubscription<void>? _sub;
  StreamSubscription<RiftLinkEvent>? _bleSub;
  List<ChatConversation> _visible = [];
  List<ChatConversation> _archived = [];
  Map<String, String> _nickById = const {};
  Set<String> _neighborIds = const {};
  String _query = '';
  bool _searchMode = false;
  bool _rightToolsExpanded = false;
  _ChatsTab _activeTab = _ChatsTab.all;
  String? _activeConversationId;
  bool _groupConversationMigrationDone = false;

  @override
  void initState() {
    super.initState();
    _searchFocusNode.addListener(() {
      if (mounted) setState(() {});
    });
    _searchController.addListener(() {
      final next = _searchController.text.trim();
      if (next != _query) {
        setState(() => _query = next);
        _load();
      }
    });
    _sub = _repo.conversationsChanged.listen((_) => _load());
    _bleSub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      if (evt is RiftLinkInfoEvent) {
        setState(() {
          _neighborIds = evt.neighbors.map((e) => e.toUpperCase()).toSet();
        });
      } else if (evt is RiftLinkRoutesEvent || evt is RiftLinkNeighborsEvent) {
        setState(() {});
      } else if (evt is RiftLinkGroupSecurityErrorEvent) {
        final msg = evt.msg.trim().isEmpty ? evt.code : evt.msg;
        _snack('${context.l10n.tr('error')}: $msg');
      }
    });
    _load();
  }

  @override
  void dispose() {
    _sub?.cancel();
    _bleSub?.cancel();
    _searchFocusNode.dispose();
    _searchController.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    if (!_groupConversationMigrationDone) {
      final groupsV2 = widget.ble.lastInfo?.groupsV2 ?? const <RiftLinkGroupV2Info>[];
      if (groupsV2.isNotEmpty) {
        final idToUid = <int, String>{};
        for (final g in groupsV2) {
          final uid = g.groupUid.trim().toUpperCase();
          if (g.channelId32 > 1 && uid.isNotEmpty) {
            idToUid[g.channelId32] = uid;
          }
        }
        if (idToUid.isNotEmpty) {
          await _repo.migrateLegacyGroupConversationsToUid(idToUid);
          _groupConversationMigrationDone = true;
        }
      }
    }
    final list = await _repo.listConversations(query: _query);
    final archived = await _repo.listArchivedConversations();
    final contacts = await ContactsService.load();
    final nickById = ContactsService.buildNicknameMap(contacts);
    final neighbors = (widget.ble.lastInfo?.neighbors ?? const <String>[])
        .map((e) => e.toUpperCase())
        .toSet();
    if (!mounted) return;
    setState(() {
      _visible = list;
      _archived = archived;
      _nickById = nickById;
      _neighborIds = neighbors;
    });
  }

  List<ChatConversation> _sortConversations(List<ChatConversation> chats) {
    final items = [...chats];
    items.sort((a, b) {
      final aBroadcast = a.kind == ConversationKind.broadcast;
      final bBroadcast = b.kind == ConversationKind.broadcast;
      if (aBroadcast != bBroadcast) return aBroadcast ? -1 : 1;
      if (a.pinned != b.pinned) return a.pinned ? -1 : 1;
      return (b.lastMessageAtMs ?? 0).compareTo(a.lastMessageAtMs ?? 0);
    });
    return items;
  }

  List<ChatConversation> _sortConversationsForTab(List<ChatConversation> chats, _ChatsTab tab) {
    final items = [...chats];
    items.sort((a, b) {
      final aAt = a.lastMessageAtMs ?? 0;
      final bAt = b.lastMessageAtMs ?? 0;
      if (tab == _ChatsTab.all) {
        final aBroadcast = a.kind == ConversationKind.broadcast;
        final bBroadcast = b.kind == ConversationKind.broadcast;
        if (aBroadcast != bBroadcast) return aBroadcast ? -1 : 1;
        return bAt.compareTo(aAt);
      }
      if (a.pinned != b.pinned) return a.pinned ? -1 : 1;
      return bAt.compareTo(aAt);
    });
    return items;
  }

  String _titleForConversation(ChatConversation c) {
    if (c.kind == ConversationKind.direct) {
      return ContactsService.displayNodeLabel(c.peerRef, _nickById);
    }
    return c.title;
  }

  bool _matchesTab(ChatConversation c, _ChatsTab tab) {
    switch (tab) {
      case _ChatsTab.all:
        return !c.archived;
      case _ChatsTab.personal:
        return !c.archived && c.kind == ConversationKind.direct;
      case _ChatsTab.groups:
        return !c.archived && c.kind == ConversationKind.group;
      case _ChatsTab.neighbors:
        return !c.archived && c.kind == ConversationKind.direct && _neighborIds.contains(c.peerRef.toUpperCase());
      case _ChatsTab.archived:
        return c.archived;
    }
  }

  List<ChatConversation> _tabItems() {
    if (_activeTab == _ChatsTab.archived) {
      final filtered = _archived.where((c) => _matchesTab(c, _activeTab)).toList();
      return _sortConversationsForTab(filtered, _activeTab);
    }
    final filtered = _visible.where((c) => _matchesTab(c, _activeTab)).toList();
    final sorted = _sortConversationsForTab(filtered, _activeTab);
    if (_activeTab != _ChatsTab.all) return sorted;
    return _withBroadcastAlwaysOnTop(sorted);
  }

  List<ChatConversation> _withBroadcastAlwaysOnTop(List<ChatConversation> chats) {
    final broadcastId = ChatRepository.broadcastConversationId();
    ChatConversation? broadcast;
    for (final c in chats) {
      if (c.kind == ConversationKind.broadcast || c.id == broadcastId) {
        broadcast = c;
        break;
      }
    }
    broadcast ??= ChatConversation(
      id: broadcastId,
      kind: ConversationKind.broadcast,
      peerRef: 'broadcast',
      title: context.l10n.tr('broadcast'),
    );
    final rest = chats.where((c) => c.id != broadcast!.id).toList();
    return [broadcast, ...rest];
  }

  RiftLinkGroupV2Info? _groupV2ById(int groupId) {
    final li = widget.ble.lastInfo;
    if (li == null) return null;
    for (final g in li.groupsV2) {
      if (g.channelId32 == groupId) return g;
    }
    return null;
  }

  String _groupDisplayNameById(int groupId) {
    final name = _groupV2ById(groupId)?.canonicalName.trim();
    if (name != null && name.isNotEmpty) return name;
    return 'Group $groupId';
  }

  List<int> _knownGroupIds() {
    final out = <int>{};
    final li = widget.ble.lastInfo;
    if (li != null) {
      out.addAll(
        li.groupsV2
            .map((g) => g.channelId32)
            .where((g) => g > 1 && g != kMeshBroadcastGroupId),
      );
    }
    for (final c in _visible) {
      if (c.kind != ConversationKind.group) continue;
      final gid = _groupIdFromPeerRef(c.peerRef);
      if (gid != null && gid > 1) out.add(gid);
    }
    final sorted = out.toList()..sort();
    return sorted;
  }

  String? _groupUidById(int groupId) => _groupV2ById(groupId)?.groupUid;

  int? _groupIdFromPeerRef(String peerRef) {
    final direct = int.tryParse(peerRef);
    if (direct != null && direct > 1) return direct;
    final uid = ChatRepository.groupUidFromPeerRef(peerRef);
    if (uid == null) return null;
    final li = widget.ble.lastInfo;
    if (li == null) return null;
    for (final g in li.groupsV2) {
      if (g.groupUid.toUpperCase() == uid.toUpperCase()) return g.channelId32;
    }
    return null;
  }

  String? _groupUidFromPeerRef(String peerRef) {
    final uidFromRef = ChatRepository.groupUidFromPeerRef(peerRef);
    if (uidFromRef != null) return uidFromRef;
    final gid = int.tryParse(peerRef);
    if (gid == null) return null;
    return _groupUidById(gid);
  }

  String? _groupRoleById(int groupId) => _groupV2ById(groupId)?.myRole;

  bool? _groupKeyActualById(int groupId) => _groupV2ById(groupId)?.ackApplied;

  List<ChatConversation> _groupTabItemsFull() {
    final byGroupId = <int, ChatConversation>{};
    final unresolvedByUid = <String, ChatConversation>{};
    for (final c in _visible) {
      if (c.archived || c.kind != ConversationKind.group) continue;
      final gid = _groupIdFromPeerRef(c.peerRef);
      if (gid != null && gid > 1) {
        byGroupId[gid] = c;
        continue;
      }
      final uid = _groupUidFromPeerRef(c.peerRef);
      if (uid != null && uid.isNotEmpty) {
        unresolvedByUid[uid] = c;
      }
    }
    final groupsFromV2 = (widget.ble.lastInfo?.groupsV2 ?? const <RiftLinkGroupV2Info>[])
        .map((e) => e.channelId32)
        .where((g) => g > 1 && g != kMeshBroadcastGroupId);
    for (final gid in groupsFromV2) {
      final uid = _groupUidById(gid);
      final displayName = _groupDisplayNameById(gid);
      byGroupId.putIfAbsent(
        gid,
        () => ChatConversation(
          id: uid != null && uid.isNotEmpty
              ? ChatRepository.groupConversationIdByUid(uid)
              : ChatRepository.groupConversationIdByUid('UNRESOLVED_$gid'),
          kind: ConversationKind.group,
          peerRef: uid != null && uid.isNotEmpty
              ? ChatRepository.groupPeerRefByUid(uid)
              : '$gid',
          title: displayName,
        ),
      );
      final existing = byGroupId[gid];
      if (existing != null && existing.title != displayName) {
        byGroupId[gid] = existing.copyWith(title: displayName);
      }
    }
    final out = <ChatConversation>[
      ...byGroupId.values,
      ...unresolvedByUid.values,
    ];
    out.sort((a, b) {
      if (a.pinned != b.pinned) return a.pinned ? -1 : 1;
      final aAt = a.lastMessageAtMs ?? 0;
      final bAt = b.lastMessageAtMs ?? 0;
      if (aAt != bAt) return bAt.compareTo(aAt);
      final ag = _groupIdFromPeerRef(a.peerRef) ?? 0;
      final bg = _groupIdFromPeerRef(b.peerRef) ?? 0;
      if (ag != bg) return ag.compareTo(bg);
      return a.title.compareTo(b.title);
    });
    return out;
  }

  String? _nicknameForNode(String nodeId) {
    final key = nodeId.toUpperCase();
    if (_nickById.containsKey(key)) return _nickById[key];
    final short = key.length >= 8 ? key.substring(0, 8) : key;
    return _nickById[short];
  }

  List<_ReachableNodeItem> _reachableNeighbors() {
    final li = widget.ble.lastInfo;
    if (li == null) return const [];
    final out = <String, _ReachableNodeItem>{};
    final selfId = li.id.toUpperCase();

    for (var i = 0; i < li.neighbors.length; i++) {
      final id = li.neighbors[i].toUpperCase();
      if (id.isEmpty || id == selfId) continue;
      final rssi = i < li.neighborsRssi.length ? li.neighborsRssi[i] : null;
      final hasKey = i < li.neighborsHasKey.length ? li.neighborsHasKey[i] : false;
      out[id] = _ReachableNodeItem(
        nodeId: id,
        nickname: _nicknameForNode(id),
        isDirect: true,
        rssi: rssi,
        hops: null,
        hasKey: hasKey,
      );
    }

    for (final route in li.routes) {
      final id = (route['dest']?.toString() ?? '').toUpperCase();
      if (id.isEmpty || id == selfId) continue;
      final hops = route['hops'] is num ? (route['hops'] as num).toInt() : null;
      final rssi = route['rssi'] is num ? (route['rssi'] as num).toInt() : null;
      final current = out[id];
      if (current == null) {
        out[id] = _ReachableNodeItem(
          nodeId: id,
          nickname: _nicknameForNode(id),
          isDirect: false,
          rssi: rssi,
          hops: hops,
          hasKey: false,
        );
        continue;
      }
      if (!current.isDirect) {
        final currentHops = current.hops ?? 999;
        final nextHops = hops ?? 999;
        if (nextHops < currentHops) {
          out[id] = current.copyWith(hops: hops, rssi: rssi);
        }
      }
    }

    final list = out.values.toList();
    list.sort((a, b) {
      if (a.isDirect != b.isDirect) return a.isDirect ? -1 : 1;
      if (a.isDirect && b.isDirect) {
        final ar = a.rssi ?? -999;
        final br = b.rssi ?? -999;
        if (ar != br) return br.compareTo(ar);
      } else {
        final ah = a.hops ?? 999;
        final bh = b.hops ?? 999;
        if (ah != bh) return ah.compareTo(bh);
      }
      return a.nodeId.compareTo(b.nodeId);
    });
    return list;
  }

  Widget _buildReachableNodeTile(_ReachableNodeItem item) {
    final p = context.palette;
    final title = ContactsService.displayNodeLabel(item.nodeId, _nickById);
    final metric = item.isDirect ? 'RSSI ${item.rssi ?? '--'}' : 'HOPS ${item.hops ?? '?'}';
    return AppSectionCard(
      margin: const EdgeInsets.only(bottom: AppSpacing.xs + 2),
      padding: EdgeInsets.zero,
      child: ListTile(
        dense: true,
        minTileHeight: _kMenuItemMinHeight,
        visualDensity: _kMenuItemDensity,
        contentPadding: const EdgeInsets.symmetric(horizontal: AppSpacing.md, vertical: 2),
        onTap: () => _openDirect(item.nodeId),
        leading: CircleAvatar(
          radius: 19,
          backgroundColor: item.isDirect ? p.primary.withOpacity(0.14) : p.surfaceVariant.withOpacity(0.85),
          child: Icon(
            item.isDirect ? Icons.radar_rounded : Icons.alt_route_rounded,
            size: 18,
            color: item.isDirect ? p.primary : p.onSurfaceVariant,
          ),
        ),
        title: Text(
          title,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: TextStyle(color: p.onSurface, fontWeight: FontWeight.w600, fontSize: 14.5),
        ),
        subtitle: Text(
          metric,
          style: TextStyle(color: p.onSurfaceVariant, fontSize: 12.5, fontWeight: FontWeight.w500),
        ),
        trailing: Icon(
          item.hasKey ? Icons.key_rounded : Icons.key_off_rounded,
          size: 16,
          color: item.hasKey ? p.primary : p.onSurfaceVariant.withOpacity(0.85),
        ),
      ),
    );
  }

  ({IconData icon, String title}) _tabMeta(AppLocalizations l) {
    return switch (_activeTab) {
      _ChatsTab.all => (icon: Icons.forum_outlined, title: l.tr('chats_folder_all')),
      _ChatsTab.personal => (icon: Icons.person_outline_rounded, title: l.tr('chats_folder_personal')),
      _ChatsTab.groups => (icon: Icons.group_outlined, title: l.tr('chats_folder_groups')),
      _ChatsTab.neighbors => (icon: Icons.radar_rounded, title: l.tr('neighbors')),
      _ChatsTab.archived => (icon: Icons.archive_outlined, title: l.tr('chats_folder_archived')),
    };
  }

  ({String tooltip, IconData icon, VoidCallback onPressed})? _fabConfigForTab(AppLocalizations l) {
    return switch (_activeTab) {
      _ChatsTab.all => (
        tooltip: l.tr('compose_message'),
        icon: Icons.add_comment_rounded,
        onPressed: _showNewChatSheet,
      ),
      _ChatsTab.personal => (
        tooltip: l.tr('new_chat'),
        icon: Icons.person_add_alt_1_rounded,
        onPressed: _showNewChatSheet,
      ),
      _ChatsTab.groups => (
        tooltip: l.tr('groups'),
        icon: Icons.group_add_rounded,
        onPressed: _showGroupFabSheet,
      ),
      _ChatsTab.neighbors => null,
      _ChatsTab.archived => null,
    };
  }

  Future<void> _openConversation(ChatConversation c) async {
    if (mounted) setState(() => _activeConversationId = c.id);
    await _repo.markConversationRead(c.id);
    if (!mounted) return;
    await appPush(
      context,
      ChatScreen(
        ble: widget.ble,
        conversationId: c.id,
        initialPeerId: c.kind == ConversationKind.direct ? c.peerRef : null,
        initialGroupId: c.kind == ConversationKind.group ? _groupIdFromPeerRef(c.peerRef) : null,
        initialGroupUid: c.kind == ConversationKind.group ? _groupUidFromPeerRef(c.peerRef) : null,
        initialBroadcast: c.kind == ConversationKind.broadcast,
      ),
    );
  }

  Future<void> _openDirect(String peerId) async {
    final id = ChatRepository.directConversationId(peerId.toUpperCase());
    await _repo.ensureConversation(
      id: id,
      kind: ConversationKind.direct,
      peerRef: peerId.toUpperCase(),
      title: await ContactsService.getNickname(peerId.toUpperCase()) ?? peerId.toUpperCase(),
    );
    if (mounted) setState(() => _activeConversationId = id);
    if (!mounted) return;
    await appPush(
      context,
      ChatScreen(
        ble: widget.ble,
        conversationId: id,
        initialPeerId: peerId.toUpperCase(),
      ),
    );
  }

  Future<void> _openGroup(int groupId, {String? groupUid}) async {
    final uid = ((groupUid ?? _groupUidById(groupId))?.toUpperCase() ?? 'UNRESOLVED_$groupId');
    if (uid.isEmpty) {
      _snack(context.l10n.tr('error'));
      return;
    }
    final id = ChatRepository.groupConversationIdByUid(uid);
    await _repo.ensureConversation(
      id: id,
      kind: ConversationKind.group,
      peerRef: ChatRepository.groupPeerRefByUid(uid),
      title: _groupDisplayNameById(groupId),
    );
    if (mounted) setState(() => _activeConversationId = id);
    if (!mounted) return;
    await appPush(
      context,
      ChatScreen(
        ble: widget.ble,
        conversationId: id,
        initialGroupId: groupId,
        initialGroupUid: uid,
      ),
    );
  }

  Future<void> _openBroadcast() async {
    final id = ChatRepository.broadcastConversationId();
    await _repo.ensureConversation(
      id: id,
      kind: ConversationKind.broadcast,
      peerRef: 'broadcast',
      title: 'Broadcast',
    );
    if (mounted) setState(() => _activeConversationId = id);
    if (!mounted) return;
    await appPush(
      context,
      ChatScreen(
        ble: widget.ble,
        conversationId: id,
        initialBroadcast: true,
      ),
    );
  }

  String _formatTime(int? ms) {
    if (ms == null) return '';
    final d = DateTime.fromMillisecondsSinceEpoch(ms);
    final h = d.hour.toString().padLeft(2, '0');
    final m = d.minute.toString().padLeft(2, '0');
    return '$h:$m';
  }

  Future<void> _togglePin(ChatConversation c) async {
    await _repo.setPinned(c.id, !c.pinned);
    await _load();
  }

  Future<void> _toggleArchive(ChatConversation c) async {
    await _repo.setArchived(c.id, !c.archived);
    await _load();
  }

  Future<void> _toggleMute(ChatConversation c) async {
    await _repo.setMuted(c.id, !c.muted);
    await _load();
  }

  Future<void> _setFolder(ChatConversation c, String folderId) async {
    if (folderId == 'groups' &&
        (c.kind == ConversationKind.group || c.kind == ConversationKind.direct)) {
      _snack(context.l10n.tr('error'));
      return;
    }
    await _repo.setFolder(c.id, folderId);
    await _load();
  }

  Future<void> _deleteConversation(ChatConversation c) async {
    if (c.kind == ConversationKind.broadcast) return;
    final l = context.l10n;
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text(l.tr('delete_chat_title')),
        content: Text(
          l.tr(
            'delete_chat_confirm',
            {'name': _titleForConversation(c)},
          ),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx, false),
            child: Text(l.tr('cancel')),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(ctx, true),
            child: Text(l.tr('delete')),
          ),
        ],
      ),
    );
    if (confirmed != true) return;
    if (_activeConversationId == c.id && mounted) {
      setState(() => _activeConversationId = null);
    }
    await _repo.deleteConversation(c.id);
    await _load();
  }

  Future<void> _showTopMenu() async {
    _scaffoldKey.currentState?.openEndDrawer();
  }

  Color _batteryColorForMv(int mv) {
    final v = mv / 1000.0;
    if (v >= 3.75) return context.palette.success;
    if (v >= 3.45) return const Color(0xFFFFB300);
    return context.palette.error;
  }

  int? _bestNeighborRssi(RiftLinkInfoEvent? info) {
    final list = info?.neighborsRssi ?? const <int>[];
    if (list.isEmpty) return null;
    final valid = list.where((v) => v != 0).toList();
    if (valid.isEmpty) return null;
    valid.sort((a, b) => b.compareTo(a));
    return valid.first;
  }

  Future<void> _showNodeStatusSheet() async {
    final l = context.l10n;
    final li = widget.ble.lastInfo;
    final connected = widget.ble.isConnected;
    final bestRssi = _bestNeighborRssi(li);
    final batteryMv = li?.batteryMv;
    final batteryPercent = li?.batteryPercent;
    final charging = li?.charging ?? false;
    final batteryText = batteryMv == null || batteryMv <= 0
        ? '—'
        : (batteryPercent != null
            ? '$batteryPercent% (${(batteryMv / 1000.0).toStringAsFixed(2)}V)'
            : '${(batteryMv / 1000.0).toStringAsFixed(2)}V');
    final rows = <(IconData, String, String, Color?)>[
      (
        connected ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
        l.tr('node_status_ble_link'),
        connected
            ? l.tr('node_status_ble_link_connected')
            : l.tr('node_status_ble_link_disconnected'),
        connected ? context.palette.success : null,
      ),
      (
        charging ? Icons.battery_charging_full : Icons.battery_std,
        l.tr('settings_energy_node'),
        batteryText,
        batteryMv != null && batteryMv > 0 ? _batteryColorForMv(batteryMv) : null,
      ),
      (
        Icons.radar_rounded,
        l.tr('node_status_rssi'),
        bestRssi != null ? '$bestRssi dBm' : '—',
        null,
      ),
      (
        Icons.memory_rounded,
        l.tr('settings_firmware_version'),
        (li?.version?.trim().isNotEmpty ?? false) ? li!.version!.trim() : '—',
        null,
      ),
      (
        Icons.outbox_rounded,
        l.tr('offline_pending'),
        '${li?.offlinePending ?? 0}',
        null,
      ),
      (
        Icons.gps_fixed_rounded,
        l.tr('gps_section'),
        (li?.gpsEnabled ?? false)
            ? ((li?.gpsFix ?? false) ? l.tr('gps_fix_yes') : l.tr('gps_fix_no'))
            : l.tr('node_status_gps_disabled'),
        null,
      ),
    ];
    await showModalBottomSheet<void>(
      context: context,
      backgroundColor: Colors.transparent,
      builder: (ctx) {
        final p = ctx.palette;
        return SafeArea(
          child: Container(
            decoration: BoxDecoration(
              color: p.card,
              borderRadius: const BorderRadius.vertical(top: Radius.circular(AppSpacing.lg)),
            ),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Padding(
                  padding: const EdgeInsets.fromLTRB(AppSpacing.lg, AppSpacing.md, AppSpacing.lg, AppSpacing.sm),
                  child: Text(
                    l.tr('node_status_title'),
                    style: AppTypography.screenTitleBase().copyWith(
                      fontSize: 18,
                      color: p.onSurface,
                    ),
                  ),
                ),
                ...rows.map((r) => ListTile(
                      dense: true,
                      leading: Icon(r.$1, color: r.$4 ?? p.onSurfaceVariant),
                      title: Text(r.$2, style: TextStyle(color: p.onSurfaceVariant)),
                      trailing: Text(
                        r.$3,
                        style: TextStyle(
                          color: p.onSurface,
                          fontWeight: FontWeight.w600,
                        ),
                      ),
                    )),
                const SizedBox(height: AppSpacing.sm),
              ],
            ),
          ),
        );
      },
    );
  }

  Future<void> _onRightMenuAction(String selected) async {
    if (!mounted) return;
    final l = context.l10n;
    if (selected == 'node_status') {
      await _showNodeStatusSheet();
      return;
    }
    if (selected == 'disconnect') {
      await widget.ble.disconnect();
      if (!mounted) return;
      await appResetTo(context, const ScanScreen());
      return;
    }
    if (selected == 'contacts_hub') {
      final li = widget.ble.lastInfo;
      await appPush(
        context,
        ContactsGroupsHubScreen(
          ble: widget.ble,
          neighbors: li?.neighbors ?? const <String>[],
          initialGroups: li?.groups ?? const <int>[],
        ),
      );
      return;
    }
    if (selected == 'settings') {
      final li = widget.ble.lastInfo;
      await appPush(
        context,
        SettingsHubScreen(
          ble: widget.ble,
          nodeId: li?.id ?? '',
          nickname: li?.nickname,
          region: li?.region ?? 'EU',
          channel: li?.channel,
          sf: li?.sf,
          gpsPresent: li?.gpsPresent ?? false,
          gpsEnabled: li?.gpsEnabled ?? false,
          gpsFix: li?.gpsFix ?? false,
          powersave: li?.powersave ?? false,
          offlinePending: li?.offlinePending,
          batteryMv: li?.batteryMv,
          onNicknameChanged: (_) {},
          onRegionChanged: (_, __) {},
          onSfChanged: (_) {},
          onPowersaveChanged: (_) {},
          onGpsChanged: (_) {},
          meshAnimationEnabled: true,
          onMeshAnimationChanged: (_) {},
        ),
      );
      return;
    }
    _snack(l.tr('error'));
  }

  Future<void> _onToolAction(String selected) async {
    final l = context.l10n;
    switch (selected) {
      case 'map':
        await appPush(context, MapScreen(ble: widget.ble));
        break;
      case 'mesh':
        final li = widget.ble.lastInfo;
        await appPush(
          context,
          MeshScreen(
            ble: widget.ble,
            nodeId: li?.id ?? '',
            neighbors: li?.neighbors ?? const <String>[],
            neighborsRssi: li?.neighborsRssi ?? const <int>[],
            routes: li?.routes ?? const <Map<String, dynamic>>[],
          ),
        );
        break;
      case 'ping':
        await _showPingDialog();
        break;
      case 'selftest':
        await widget.ble.selftest();
        _snack(l.tr('selftest'));
        break;
      default:
        break;
    }
  }

  Future<void> _showPingDialog() async {
    final l = context.l10n;
    final controller = TextEditingController();
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text(l.tr('ping_title')),
        content: TextField(
          controller: controller,
          decoration: InputDecoration(hintText: l.tr('ping_hint')),
          textCapitalization: TextCapitalization.characters,
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: Text(l.tr('cancel'))),
          FilledButton(onPressed: () => Navigator.pop(ctx, true), child: Text(l.tr('send'))),
        ],
      ),
    );
    if (ok != true) return;
    final id = controller.text.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();
    if (id.length != 16) {
      _snack(l.tr('ping_invalid'));
      return;
    }
    final sent = await widget.ble.sendPing(id);
    _snack(sent ? l.tr('ping_sent', {'id': id}) : l.tr('error'));
  }

  void _snack(String text) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(text), duration: const Duration(seconds: 2)));
  }

  String _generateBase64Token(int sizeBytes) {
    final rnd = Random.secure();
    final bytes = List<int>.generate(sizeBytes, (_) => rnd.nextInt(256));
    return base64Encode(bytes);
  }

  Future<void> _copyGroupInvite(int groupId) async {
    final l = context.l10n;
    if (!widget.ble.isConnected) return;
    final v2 = _groupV2ById(groupId);
    if (v2 != null && v2.groupUid.trim().isNotEmpty) {
      final ok = await widget.ble.groupInviteCreate(groupUid: v2.groupUid, role: 'member');
      if (!ok) {
        _snack(l.tr('error'));
        return;
      }
      try {
        final evt = await widget.ble.events
            .where((e) => e is RiftLinkGroupInviteEvent && (e as RiftLinkGroupInviteEvent).groupUid == v2.groupUid)
            .cast<RiftLinkGroupInviteEvent>()
            .first
            .timeout(const Duration(seconds: 3));
        await Clipboard.setData(ClipboardData(text: evt.invite));
        if (!mounted) return;
        _snack(l.tr('group_invite_copied', {'id': '$groupId'}));
        return;
      } catch (_) {
        _snack(l.tr('error'));
        return;
      }
    }
    _snack(l.tr('error'));
  }

  Future<void> _copyGroupInviteForConversation(ChatConversation c) async {
    final groupId = _groupIdFromPeerRef(c.peerRef);
    if (groupId == null || groupId <= 1) {
      _snack(context.l10n.tr('error'));
      return;
    }
    await _copyGroupInvite(groupId);
  }

  Future<void> _createGroupFromFab() async {
    final l = context.l10n;
    final c = TextEditingController();
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text(l.tr('group_create')),
        content: TextField(
          controller: c,
          autofocus: true,
          keyboardType: TextInputType.number,
          decoration: InputDecoration(hintText: l.tr('group_id_hint')),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: Text(l.tr('cancel'))),
          FilledButton(onPressed: () => Navigator.pop(ctx, true), child: Text(l.tr('ok'))),
        ],
      ),
    );
    if (ok != true) return;
    final groupId = int.tryParse(c.text.trim());
    if (groupId == null || groupId <= 1 || groupId > 0xFFFFFFFF) {
      _snack(l.tr('invalid_group_id'));
      return;
    }
    if (groupId == kMeshBroadcastGroupId) {
      _snack(l.tr('group_id_reserved'));
      return;
    }
    final groupUid = _generateBase64Token(16);
    final created = await widget.ble.groupCreate(
      groupUid: groupUid,
      displayName: '${l.tr('group')} $groupId',
      channelId32: groupId,
      groupTag: _generateBase64Token(8),
    );
    if (!created) {
      _snack(l.tr('error'));
      return;
    }
    await _openGroup(groupId, groupUid: groupUid);
  }

  Future<void> _joinGroupByInviteCode(String inviteCodeRaw) async {
    final l = context.l10n;
    final acceptedV2 = await widget.ble.groupInviteAccept(inviteCodeRaw.trim());
    if (acceptedV2) {
      await _load();
      _snack(l.tr('group_invite_joined_v2'));
      return;
    }
    _snack(l.tr('group_invite_bad'));
  }

  Future<void> _joinGroupByCodeDialog() async {
    final l = context.l10n;
    final c = TextEditingController();
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text(l.tr('group_join_by_code')),
        content: TextField(
          controller: c,
          autofocus: true,
          maxLines: 2,
          minLines: 1,
          decoration: const InputDecoration(hintText: 'BASE64...'),
        ),
        actions: [
          TextButton(
            onPressed: () async {
              final data = await Clipboard.getData(Clipboard.kTextPlain);
              c.text = data?.text?.trim() ?? '';
            },
            child: Text(l.tr('paste')),
          ),
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: Text(l.tr('cancel'))),
          FilledButton(onPressed: () => Navigator.pop(ctx, true), child: Text(l.tr('ok'))),
        ],
      ),
    );
    if (ok != true) return;
    await _joinGroupByInviteCode(c.text);
  }

  Future<void> _showGroupFabSheet() async {
    final l = context.l10n;
    await showModalBottomSheet<void>(
      context: context,
      isScrollControlled: true,
      builder: (ctx) {
        final p = ctx.palette;
        return SafeArea(
          child: DraggableScrollableSheet(
            expand: false,
            initialChildSize: 0.34,
            minChildSize: 0.28,
            maxChildSize: 0.52,
            builder: (_, controller) => ListView(
              controller: controller,
              padding: const EdgeInsets.fromLTRB(AppSpacing.lg, AppSpacing.md, AppSpacing.lg, AppSpacing.lg),
              children: [
                Center(
                  child: Container(
                    width: 36,
                    height: 3,
                    decoration: BoxDecoration(
                      color: p.onSurfaceVariant.withOpacity(0.3),
                      borderRadius: BorderRadius.circular(AppRadius.sm),
                    ),
                  ),
                ),
                const SizedBox(height: AppSpacing.md),
                Text(
                  l.tr('groups'),
                  style: TextStyle(
                    color: p.onSurface,
                    fontSize: 17,
                    fontWeight: FontWeight.w700,
                  ),
                ),
                const SizedBox(height: AppSpacing.sm),
                ListTile(
                  dense: true,
                  minTileHeight: _kMenuItemMinHeight,
                  visualDensity: _kMenuItemDensity,
                  contentPadding: _kMenuItemPadding,
                  leading: const Icon(Icons.group_add_rounded),
                  title: Text(
                    l.tr('group_create'),
                    style: const TextStyle(
                      fontSize: _kMenuItemTitleSize,
                      fontWeight: _kMenuItemTitleWeight,
                      height: _kMenuItemLineHeight,
                    ),
                  ),
                  onTap: () async {
                    Navigator.pop(ctx);
                    await _createGroupFromFab();
                  },
                ),
                ListTile(
                  dense: true,
                  minTileHeight: _kMenuItemMinHeight,
                  visualDensity: _kMenuItemDensity,
                  contentPadding: _kMenuItemPadding,
                  leading: const Icon(Icons.vpn_key_outlined),
                  title: Text(
                    l.tr('group_join_by_code'),
                    style: const TextStyle(
                      fontSize: _kMenuItemTitleSize,
                      fontWeight: _kMenuItemTitleWeight,
                      height: _kMenuItemLineHeight,
                    ),
                  ),
                  onTap: () async {
                    Navigator.pop(ctx);
                    await _joinGroupByCodeDialog();
                  },
                ),
                ListTile(
                  dense: true,
                  minTileHeight: _kMenuItemMinHeight,
                  visualDensity: _kMenuItemDensity,
                  contentPadding: _kMenuItemPadding,
                  leading: const Icon(Icons.admin_panel_settings_outlined),
                  title: Text(
                    l.tr('contacts_groups_title'),
                    style: const TextStyle(
                      fontSize: _kMenuItemTitleSize,
                      fontWeight: _kMenuItemTitleWeight,
                      height: _kMenuItemLineHeight,
                    ),
                  ),
                  onTap: () async {
                    Navigator.pop(ctx);
                    await _onRightMenuAction('contacts_hub');
                  },
                ),
              ],
            ),
          ),
        );
      },
    );
  }

  Future<void> _showNewChatSheet() async {
    final l = context.l10n;
    final contacts = await ContactsService.load();
    final groups = _knownGroupIds();

    if (!mounted) return;
    final selected = await showModalBottomSheet<String>(
      context: context,
      isScrollControlled: true,
      builder: (ctx) {
        final p = ctx.palette;
        return SafeArea(
          child: DraggableScrollableSheet(
            expand: false,
            initialChildSize: 0.7,
            maxChildSize: 0.9,
            minChildSize: 0.4,
            builder: (_, controller) => ListView(
              controller: controller,
              padding: const EdgeInsets.fromLTRB(AppSpacing.lg, AppSpacing.md, AppSpacing.lg, AppSpacing.lg),
              children: [
                Text(l.tr('compose_message'), style: TextStyle(color: p.onSurface, fontWeight: FontWeight.w700, fontSize: 17)),
                const SizedBox(height: AppSpacing.md),
                _sheetSectionTitle(ctx, l.tr('new_chat_contacts')),
                ...contacts.map((c) => _sheetActionTile(
                      context: ctx,
                      icon: Icons.person_outline_rounded,
                      title: c.nickname,
                      subtitle: c.id,
                      onTap: () => Navigator.pop(ctx, 'direct:${c.id.toUpperCase()}'),
                    )),
                const SizedBox(height: AppSpacing.sm),
                _sheetSectionTitle(ctx, l.tr('new_chat_groups')),
                if (groups.isEmpty)
                  _sheetEmptyTile(context: ctx, text: l.tr('no_groups')),
                ...groups.map((g) => _sheetActionTile(
                      context: ctx,
                      icon: Icons.group_outlined,
                      title: '${l.tr('group')} $g',
                      onTap: () => Navigator.pop(ctx, 'groupv2id:$g'),
                    )),
                const SizedBox(height: AppSpacing.sm),
                _sheetSectionTitle(ctx, l.tr('new_chat_broadcast')),
                _sheetActionTile(
                  context: ctx,
                  icon: Icons.campaign_outlined,
                  title: l.tr('broadcast'),
                  onTap: () => Navigator.pop(ctx, 'broadcast'),
                ),
                const SizedBox(height: AppSpacing.sm),
                _sheetSectionTitle(ctx, l.tr('menu_title')),
                _sheetActionTile(
                  context: ctx,
                  icon: Icons.admin_panel_settings_outlined,
                  title: l.tr('contacts_groups_title'),
                  onTap: () => Navigator.pop(ctx, 'groups_hub'),
                ),
              ],
            ),
          ),
        );
      },
    );
    if (!mounted || selected == null) return;
    if (selected == 'broadcast') {
      await _openBroadcast();
      return;
    }
    if (selected == 'groups_hub') {
      await _onRightMenuAction('contacts_hub');
      return;
    }
    if (selected.startsWith('groupv2id:')) {
      final g = int.tryParse(selected.substring('groupv2id:'.length));
      if (g != null) await _openGroup(g, groupUid: _groupUidById(g));
      return;
    }
    if (selected.startsWith('direct:')) {
      final id = selected.substring('direct:'.length);
      if (id.isNotEmpty) await _openDirect(id);
    }
  }

  Widget _sheetSectionTitle(BuildContext context, String text) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(8, AppSpacing.xs, 8, AppSpacing.xs),
      child: Text(
        text,
        style: TextStyle(
          color: context.palette.onSurfaceVariant,
          fontWeight: FontWeight.w600,
          fontSize: 12,
        ),
      ),
    );
  }

  Widget _sheetEmptyTile({
    required BuildContext context,
    required String text,
  }) {
    final p = context.palette;
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
      child: Text(
        text,
        style: TextStyle(
          color: p.onSurfaceVariant,
          fontSize: _kMenuItemSubtitleSize,
          fontWeight: _kMenuItemTitleWeight,
        ),
      ),
    );
  }

  Widget _sheetActionTile({
    required BuildContext context,
    required IconData icon,
    required String title,
    String? subtitle,
    required VoidCallback onTap,
  }) {
    final p = context.palette;
    return ListTile(
      dense: true,
      minTileHeight: _kMenuItemMinHeight,
      visualDensity: _kMenuItemDensity,
      contentPadding: _kMenuItemPadding,
      leading: Icon(icon, size: _kMenuItemIconSize, color: p.onSurfaceVariant),
      title: Text(
        title,
        style: TextStyle(
          color: p.onSurface,
          fontSize: _kMenuItemTitleSize,
          fontWeight: _kMenuItemTitleWeight,
          height: _kMenuItemLineHeight,
        ),
      ),
      subtitle: subtitle == null
          ? null
          : Text(
              subtitle,
              style: TextStyle(
                color: p.onSurfaceVariant,
                fontSize: _kMenuItemSubtitleSize,
                fontFamily: 'monospace',
                height: _kMenuItemLineHeight,
              ),
            ),
      onTap: onTap,
    );
  }

  Widget _buildGroupsEmptyState(AppLocalizations l, AppPalette p) {
    return SizedBox(
      height: MediaQuery.of(context).size.height * 0.56,
      child: Center(
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: AppSpacing.xl),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Icon(Icons.group_outlined, size: 56, color: p.onSurfaceVariant.withOpacity(0.38)),
              const SizedBox(height: AppSpacing.sm),
              Text(
                l.tr('no_groups'),
                textAlign: TextAlign.center,
                style: TextStyle(
                  color: p.onSurfaceVariant.withOpacity(0.95),
                  fontSize: 15,
                  fontWeight: FontWeight.w500,
                ),
              ),
              const SizedBox(height: AppSpacing.md),
              Wrap(
                spacing: AppSpacing.sm,
                runSpacing: AppSpacing.sm,
                alignment: WrapAlignment.center,
                children: [
                  FilledButton.icon(
                    onPressed: widget.ble.isConnected ? _createGroupFromFab : null,
                    icon: const Icon(Icons.group_add_rounded, size: 16),
                    label: Text(l.tr('group_create')),
                  ),
                  OutlinedButton.icon(
                    onPressed: widget.ble.isConnected ? _joinGroupByCodeDialog : null,
                    icon: const Icon(Icons.vpn_key_outlined, size: 16),
                    label: Text(l.tr('group_join_by_code')),
                  ),
                  TextButton.icon(
                    onPressed: () => _onRightMenuAction('contacts_hub'),
                    icon: const Icon(Icons.admin_panel_settings_outlined, size: 16),
                    label: Text(l.tr('contacts_groups_title')),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }

  void _toggleSearch() {
    setState(() {
      _searchMode = !_searchMode;
      if (!_searchMode) {
        _searchFocusNode.unfocus();
        _searchController.clear();
      } else {
        _searchFocusNode.requestFocus();
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final p = context.palette;
    final tabMeta = _tabMeta(l);
    final fabConfig = _fabConfigForTab(l);
    final tabChats = _activeTab == _ChatsTab.groups ? _groupTabItemsFull() : _tabItems();
    final reachableNeighbors = _activeTab == _ChatsTab.neighbors ? _reachableNeighbors() : const <_ReachableNodeItem>[];
    final allChatsForDrawer = _sortConversations([..._visible, ..._archived]);
    final pinnedForDrawer = allChatsForDrawer.where((c) => c.pinned && !c.archived).toList();
    final regularForDrawer = allChatsForDrawer.where((c) => !(c.pinned && !c.archived)).toList();

    return PopScope(
      canPop: true,
      onPopInvokedWithResult: (didPop, _) async {
        if (didPop) return;
        if (Navigator.of(context).canPop()) {
          Navigator.of(context).pop();
          return;
        }
        await AppLifecycleBridge.moveToBackground();
      },
      child: Scaffold(
        key: _scaffoldKey,
        backgroundColor: p.surface,
        drawer: Drawer(
        backgroundColor: p.surface,
        child: SafeArea(
          child: Column(
            children: [
              Padding(
                padding: const EdgeInsets.fromLTRB(AppSpacing.md, AppSpacing.md, AppSpacing.md, AppSpacing.sm),
                child: Row(
                  children: [
                    Icon(Icons.forum_outlined, color: p.primary, size: 20),
                    const SizedBox(width: 8),
                    Expanded(
                      child: Text(
                        l.tr('chats_list_title'),
                        style: TextStyle(
                          color: p.onSurface,
                          fontWeight: FontWeight.w700,
                          fontSize: 17,
                        ),
                      ),
                    ),
                    Container(
                      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                      decoration: BoxDecoration(
                        color: p.primary.withOpacity(0.20),
                        borderRadius: BorderRadius.circular(999),
                      ),
                      child: Text(
                        '${allChatsForDrawer.length}',
                        style: TextStyle(
                          color: p.primary,
                          fontWeight: FontWeight.w700,
                          fontSize: 12,
                        ),
                      ),
                    ),
                  ],
                ),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(AppSpacing.sm, 0, AppSpacing.sm, AppSpacing.sm),
                child: Column(
                  children: [
                    _DrawerNavItem(
                      icon: Icons.chat_bubble_outline_rounded,
                      label: l.tr('chats_folder_all'),
                      selected: _activeTab == _ChatsTab.all,
                      onTap: () {
                        setState(() => _activeTab = _ChatsTab.all);
                        Navigator.pop(context);
                      },
                    ),
                    _DrawerNavItem(
                      icon: Icons.person_outline_rounded,
                      label: l.tr('chats_folder_personal'),
                      selected: _activeTab == _ChatsTab.personal,
                      onTap: () {
                        setState(() => _activeTab = _ChatsTab.personal);
                        Navigator.pop(context);
                      },
                    ),
                    _DrawerNavItem(
                      icon: Icons.group_outlined,
                      label: l.tr('chats_folder_groups'),
                      selected: _activeTab == _ChatsTab.groups,
                      onTap: () {
                        setState(() => _activeTab = _ChatsTab.groups);
                        Navigator.pop(context);
                      },
                    ),
                    _DrawerNavItem(
                      icon: Icons.radar_rounded,
                      label: l.tr('neighbors'),
                      selected: _activeTab == _ChatsTab.neighbors,
                      onTap: () {
                        setState(() => _activeTab = _ChatsTab.neighbors);
                        Navigator.pop(context);
                      },
                    ),
                    _DrawerNavItem(
                      icon: Icons.archive_outlined,
                      label: l.tr('chats_folder_archived'),
                      selected: _activeTab == _ChatsTab.archived,
                      onTap: () {
                        setState(() => _activeTab = _ChatsTab.archived);
                        Navigator.pop(context);
                      },
                    ),
                  ],
                ),
              ),
              Divider(height: 1, color: p.divider),
              Expanded(
                child: ListView(
                  padding: const EdgeInsets.fromLTRB(AppSpacing.md, AppSpacing.xs, AppSpacing.md, AppSpacing.xl),
                  children: [
                    if (pinnedForDrawer.isNotEmpty) ...[
                      _DrawerSectionLabel(
                        label: l.tr('chats_action_pin'),
                        icon: Icons.push_pin_rounded,
                      ),
                      ...pinnedForDrawer.map((c) => _DrawerConversationTile(
                            conversation: c,
                            title: _titleForConversation(c),
                            selected: _activeConversationId == c.id,
                            onTap: () {
                              Navigator.pop(context);
                              _openConversation(c);
                            },
                          )),
                      const SizedBox(height: AppSpacing.sm),
                    ],
                    _DrawerSectionLabel(
                      label: l.tr('chats_folder_recent'),
                      icon: Icons.history_rounded,
                    ),
                    ...regularForDrawer.map((c) => _DrawerConversationTile(
                          conversation: c,
                          title: _titleForConversation(c),
                          selected: _activeConversationId == c.id,
                          onTap: () {
                            Navigator.pop(context);
                            _openConversation(c);
                          },
                        )),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
      endDrawer: Drawer(
        backgroundColor: p.surface,
        child: SafeArea(
          child: ListView(
            padding: const EdgeInsets.fromLTRB(AppSpacing.md, AppSpacing.md, AppSpacing.md, AppSpacing.xl),
            children: [
              Row(
                children: [
                  Icon(Icons.tune_rounded, color: p.primary, size: 20),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      l.tr('menu_title'),
                      style: TextStyle(
                        color: p.onSurface,
                        fontWeight: FontWeight.w700,
                        fontSize: 17,
                      ),
                    ),
                  ),
                ],
              ),
              const SizedBox(height: AppSpacing.md),
              ListTile(
                dense: true,
                minTileHeight: _kMenuItemMinHeight,
                visualDensity: _kMenuItemDensity,
                contentPadding: _kMenuItemPadding,
                leading: const Icon(Icons.contact_mail_outlined, size: 18),
                title: Text(
                  l.tr('contacts_groups_title'),
                  style: const TextStyle(
                    fontSize: _kMenuItemTitleSize,
                    fontWeight: _kMenuItemTitleWeight,
                    height: _kMenuItemLineHeight,
                  ),
                ),
                onTap: () async {
                  Navigator.pop(context);
                  await _onRightMenuAction('contacts_hub');
                },
              ),
              ListTile(
                dense: true,
                minTileHeight: _kMenuItemMinHeight,
                visualDensity: _kMenuItemDensity,
                contentPadding: _kMenuItemPadding,
                leading: const Icon(Icons.developer_board_rounded, size: 18),
                title: Text(
                  l.tr('chat_menu_node_status'),
                  style: const TextStyle(
                    fontSize: _kMenuItemTitleSize,
                    fontWeight: _kMenuItemTitleWeight,
                    height: _kMenuItemLineHeight,
                  ),
                ),
                onTap: () async {
                  Navigator.pop(context);
                  await _onRightMenuAction('node_status');
                },
              ),
              ListTile(
                dense: true,
                minTileHeight: _kMenuItemMinHeight,
                visualDensity: _kMenuItemDensity,
                contentPadding: _kMenuItemPadding,
                leading: const Icon(Icons.settings_rounded, size: 18),
                title: Text(
                  l.tr('settings'),
                  style: const TextStyle(
                    fontSize: _kMenuItemTitleSize,
                    fontWeight: _kMenuItemTitleWeight,
                    height: _kMenuItemLineHeight,
                  ),
                ),
                onTap: () async {
                  Navigator.pop(context);
                  await _onRightMenuAction('settings');
                },
              ),
              ExpansionTile(
                initiallyExpanded: _rightToolsExpanded,
                onExpansionChanged: (v) => setState(() => _rightToolsExpanded = v),
                tilePadding: const EdgeInsets.symmetric(horizontal: 10),
                childrenPadding: const EdgeInsets.only(left: 12, right: 8, bottom: 6),
                leading: const Icon(Icons.build_rounded, size: 18),
                title: Text(
                  l.tr('tools'),
                  style: const TextStyle(
                    fontSize: _kMenuItemTitleSize,
                    fontWeight: _kMenuItemTitleWeight,
                    height: _kMenuItemLineHeight,
                  ),
                ),
                children: [
                  _RightDrawerActionTile(
                    icon: Icons.map_rounded,
                    label: l.tr('map'),
                    onTap: () async {
                      Navigator.pop(context);
                      await _onToolAction('map');
                    },
                  ),
                  _RightDrawerActionTile(
                    icon: Icons.hub_rounded,
                    label: l.tr('mesh_topology'),
                    onTap: () async {
                      Navigator.pop(context);
                      await _onToolAction('mesh');
                    },
                  ),
                  _RightDrawerActionTile(
                    icon: Icons.radar_rounded,
                    label: l.tr('ping'),
                    onTap: () async {
                      Navigator.pop(context);
                      await _onToolAction('ping');
                    },
                  ),
                  _RightDrawerActionTile(
                    icon: Icons.health_and_safety_rounded,
                    label: l.tr('selftest'),
                    onTap: () async {
                      Navigator.pop(context);
                      await _onToolAction('selftest');
                    },
                  ),
                ],
              ),
              const SizedBox(height: 4),
              ListTile(
                dense: true,
                minTileHeight: _kMenuItemMinHeight,
                visualDensity: _kMenuItemDensity,
                contentPadding: _kMenuItemPadding,
                leading: Icon(Icons.link_off_rounded, size: 18, color: p.error),
                title: Text(
                  l.tr('disconnect'),
                  style: TextStyle(
                    fontSize: _kMenuItemTitleSize,
                    fontWeight: _kMenuItemTitleWeight,
                    height: _kMenuItemLineHeight,
                    color: p.error,
                  ),
                ),
                onTap: () async {
                  Navigator.pop(context);
                  await _onRightMenuAction('disconnect');
                },
              ),
            ],
          ),
        ),
      ),
      appBar: AppBar(
        automaticallyImplyLeading: false,
        toolbarHeight: 52,
        titleSpacing: 12,
        scrolledUnderElevation: 0,
        elevation: 0,
        backgroundColor: p.surface,
        surfaceTintColor: Colors.transparent,
        title: _searchMode
            ? Container(
                height: 39,
                padding: const EdgeInsets.symmetric(horizontal: 12),
                decoration: BoxDecoration(
                  color: p.card.withOpacity(0.36),
                  borderRadius: BorderRadius.circular(11),
                  border: Border.all(
                    color: _searchFocusNode.hasFocus ? p.primary.withOpacity(0.80) : p.divider.withOpacity(0.55),
                    width: _searchFocusNode.hasFocus ? 1.2 : 1.0,
                  ),
                ),
                child: TextField(
                  controller: _searchController,
                  focusNode: _searchFocusNode,
                  autofocus: true,
                  style: TextStyle(color: p.onSurface, fontSize: 15.5),
                  decoration: InputDecoration(
                    hintText: l.tr('search_chats'),
                    border: InputBorder.none,
                    enabledBorder: InputBorder.none,
                    focusedBorder: InputBorder.none,
                    disabledBorder: InputBorder.none,
                    errorBorder: InputBorder.none,
                    focusedErrorBorder: InputBorder.none,
                    isCollapsed: true,
                    contentPadding: const EdgeInsets.symmetric(vertical: 8.5),
                  ),
                ),
              )
            : InkWell(
                borderRadius: BorderRadius.circular(10),
                onTap: () => _scaffoldKey.currentState?.openDrawer(),
                child: Padding(
                  padding: const EdgeInsets.symmetric(vertical: 3, horizontal: 2),
                  child: Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Icon(tabMeta.icon, size: 21, color: p.primary),
                      const SizedBox(width: 10),
                      Text(
                        tabMeta.title,
                        style: TextStyle(
                          color: p.onSurface,
                          fontSize: 17,
                          fontWeight: FontWeight.w700,
                          letterSpacing: 0.1,
                        ),
                      ),
                      const SizedBox(width: 2),
                      Icon(Icons.expand_more_rounded, size: 21, color: p.onSurfaceVariant),
                    ],
                  ),
                ),
              ),
        actions: [
          Padding(
            padding: const EdgeInsets.only(right: 2),
            child: IconButton(
              visualDensity: VisualDensity.compact,
              iconSize: 23,
              tooltip: l.tr('search_chats'),
              icon: Icon(_searchMode ? Icons.close : Icons.search_rounded),
              onPressed: _toggleSearch,
            ),
          ),
          Padding(
            padding: const EdgeInsets.only(right: 8),
            child: IconButton(
              visualDensity: VisualDensity.compact,
              iconSize: 23,
              tooltip: l.tr('menu_title'),
              icon: const Icon(Icons.tune_rounded),
              onPressed: _showTopMenu,
            ),
          ),
        ],
      ),
      body: MeshBackgroundWrapper(
        child: SafeArea(
          child: Column(
            children: [
              Expanded(
                child: RefreshIndicator(
                  onRefresh: _load,
                  child: ListView(
                    padding: const EdgeInsets.fromLTRB(AppSpacing.sm, AppSpacing.xs, AppSpacing.sm, AppSpacing.xxl),
                    children: [
                      if (_activeTab == _ChatsTab.neighbors && reachableNeighbors.isEmpty)
                        SizedBox(
                          height: MediaQuery.of(context).size.height * 0.56,
                          child: Center(
                            child: Column(
                              mainAxisSize: MainAxisSize.min,
                              children: [
                                Icon(
                                  Icons.radar_rounded,
                                  size: 52,
                                  color: p.onSurfaceVariant.withOpacity(0.38),
                                ),
                                const SizedBox(height: AppSpacing.sm),
                                Text(
                                  l.tr('chat_preview_empty'),
                                  textAlign: TextAlign.center,
                                  style: TextStyle(
                                    color: p.onSurfaceVariant.withOpacity(0.95),
                                    fontSize: 15,
                                    fontWeight: FontWeight.w500,
                                  ),
                                ),
                              ],
                            ),
                          ),
                        ),
                      if (_activeTab == _ChatsTab.groups && tabChats.isEmpty)
                        _buildGroupsEmptyState(l, p),
                      if (_activeTab != _ChatsTab.neighbors && _activeTab != _ChatsTab.groups && tabChats.isEmpty)
                        SizedBox(
                          height: MediaQuery.of(context).size.height * 0.56,
                          child: Center(
                            child: Transform.translate(
                              offset: const Offset(0, 18),
                              child: Column(
                                mainAxisSize: MainAxisSize.min,
                                children: [
                                  Icon(
                                    Icons.chat_outlined,
                                    size: 56,
                                    color: p.onSurfaceVariant.withOpacity(0.38),
                                  ),
                                  const SizedBox(height: AppSpacing.sm),
                                  Text(
                                    l.tr('chat_preview_empty'),
                                    textAlign: TextAlign.center,
                                    style: TextStyle(
                                      color: p.onSurfaceVariant.withOpacity(0.95),
                                      fontSize: 15,
                                      fontWeight: FontWeight.w500,
                                    ),
                                  ),
                                ],
                              ),
                            ),
                          ),
                        ),
                      if (_activeTab != _ChatsTab.archived && _activeTab != _ChatsTab.neighbors && _archived.isNotEmpty)
                        _ArchiveEntryTile(
                          archivedCount: _archived.length,
                          onTap: () => appPush(
                            context,
                            _ArchivedChatsScreen(
                              ble: widget.ble,
                              chats: _archived,
                              onOpen: _openConversation,
                              onPin: _togglePin,
                              onArchive: _toggleArchive,
                              onMute: _toggleMute,
                              onFolder: _setFolder,
                              onDelete: _deleteConversation,
                              titleResolver: _titleForConversation,
                            ),
                          ),
                        ),
                      if (_activeTab == _ChatsTab.neighbors)
                        ...reachableNeighbors.map(_buildReachableNodeTile),
                      if (_activeTab != _ChatsTab.neighbors)
                        ...tabChats.map((c) => _ConversationTile(
                            conversation: c,
                            titleOverride: _titleForConversation(c),
                            timeText: _formatTime(c.lastMessageAtMs),
                            groupRole: c.kind == ConversationKind.group
                                ? (() {
                                    final gid = _groupIdFromPeerRef(c.peerRef);
                                    return gid == null ? null : _groupRoleById(gid);
                                  })()
                                : null,
                            groupKeyActual: c.kind == ConversationKind.group
                                ? (() {
                                    final gid = _groupIdFromPeerRef(c.peerRef);
                                    return gid == null ? null : _groupKeyActualById(gid);
                                  })()
                                : null,
                            onTap: () => _openConversation(c),
                            onPin: () => _togglePin(c),
                            onArchive: () => _toggleArchive(c),
                            onMute: () => _toggleMute(c),
                            onFolder: (folderId) => _setFolder(c, folderId),
                            onDelete: () => _deleteConversation(c),
                            onCopyInvite: () => _copyGroupInviteForConversation(c),
                            canCopyInvite: c.kind == ConversationKind.group &&
                                (_groupIdFromPeerRef(c.peerRef) != null),
                            canDelete: c.kind != ConversationKind.broadcast,
                          )),
                    ],
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
        floatingActionButton: fabConfig == null
            ? null
            : FloatingActionButton(
                tooltip: fabConfig.tooltip,
                backgroundColor: p.card.withOpacity(0.92),
                foregroundColor: p.onSurface,
                shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
                elevation: 1,
                hoverElevation: 2,
                highlightElevation: 2,
                onPressed: fabConfig.onPressed,
                child: Icon(fabConfig.icon, size: 22),
              ),
      ),
    );
  }
}

class _ConversationTile extends StatelessWidget {
  final ChatConversation conversation;
  final String? titleOverride;
  final String timeText;
  final String? groupRole;
  final bool? groupKeyActual;
  final VoidCallback onTap;
  final VoidCallback onPin;
  final VoidCallback onArchive;
  final VoidCallback onMute;
  final ValueChanged<String> onFolder;
  final VoidCallback onDelete;
  final VoidCallback? onCopyInvite;
  final bool canCopyInvite;
  final bool canDelete;

  const _ConversationTile({
    required this.conversation,
    this.titleOverride,
    required this.timeText,
    this.groupRole,
    this.groupKeyActual,
    required this.onTap,
    required this.onPin,
    required this.onArchive,
    required this.onMute,
    required this.onFolder,
    required this.onDelete,
    this.onCopyInvite,
    this.canCopyInvite = false,
    this.canDelete = true,
  });

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final p = context.palette;
    final hasUnread = conversation.unreadCount > 0;
    final kindIcon = switch (conversation.kind) {
      ConversationKind.direct => Icons.person_rounded,
      ConversationKind.group => Icons.group_rounded,
      ConversationKind.broadcast => Icons.campaign_rounded,
    };
    final kindTint = switch (conversation.kind) {
      ConversationKind.direct => p.primary,
      ConversationKind.group => p.success,
      ConversationKind.broadcast => p.onSurfaceVariant,
    };
    final v2RoleText = switch (groupRole) {
      'owner' => l.tr('group_role_owner'),
      'admin' => l.tr('group_role_admin'),
      'member' => l.tr('group_role_member'),
      _ => null,
    };
    final v2KeyText = groupKeyActual == null
        ? null
        : (groupKeyActual! ? l.tr('group_key_actual') : l.tr('group_key_rekey_required_short'));
    final statusText = [
      if (v2RoleText != null) v2RoleText,
      if (v2KeyText != null) v2KeyText,
    ].join(' · ');
    final subtitleText = (conversation.lastMessagePreview ?? '').trim().isNotEmpty
        ? (conversation.lastMessagePreview ?? '')
        : statusText;
    return AppSectionCard(
      margin: const EdgeInsets.only(bottom: AppSpacing.sm),
      padding: EdgeInsets.zero,
      child: ClipRRect(
        borderRadius: BorderRadius.circular(AppSpacing.md),
        child: Stack(
          children: [
            Container(
              decoration: BoxDecoration(
                borderRadius: BorderRadius.circular(AppSpacing.md),
                border: Border.all(
                  color: hasUnread ? kindTint.withOpacity(0.28) : p.divider.withOpacity(0.72),
                ),
                gradient: LinearGradient(
                  begin: Alignment.centerLeft,
                  end: Alignment.centerRight,
                  colors: [
                    hasUnread ? kindTint.withOpacity(0.08) : p.card.withOpacity(0.92),
                    p.card.withOpacity(0.88),
                  ],
                ),
                boxShadow: [
                  BoxShadow(
                    color: Colors.black.withOpacity(0.10),
                    blurRadius: 8,
                    offset: const Offset(0, 3),
                  ),
                  if (hasUnread)
                    BoxShadow(
                      color: kindTint.withOpacity(0.10),
                      blurRadius: 12,
                      offset: const Offset(0, 0),
                    ),
                ],
              ),
            ),
            Positioned(
              left: 0,
              top: 6,
              bottom: 6,
              child: Container(
                width: 2,
                decoration: BoxDecoration(
                  color: kindTint.withOpacity(hasUnread ? 0.82 : 0.58),
                  borderRadius: BorderRadius.circular(999),
                ),
              ),
            ),
            ListTile(
          onTap: onTap,
          onLongPress: () => _showActions(context),
          dense: true,
          visualDensity: const VisualDensity(horizontal: 0, vertical: -2),
          minVerticalPadding: 2,
          contentPadding: const EdgeInsets.symmetric(horizontal: AppSpacing.md, vertical: 1),
          leading: Container(
            width: 34,
            height: 34,
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(11),
              color: kindTint.withOpacity(0.14),
              border: Border.all(
                color: kindTint.withOpacity(0.28),
              ),
            ),
            child: Icon(kindIcon, color: kindTint, size: 18),
          ),
          title: Row(
            children: [
              Expanded(
                child: Text(
                  titleOverride ?? conversation.title,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    color: p.onSurface,
                    fontWeight: hasUnread ? FontWeight.w700 : FontWeight.w600,
                    letterSpacing: 0.1,
                  ),
                ),
              ),
              if (groupRole != null) ...[
                Icon(
                  groupRole == 'owner'
                      ? Icons.workspace_premium_rounded
                      : (groupRole == 'admin' ? Icons.admin_panel_settings_rounded : Icons.shield_outlined),
                  size: 14,
                  color: (groupRole == 'owner' || groupRole == 'admin') ? p.primary : p.onSurfaceVariant,
                ),
                const SizedBox(width: 6),
              ],
            ],
          ),
          subtitle: Text(
            subtitleText,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: TextStyle(
              color: hasUnread ? p.onSurfaceVariant.withOpacity(0.98) : p.onSurfaceVariant.withOpacity(0.92),
              fontWeight: hasUnread ? FontWeight.w500 : FontWeight.w400,
              fontSize: 13,
            ),
          ),
          trailing: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              if (timeText.isNotEmpty)
                Text(
                  timeText,
                  style: TextStyle(
                    color: hasUnread ? kindTint.withOpacity(0.95) : p.onSurfaceVariant,
                    fontSize: 10.5,
                    fontWeight: hasUnread ? FontWeight.w600 : FontWeight.w500,
                  ),
                ),
              if (timeText.isNotEmpty &&
                  (conversation.pinned || conversation.muted || hasUnread))
                const SizedBox(width: 7),
              if (conversation.pinned)
                Icon(Icons.push_pin_rounded, size: 15, color: p.primary.withOpacity(0.9)),
              if (conversation.muted) const SizedBox(width: 4),
              if (conversation.muted)
                Icon(Icons.notifications_off_rounded, size: 15, color: p.onSurfaceVariant),
              if (hasUnread) const SizedBox(width: 7),
              if (hasUnread)
                Container(
                  constraints: const BoxConstraints(minWidth: 22),
                  padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 4),
                  decoration: BoxDecoration(
                    color: kindTint.withOpacity(0.20),
                    border: Border.all(color: kindTint.withOpacity(0.36)),
                    borderRadius: BorderRadius.circular(999),
                  ),
                  child: Text(
                    '${conversation.unreadCount}',
                    textAlign: TextAlign.center,
                    style: TextStyle(
                      color: kindTint.withOpacity(0.98),
                      fontWeight: FontWeight.w700,
                      fontSize: 11,
                    ),
                  ),
                ),
            ],
          ),
            ),
          ],
        ),
      ),
    );
  }

  Future<void> _showActions(BuildContext context) async {
    final selected = await showModalBottomSheet<String>(
      context: context,
      builder: (ctx) {
        return SafeArea(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              ListTile(
                leading: Icon(conversation.pinned ? Icons.push_pin_outlined : Icons.push_pin_rounded),
                title: Text(conversation.pinned ? context.l10n.tr('chats_action_unpin') : context.l10n.tr('chats_action_pin')),
                onTap: () => Navigator.pop(ctx, 'pin'),
              ),
              ListTile(
                leading: Icon(conversation.muted ? Icons.notifications_active_rounded : Icons.notifications_off_rounded),
                title: Text(conversation.muted ? context.l10n.tr('chats_action_unmute') : context.l10n.tr('chats_action_mute')),
                onTap: () => Navigator.pop(ctx, 'mute'),
              ),
              ListTile(
                leading: Icon(conversation.archived ? Icons.unarchive_rounded : Icons.archive_rounded),
                title: Text(conversation.archived ? context.l10n.tr('chats_action_unarchive') : context.l10n.tr('chats_action_archive')),
                onTap: () => Navigator.pop(ctx, 'archive'),
              ),
              if (conversation.kind != ConversationKind.group)
                ListTile(
                  leading: const Icon(Icons.folder_rounded),
                  title: Text(context.l10n.tr('chats_action_to_personal')),
                  onTap: () => Navigator.pop(ctx, 'folder_personal'),
                ),
              if (conversation.kind != ConversationKind.group &&
                  conversation.kind != ConversationKind.direct)
                ListTile(
                  leading: const Icon(Icons.folder_rounded),
                  title: Text(context.l10n.tr('chats_action_to_groups')),
                  onTap: () => Navigator.pop(ctx, 'folder_groups'),
                ),
              if (canCopyInvite)
                ListTile(
                  leading: const Icon(Icons.key_rounded),
                  title: Text(context.l10n.tr('group_copy_invite')),
                  onTap: () => Navigator.pop(ctx, 'copy_invite'),
                ),
              if (canDelete)
                ListTile(
                  leading: Icon(Icons.delete_outline_rounded, color: context.palette.error),
                  title: Text(
                    context.l10n.tr('chats_action_delete'),
                    style: TextStyle(color: context.palette.error),
                  ),
                  onTap: () => Navigator.pop(ctx, 'delete'),
                ),
            ],
          ),
        );
      },
    );
    switch (selected) {
      case 'pin':
        onPin();
        break;
      case 'mute':
        onMute();
        break;
      case 'archive':
        onArchive();
        break;
      case 'folder_personal':
        onFolder('personal');
        break;
      case 'folder_groups':
        onFolder('groups');
        break;
      case 'delete':
        onDelete();
        break;
      case 'copy_invite':
        onCopyInvite?.call();
        break;
      default:
        break;
    }
  }
}

class _ArchiveEntryTile extends StatelessWidget {
  final int archivedCount;
  final VoidCallback onTap;

  const _ArchiveEntryTile({required this.archivedCount, required this.onTap});

  @override
  Widget build(BuildContext context) {
    final p = context.palette;
    return AppSectionCard(
      margin: const EdgeInsets.only(bottom: AppSpacing.sm),
      padding: EdgeInsets.zero,
      child: ListTile(
        onTap: onTap,
        dense: true,
        contentPadding: const EdgeInsets.symmetric(horizontal: AppSpacing.md),
        leading: Icon(Icons.archive_outlined, color: p.onSurfaceVariant),
        title: Text(context.l10n.tr('open_archived'), style: TextStyle(color: p.onSurface)),
        trailing: Container(
          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
          decoration: BoxDecoration(
            color: p.surfaceVariant,
            borderRadius: BorderRadius.circular(10),
          ),
          child: Text('$archivedCount', style: TextStyle(color: p.onSurfaceVariant, fontWeight: FontWeight.w700)),
        ),
      ),
    );
  }
}

class _DrawerSectionLabel extends StatelessWidget {
  final String label;
  final IconData? icon;

  const _DrawerSectionLabel({required this.label, this.icon});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(2, AppSpacing.xs, 2, AppSpacing.xs),
      child: Row(
        children: [
          if (icon != null) ...[
            Icon(icon, size: 14, color: context.palette.onSurfaceVariant),
            const SizedBox(width: 6),
          ],
          Text(
            label,
            style: TextStyle(
              color: context.palette.onSurfaceVariant,
              fontWeight: FontWeight.w600,
              fontSize: 12,
              letterSpacing: 0.25,
            ),
          ),
        ],
      ),
    );
  }
}

class _DrawerNavItem extends StatelessWidget {
  final IconData icon;
  final String label;
  final bool selected;
  final VoidCallback onTap;

  const _DrawerNavItem({
    required this.icon,
    required this.label,
    required this.selected,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    final p = context.palette;
    return Padding(
      padding: const EdgeInsets.only(bottom: 6),
      child: Material(
        color: selected ? p.primary.withOpacity(0.16) : Colors.transparent,
        borderRadius: BorderRadius.circular(10),
        child: InkWell(
          borderRadius: BorderRadius.circular(10),
          splashColor: p.primary.withOpacity(0.14),
          highlightColor: p.primary.withOpacity(0.08),
          onTap: onTap,
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 9),
            child: Row(
              children: [
                Icon(icon, size: _kMenuItemIconSize, color: selected ? p.primary : p.onSurfaceVariant),
                const SizedBox(width: 10),
                Expanded(
                  child: Text(
                    label,
                    style: TextStyle(
                      color: p.onSurface,
                      fontWeight: selected ? FontWeight.w700 : _kMenuItemTitleWeight,
                      fontSize: _kMenuItemTitleSize,
                      height: _kMenuItemLineHeight,
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _DrawerConversationTile extends StatelessWidget {
  final ChatConversation conversation;
  final String title;
  final bool selected;
  final VoidCallback onTap;

  const _DrawerConversationTile({
    required this.conversation,
    required this.title,
    required this.selected,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    final p = context.palette;
    return Padding(
      padding: const EdgeInsets.only(bottom: 5),
      child: Material(
        color: selected ? p.primary.withOpacity(0.14) : p.card.withOpacity(0.50),
        borderRadius: BorderRadius.circular(12),
        child: ListTile(
          dense: true,
          visualDensity: const VisualDensity(horizontal: 0, vertical: -1),
          contentPadding: const EdgeInsets.symmetric(horizontal: 10, vertical: 1),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(12),
            side: BorderSide(color: selected ? p.primary.withOpacity(0.44) : p.divider.withOpacity(0.78)),
          ),
          leading: Icon(
            switch (conversation.kind) {
              ConversationKind.direct => Icons.person_rounded,
              ConversationKind.group => Icons.group_rounded,
              ConversationKind.broadcast => Icons.campaign_rounded,
            },
            color: selected ? p.primary : p.onSurfaceVariant,
            size: 18,
          ),
          title: Text(
            title,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: TextStyle(
              color: selected ? p.onSurface : null,
              fontWeight: selected ? FontWeight.w700 : FontWeight.w500,
              fontSize: 14,
            ),
          ),
          subtitle: conversation.lastMessagePreview == null || conversation.lastMessagePreview!.isEmpty
              ? null
              : Text(
                  conversation.lastMessagePreview!,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(color: p.onSurfaceVariant, fontSize: 12.5),
                ),
          trailing: conversation.unreadCount > 0
              ? Container(
                  padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                  decoration: BoxDecoration(
                    color: p.primary.withOpacity(0.15),
                    borderRadius: BorderRadius.circular(999),
                  ),
                  child: Text(
                    '${conversation.unreadCount}',
                    style: TextStyle(
                      fontSize: 12,
                      fontWeight: FontWeight.w600,
                      color: p.primary.withOpacity(0.92),
                    ),
                  ),
                )
              : null,
          onTap: onTap,
        ),
      ),
    );
  }
}

class _RightDrawerActionTile extends StatelessWidget {
  final IconData icon;
  final String label;
  final VoidCallback onTap;

  const _RightDrawerActionTile({
    required this.icon,
    required this.label,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    return ListTile(
      dense: true,
      minTileHeight: _kMenuItemMinHeight,
      visualDensity: _kMenuItemDensity,
      contentPadding: _kMenuItemPadding,
      leading: Icon(icon, size: _kMenuItemIconSize),
      title: Text(
        label,
        style: const TextStyle(
          fontSize: _kMenuItemTitleSize,
          fontWeight: _kMenuItemTitleWeight,
          height: _kMenuItemLineHeight,
        ),
      ),
      onTap: onTap,
    );
  }
}

class _ReachableNodeItem {
  final String nodeId;
  final String? nickname;
  final bool isDirect;
  final int? rssi;
  final int? hops;
  final bool hasKey;

  const _ReachableNodeItem({
    required this.nodeId,
    required this.nickname,
    required this.isDirect,
    required this.rssi,
    required this.hops,
    required this.hasKey,
  });

  _ReachableNodeItem copyWith({
    String? nodeId,
    String? nickname,
    bool? isDirect,
    int? rssi,
    int? hops,
    bool? hasKey,
  }) {
    return _ReachableNodeItem(
      nodeId: nodeId ?? this.nodeId,
      nickname: nickname ?? this.nickname,
      isDirect: isDirect ?? this.isDirect,
      rssi: rssi ?? this.rssi,
      hops: hops ?? this.hops,
      hasKey: hasKey ?? this.hasKey,
    );
  }
}

class _ArchivedChatsScreen extends StatelessWidget {
  final RiftLinkBle ble;
  final List<ChatConversation> chats;
  final Future<void> Function(ChatConversation) onOpen;
  final Future<void> Function(ChatConversation) onPin;
  final Future<void> Function(ChatConversation) onArchive;
  final Future<void> Function(ChatConversation) onMute;
  final Future<void> Function(ChatConversation, String) onFolder;
  final Future<void> Function(ChatConversation) onDelete;
  final String Function(ChatConversation)? titleResolver;

  const _ArchivedChatsScreen({
    required this.ble,
    required this.chats,
    required this.onOpen,
    required this.onPin,
    required this.onArchive,
    required this.onMute,
    required this.onFolder,
    required this.onDelete,
    this.titleResolver,
  });

  String _formatTime(int? ms) {
    if (ms == null) return '';
    final d = DateTime.fromMillisecondsSinceEpoch(ms);
    return '${d.hour.toString().padLeft(2, '0')}:${d.minute.toString().padLeft(2, '0')}';
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: riftAppBar(context, showBack: true, title: context.l10n.tr('chats_folder_archived')),
      body: MeshBackgroundWrapper(
        child: ListView(
          padding: const EdgeInsets.fromLTRB(AppSpacing.sm, AppSpacing.sm, AppSpacing.sm, AppSpacing.xxl),
          children: chats
              .map((c) => _ConversationTile(
                    conversation: c,
                    titleOverride: titleResolver?.call(c),
                    timeText: _formatTime(c.lastMessageAtMs),
                    onTap: () => onOpen(c),
                    onPin: () => onPin(c),
                    onArchive: () => onArchive(c),
                    onMute: () => onMute(c),
                    onFolder: (folderId) => onFolder(c, folderId),
                    onDelete: () => onDelete(c),
                    canDelete: c.kind != ConversationKind.broadcast,
                  ))
              .toList(),
        ),
      ),
    );
  }
}

