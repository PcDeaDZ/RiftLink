import 'dart:async';
import 'dart:convert';
import 'dart:math';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app_navigator.dart';
import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../mesh_constants.dart';
import '../theme/app_theme.dart';
import '../widgets/mesh_background.dart';
import '../widgets/app_snackbar.dart';
import '../widgets/rift_dialogs.dart';

class GroupsScreen extends StatefulWidget {
  final RiftLinkBle ble;
  final List<int> initialGroups;
  /// Без AppBar — для вкладки в [ContactsGroupsHubScreen].
  final bool embedded;

  const GroupsScreen({
    super.key,
    required this.ble,
    required this.initialGroups,
    this.embedded = false,
  });

  @override
  State<GroupsScreen> createState() => _GroupsScreenState();
}

class _GroupsScreenState extends State<GroupsScreen> {
  List<int> _groups = [];
  final Map<int, bool> _groupPrivate = <int, bool>{};
  bool _loading = false;
  StreamSubscription<RiftLinkEvent>? _sub;

  List<int> _normalizeGroups(Iterable<int> groups) {
    final out = groups.where((g) => g != kMeshBroadcastGroupId && g > 0).toSet().toList();
    out.sort();
    return out;
  }

  void _applyGroups(Iterable<int> groups, [List<bool>? privateFlags]) {
    final normalized = _normalizeGroups(groups);
    final nextPriv = <int, bool>{};
    for (var i = 0; i < normalized.length; i++) {
      final gid = normalized[i];
      bool isPriv = _groupPrivate[gid] ?? false;
      if (privateFlags != null && i < privateFlags.length) isPriv = privateFlags[i];
      nextPriv[gid] = isPriv;
    }
    _groups = normalized;
    _groupPrivate
      ..clear()
      ..addAll(nextPriv);
  }

  @override
  void initState() {
    super.initState();
    _applyGroups(widget.initialGroups);
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      // Ответ на getGroups — и поле groups в evt:info (как в чате), иначе список не обновлялся без отдельного notify.
      if (evt is RiftLinkGroupsEvent) {
        setState(() => _applyGroups(evt.groups, evt.groupsPrivate));
      } else if (evt is RiftLinkInfoEvent) {
        setState(() => _applyGroups(evt.groups, evt.groupsPrivate));
      }
    });
    _refresh();
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }

  Future<void> _refresh() async {
    if (!widget.ble.isConnected) return;
    setState(() => _loading = true);
    await widget.ble.getGroups();
    if (mounted) setState(() => _loading = false);
  }

  String _generateGroupKeyB64() {
    final rnd = Random.secure();
    final bytes = List<int>.generate(32, (_) => rnd.nextInt(256));
    return base64Encode(bytes);
  }

  Future<void> _setGroupPrivate(int gid, bool privateMode) async {
    final l = context.l10n;
    if (!widget.ble.isConnected) return;
    if (privateMode) {
      final keyB64 = _generateGroupKeyB64();
      final ok = await widget.ble.setGroupKey(gid, keyB64);
      if (ok && mounted) {
        _snack(l.tr('group_set_private', {'id': '$gid'}));
        await Clipboard.setData(ClipboardData(text: jsonEncode({'group': gid, 'groupKey': keyB64})));
        _snack(l.tr('group_invite_copied', {'id': '$gid'}));
      } else if (mounted) {
        _snack(l.tr('error'), backgroundColor: context.palette.error);
      }
    } else {
      final ok = await widget.ble.clearGroupKey(gid);
      if (ok && mounted) {
        _snack(l.tr('group_set_public', {'id': '$gid'}));
      } else if (mounted) {
        _snack(l.tr('error'), backgroundColor: context.palette.error);
      }
    }
    await _refresh();
  }

  Future<void> _rotatePrivateKey(int gid) async {
    final l = context.l10n;
    final keyB64 = _generateGroupKeyB64();
    final ok = await widget.ble.setGroupKey(gid, keyB64);
    if (ok && mounted) {
      await Clipboard.setData(ClipboardData(text: jsonEncode({'group': gid, 'groupKey': keyB64})));
      _snack(l.tr('group_key_rotated', {'id': '$gid'}));
      _snack(l.tr('group_invite_copied', {'id': '$gid'}));
      await _refresh();
    } else if (mounted) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
    }
  }

  Future<void> _copyInvite(int gid) async {
    final l = context.l10n;
    if (!widget.ble.isConnected) return;
    if (!await widget.ble.getGroupKey(gid)) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
      return;
    }
    try {
      final evt = await widget.ble.events
          .where((e) => e is RiftLinkGroupKeyEvent && (e as RiftLinkGroupKeyEvent).group == gid)
          .cast<RiftLinkGroupKeyEvent>()
          .first
          .timeout(const Duration(seconds: 3));
      await Clipboard.setData(ClipboardData(text: jsonEncode({'group': gid, 'groupKey': evt.key})));
      if (mounted) _snack(l.tr('group_invite_copied', {'id': '$gid'}));
    } catch (_) {
      if (mounted) _snack(l.tr('error'), backgroundColor: context.palette.error);
    }
  }

  Future<void> _pasteInviteAndJoin() async {
    final l = context.l10n;
    final data = await Clipboard.getData(Clipboard.kTextPlain);
    final text = data?.text?.trim();
    if (text == null || text.isEmpty) {
      _snack(l.tr('group_invite_bad'), backgroundColor: context.palette.error);
      return;
    }
    try {
      final m = jsonDecode(text);
      if (m is! Map) throw Exception('bad');
      final group = int.tryParse('${m['group'] ?? ''}') ?? 0;
      final key = (m['groupKey'] as String?)?.trim();
      if (group <= 1) throw Exception('bad');
      await widget.ble.addGroup(group);
      if (key != null && key.isNotEmpty) {
        await widget.ble.setGroupKey(group, key);
      }
      await _refresh();
      if (mounted) _snack(l.tr('group_invite_joined', {'id': '$group'}));
    } catch (_) {
      _snack(l.tr('group_invite_bad'), backgroundColor: context.palette.error);
    }
  }

  void _snack(String msg, {Color? backgroundColor}) {
    if (!mounted) return;
    showAppSnackBar(
      context,
      msg,
      kind: backgroundColor == context.palette.error ? AppSnackKind.error : AppSnackKind.neutral,
    );
  }

  void _showAddSheet() {
    if (!widget.ble.isConnected) return;
    final c = TextEditingController();
    final l = context.l10n;
    FocusScope.of(context).unfocus();
    HapticFeedback.mediumImpact();
    showAppModalBottomSheet<void>(
      context: context,
      backgroundColor: Colors.transparent,
      isScrollControlled: true,
      builder: (ctx) {
        final bottomInset = MediaQuery.of(ctx).viewInsets.bottom;
        return Padding(
          padding: EdgeInsets.only(bottom: bottomInset),
          child: Container(
            decoration: BoxDecoration(
              color: context.palette.card,
              borderRadius: const BorderRadius.vertical(top: Radius.circular(20)),
            ),
            child: SafeArea(
              top: false,
              child: SingleChildScrollView(
                child: Padding(
                  padding: const EdgeInsets.fromLTRB(16, 10, 16, 18),
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      Center(
                        child: Container(
                          width: 36,
                          height: 3,
                          decoration: BoxDecoration(
                            color: context.palette.onSurfaceVariant.withOpacity(0.3),
                            borderRadius: BorderRadius.circular(2),
                          ),
                        ),
                      ),
                      const SizedBox(height: 12),
                      Row(
                        children: [
                          Icon(Icons.group_add_rounded, color: context.palette.primary, size: 22),
                          const SizedBox(width: 10),
                          Expanded(
                            child: Text(
                              l.tr('add_group'),
                              style: TextStyle(
                                fontSize: 17,
                                fontWeight: FontWeight.w700,
                                color: context.palette.onSurface,
                              ),
                            ),
                          ),
                          IconButton(
                            icon: Icon(Icons.content_paste_rounded, color: context.palette.onSurfaceVariant.withOpacity(0.85)),
                            onPressed: () async {
                              Navigator.pop(ctx);
                              await _pasteInviteAndJoin();
                            },
                            visualDensity: VisualDensity.compact,
                            padding: EdgeInsets.zero,
                            constraints: const BoxConstraints(minWidth: 36, minHeight: 36),
                            tooltip: l.tr('paste'),
                          ),
                          IconButton(
                            icon: Icon(Icons.close_rounded, color: context.palette.onSurfaceVariant.withOpacity(0.85)),
                            onPressed: () => Navigator.pop(ctx),
                            visualDensity: VisualDensity.compact,
                            padding: EdgeInsets.zero,
                            constraints: const BoxConstraints(minWidth: 36, minHeight: 36),
                          ),
                        ],
                      ),
                      const SizedBox(height: 6),
                      Text(
                        l.tr('groups_add_sheet_hint'),
                        maxLines: 3,
                        overflow: TextOverflow.ellipsis,
                        style: TextStyle(
                          fontSize: 12,
                          height: 1.32,
                          color: context.palette.onSurfaceVariant.withOpacity(0.92),
                        ),
                      ),
                      const SizedBox(height: 12),
                      TextField(
                        controller: c,
                        keyboardType: TextInputType.number,
                        autofocus: true,
                        style: TextStyle(color: context.palette.onSurface, fontSize: 16),
                        decoration: InputDecoration(
                          labelText: l.tr('group_id_hint'),
                          hintText: '42',
                          filled: true,
                          fillColor: context.palette.surfaceVariant.withOpacity(0.55),
                          border: OutlineInputBorder(
                            borderRadius: BorderRadius.circular(12),
                            borderSide: BorderSide.none,
                          ),
                          contentPadding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
                        ),
                      ),
                      const SizedBox(height: 14),
                      FilledButton(
                        onPressed: () async {
                          final val = int.tryParse(c.text.trim());
                          if (val == null || val <= 0 || val > 0xFFFFFFFF) {
                            _snack(l.tr('invalid_group_id'), backgroundColor: context.palette.error);
                            return;
                          }
                          if (val == kMeshBroadcastGroupId) {
                            _snack(l.tr('group_id_reserved'), backgroundColor: context.palette.error);
                            return;
                          }
                          Navigator.pop(ctx);
                          final ok = await widget.ble.addGroup(val);
                          if (ok && mounted) {
                            setState(() {
                              _applyGroups([..._groups, val]);
                            });
                          }
                          await _refresh();
                          if (mounted) {
                            _snack('${l.tr('group')} $val ${l.tr('added')}');
                          }
                        },
                        style: FilledButton.styleFrom(
                          backgroundColor: context.palette.primary,
                          foregroundColor: Colors.white,
                          padding: const EdgeInsets.symmetric(vertical: 12),
                          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
                          visualDensity: VisualDensity.standard,
                        ),
                        child: Text(l.tr('add'), style: const TextStyle(fontWeight: FontWeight.w600, fontSize: 15)),
                      ),
                    ],
                  ),
                ),
              ),
            ),
          ),
        );
      },
    );
  }

  Future<void> _removeGroup(int gid) async {
    final l = context.l10n;
    final ok = await showRiftConfirmDialog(
      context: context,
      title: l.tr('delete'),
      message: '${l.tr('group')} $gid',
      cancelText: l.tr('cancel'),
      confirmText: l.tr('delete'),
      danger: true,
    );
    if (ok != true) return;
    await widget.ble.removeGroup(gid);
    if (mounted) {
      setState(() {
        _applyGroups(_groups.where((g) => g != gid));
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;

    final inner = Material(
        color: Colors.transparent,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            if (widget.embedded)
              Padding(
                padding: const EdgeInsets.fromLTRB(10, 0, 10, 0),
                child: Row(
                  children: [
                    Expanded(
                      child: Text(
                        l.tr('groups_hint'),
                        maxLines: 2,
                        overflow: TextOverflow.ellipsis,
                        style: TextStyle(
                          fontSize: 12,
                          height: 1.3,
                          color: context.palette.onSurfaceVariant.withOpacity(0.9),
                        ),
                      ),
                    ),
                    TextButton.icon(
                      style: TextButton.styleFrom(
                        visualDensity: VisualDensity.compact,
                        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                        minimumSize: Size.zero,
                        tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                      ),
                      onPressed: widget.ble.isConnected && !_loading ? _refresh : null,
                      icon: _loading
                          ? SizedBox(
                              width: 16,
                              height: 16,
                              child: CircularProgressIndicator(strokeWidth: 2, color: context.palette.primary),
                            )
                          : Icon(Icons.refresh_rounded, size: 18, color: context.palette.primary),
                      label: Text(l.tr('refresh'), style: TextStyle(fontSize: 13, color: context.palette.primary, fontWeight: FontWeight.w600)),
                    ),
                  ],
                ),
              )
            else
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 6, 16, 4),
                child: Text(
                  l.tr('groups_hint'),
                  maxLines: 2,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    fontSize: 12,
                    height: 1.3,
                    color: context.palette.onSurfaceVariant.withOpacity(0.9),
                  ),
                ),
              ),
            Expanded(
              child: _groups.isEmpty
                  ? Center(
                      child: Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 24),
                        child: Column(
                          mainAxisAlignment: MainAxisAlignment.center,
                          children: [
                            Icon(Icons.group_off, size: 48, color: context.palette.onSurfaceVariant.withOpacity(0.4)),
                            const SizedBox(height: 12),
                            Text(
                              l.tr('no_groups'),
                              textAlign: TextAlign.center,
                              style: TextStyle(color: context.palette.onSurfaceVariant.withOpacity(0.92), fontSize: 13, height: 1.3),
                            ),
                          ],
                        ),
                      ),
                    )
                  : ListView.builder(
                      padding: const EdgeInsets.fromLTRB(10, 4, 10, 72),
                      itemCount: _groups.length,
                      itemBuilder: (_, i) {
                        final gid = _groups[i];
                        return Padding(
                          padding: const EdgeInsets.only(bottom: 6),
                          child: Material(
                            color: context.palette.card,
                            elevation: 0,
                            shape: RoundedRectangleBorder(
                              borderRadius: BorderRadius.circular(14),
                              side: BorderSide(color: context.palette.divider.withOpacity(0.55)),
                            ),
                            clipBehavior: Clip.antiAlias,
                            child: Padding(
                              padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 2),
                              child: Row(
                                children: [
                                  Padding(
                                    padding: const EdgeInsets.only(left: 8),
                                    child: CircleAvatar(
                                      radius: 18,
                                      backgroundColor: context.palette.primary.withOpacity(0.12),
                                      child: Text(
                                        '$gid',
                                        style: TextStyle(color: context.palette.primary, fontWeight: FontWeight.w700, fontSize: 10),
                                      ),
                                    ),
                                  ),
                                  const SizedBox(width: 10),
                                  Expanded(
                                    child: Column(
                                      crossAxisAlignment: CrossAxisAlignment.start,
                                      children: [
                                        Text(
                                          '${l.tr('group')} $gid',
                                          style: TextStyle(color: context.palette.onSurface, fontWeight: FontWeight.w600, fontSize: 15),
                                        ),
                                        const SizedBox(height: 2),
                                        Text(
                                          _groupPrivate[gid] == true ? l.tr('group_private') : l.tr('group_public'),
                                          style: TextStyle(
                                            color: _groupPrivate[gid] == true ? context.palette.primary : context.palette.onSurfaceVariant,
                                            fontSize: 12,
                                            fontWeight: FontWeight.w500,
                                          ),
                                        ),
                                      ],
                                    ),
                                  ),
                                  PopupMenuButton<String>(
                                    tooltip: l.tr('settings'),
                                    onSelected: (v) async {
                                      if (v == 'private') await _setGroupPrivate(gid, true);
                                      if (v == 'public') await _setGroupPrivate(gid, false);
                                      if (v == 'rotate') await _rotatePrivateKey(gid);
                                      if (v == 'invite') await _copyInvite(gid);
                                    },
                                    itemBuilder: (ctx) => [
                                      if (_groupPrivate[gid] == true)
                                        PopupMenuItem(value: 'invite', child: Text(l.tr('group_copy_invite')))
                                      else
                                        PopupMenuItem(value: 'private', child: Text(l.tr('group_make_private'))),
                                      if (_groupPrivate[gid] == true)
                                        PopupMenuItem(value: 'rotate', child: Text(l.tr('group_rotate_key'))),
                                      if (_groupPrivate[gid] == true)
                                        PopupMenuItem(value: 'public', child: Text(l.tr('group_make_public'))),
                                    ],
                                    icon: Icon(Icons.lock_outline_rounded, color: context.palette.onSurfaceVariant),
                                  ),
                                  IconButton(
                                    visualDensity: VisualDensity.compact,
                                    padding: const EdgeInsets.all(8),
                                    constraints: const BoxConstraints(minWidth: 36, minHeight: 36),
                                    icon: Icon(Icons.remove_circle_outline_rounded, size: 22, color: context.palette.error.withOpacity(0.85)),
                                    tooltip: l.tr('delete'),
                                    onPressed: () => _removeGroup(gid),
                                  ),
                                ],
                              ),
                            ),
                          ),
                        );
                      },
                    ),
            ),
          ],
        ),
    );
    final body = widget.embedded ? inner : MeshBackgroundWrapper(child: inner);

    final fab = FloatingActionButton(
      heroTag: widget.embedded ? 'groups_embedded_fab' : 'groups_screen_fab',
      backgroundColor: context.palette.primary,
      foregroundColor: Colors.white,
      onPressed: widget.ble.isConnected ? _showAddSheet : null,
      tooltip: l.tr('add_group'),
      child: const Icon(Icons.add),
    );

    if (widget.embedded) {
      return Scaffold(
        backgroundColor: Colors.transparent,
        body: body,
        floatingActionButton: fab,
      );
    }

    return Scaffold(
      backgroundColor: context.palette.surface,
      appBar: AppBar(
        title: Text(l.tr('groups')),
        leading: IconButton(icon: const Icon(Icons.arrow_back), onPressed: () => Navigator.pop(context)),
        actions: [
          IconButton(
            onPressed: _loading ? null : _refresh,
            icon: _loading
                ? SizedBox(
                    width: 22,
                    height: 22,
                    child: CircularProgressIndicator(strokeWidth: 2, color: context.palette.onSurface),
                  )
                : const Icon(Icons.refresh),
            tooltip: l.tr('refresh'),
          ),
        ],
      ),
      body: body,
      floatingActionButton: fab,
    );
  }
}
