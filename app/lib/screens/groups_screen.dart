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
import '../theme/design_tokens.dart';
import '../widgets/app_primitives.dart';
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
  final Map<int, int> _groupKeyVersion = <int, int>{};
  final Map<int, RiftLinkGroupV2Info> _groupV2ByChannel = <int, RiftLinkGroupV2Info>{};
  bool _loading = false;
  StreamSubscription<RiftLinkEvent>? _sub;

  List<int> _normalizeGroups(Iterable<int> groups) {
    final out = groups.where((g) => g != kMeshBroadcastGroupId && g > 0).toSet().toList();
    out.sort();
    return out;
  }

  void _applyGroups(
    Iterable<int> groups, [
    List<RiftLinkGroupV2Info>? groupsV2,
  ]) {
    final fromV2 = (groupsV2 ?? const <RiftLinkGroupV2Info>[])
        .map((e) => e.channelId32)
        .where((e) => e > 1 && e != kMeshBroadcastGroupId);
    final normalized = _normalizeGroups([...groups, ...fromV2]);
    final nextVer = <int, int>{};
    final nextV2 = <int, RiftLinkGroupV2Info>{};
    for (final g in groupsV2 ?? const <RiftLinkGroupV2Info>[]) {
      if (g.channelId32 > 1 && g.channelId32 != kMeshBroadcastGroupId) {
        nextV2[g.channelId32] = g;
      }
    }
    for (var i = 0; i < normalized.length; i++) {
      final gid = normalized[i];
      int ver = _groupKeyVersion[gid] ?? 0;
      final v2 = nextV2[gid] ?? _groupV2ByChannel[gid];
      if (v2 != null) {
        ver = v2.keyVersion > 0 ? v2.keyVersion : ver;
      }
      nextVer[gid] = ver;
    }
    _groups = normalized;
    _groupKeyVersion
      ..clear()
      ..addAll(nextVer);
    _groupV2ByChannel
      ..clear()
      ..addAll(nextV2);
  }

  int? _channelByUid(String groupUid) {
    final uid = groupUid.trim().toUpperCase();
    if (uid.isEmpty) return null;
    for (final entry in _groupV2ByChannel.entries) {
      if (entry.value.groupUid.toUpperCase() == uid) return entry.key;
    }
    return null;
  }

  String? _uidByChannel(int gid) {
    final uid = _groupV2ByChannel[gid]?.groupUid.trim().toUpperCase();
    return (uid == null || uid.isEmpty) ? null : uid;
  }

  String _groupDisplayName(int gid) {
    final v2 = _groupV2ByChannel[gid];
    final name = v2?.canonicalName.trim();
    if (name != null && name.isNotEmpty) return name;
    return '${context.l10n.tr('group')} $gid';
  }

  @override
  void initState() {
    super.initState();
    _applyGroups(const <int>[], widget.ble.lastInfo?.groupsV2 ?? const <RiftLinkGroupV2Info>[]);
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      // Ответ на getGroups — и поле groups в evt:info (как в чате), иначе список не обновлялся без отдельного notify.
      if (evt is RiftLinkGroupsEvent) {
        setState(() => _applyGroups(
              const <int>[],
              evt.groupsV2,
            ));
      } else if (evt is RiftLinkInfoEvent) {
        setState(() => _applyGroups(
              const <int>[],
              evt.groupsV2,
            ));
      } else if (evt is RiftLinkGroupStatusEvent) {
        final gid = evt.channelId32 ?? _channelByUid(evt.groupUid);
        if (gid == null || gid <= 1) return;
        final prev = _groupV2ByChannel[gid];
        final next = RiftLinkGroupV2Info(
          groupUid: evt.groupUid,
          groupTag: prev?.groupTag ?? '',
          canonicalName: evt.canonicalName.trim().isNotEmpty
              ? evt.canonicalName
              : (prev?.canonicalName ?? ''),
          channelId32: gid,
          keyVersion: evt.keyVersion,
          myRole: evt.myRole,
          revocationEpoch: prev?.revocationEpoch ?? 0,
          ackApplied: !evt.rekeyRequired,
        );
        setState(() {
          _groupV2ByChannel[gid] = next;
          _groupKeyVersion[gid] = evt.keyVersion;
          if (!_groups.contains(gid)) {
            _applyGroups([..._groups, gid], _groupV2ByChannel.values.toList());
          }
        });
      } else if (evt is RiftLinkGroupRekeyProgressEvent) {
        final gid = _channelByUid(evt.groupUid);
        if (gid != null && mounted) {
          _snack(
            '${context.l10n.tr('group_rotate_key')}: v${evt.keyVersion} (${evt.applied}/${evt.applied + evt.pending + evt.failed})',
          );
        }
      } else if (evt is RiftLinkGroupMemberKeyStateEvent) {
        final gid = _channelByUid(evt.groupUid);
        if (gid != null && mounted) {
          _snack('${context.l10n.tr('group')} $gid: ${evt.memberId} -> ${evt.status}');
        }
      } else if (evt is RiftLinkGroupSecurityErrorEvent) {
        _snack('${evt.code}: ${evt.msg}', backgroundColor: context.palette.error);
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

  String _generateBase64Token(int sizeBytes) {
    final rnd = Random.secure();
    final bytes = List<int>.generate(sizeBytes, (_) => rnd.nextInt(256));
    return base64Encode(bytes);
  }

  Future<void> _requestGroupStatus(int gid) async {
    final l = context.l10n;
    final uid = _uidByChannel(gid);
    if (uid == null) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
      return;
    }
    final ok = await widget.ble.groupStatus(uid);
    if (!ok) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
      return;
    }
    _snack('${l.tr('refresh')}: ${l.tr('group')} $gid');
  }

  Future<void> _ackCurrentGroupKey(int gid) async {
    final l = context.l10n;
    final v2 = _groupV2ByChannel[gid];
    if (v2 == null || v2.groupUid.trim().isEmpty || v2.keyVersion <= 0) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
      return;
    }
    final ok = await widget.ble.groupAckKeyApplied(
      groupUid: v2.groupUid,
      keyVersion: v2.keyVersion,
    );
    if (!ok) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
      return;
    }
    setState(() {
      _groupV2ByChannel[gid] = RiftLinkGroupV2Info(
        groupUid: v2.groupUid,
        groupTag: v2.groupTag,
        canonicalName: v2.canonicalName,
        channelId32: v2.channelId32,
        keyVersion: v2.keyVersion,
        myRole: v2.myRole,
        revocationEpoch: v2.revocationEpoch,
        ackApplied: true,
      );
    });
    _snack(l.tr('group_key_actual'));
  }

  Future<void> _rekeyGroupV2(int gid) async {
    final l = context.l10n;
    final uid = _uidByChannel(gid);
    if (uid == null) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
      return;
    }
    final ok = await widget.ble.groupRekey(groupUid: uid, reason: 'ui_manual_rekey');
    if (!ok) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
      return;
    }
    _snack('${l.tr('group_rotate_key')} · ${l.tr('group')} $gid');
  }

  Future<void> _renameCanonicalName(int gid) async {
    final l = context.l10n;
    final uid = _uidByChannel(gid);
    final v2 = _groupV2ByChannel[gid];
    if (uid == null || v2 == null) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
      return;
    }
    if (v2.myRole != 'owner') {
      _snack('Only owner can rename', backgroundColor: context.palette.error);
      return;
    }
    final ctrl = TextEditingController(text: v2.canonicalName.trim().isEmpty ? _groupDisplayName(gid) : v2.canonicalName.trim());
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Rename canonical name'),
        content: TextField(
          controller: ctrl,
          autofocus: true,
          decoration: const InputDecoration(hintText: 'Canonical name'),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: Text(l.tr('cancel'))),
          FilledButton(onPressed: () => Navigator.pop(ctx, true), child: Text(l.tr('ok'))),
        ],
      ),
    );
    if (ok != true) return;
    final name = ctrl.text.trim();
    if (name.isEmpty) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
      return;
    }
    final renamed = await widget.ble.groupCanonicalRename(groupUid: uid, canonicalName: name);
    if (!renamed) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
      return;
    }
    _snack('Canonical name updated');
  }

  Future<void> _showGrantDialog(int gid) async {
    final l = context.l10n;
    final uid = _uidByChannel(gid);
    if (uid == null) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
      return;
    }
    String role = 'member';
    final idCtrl = TextEditingController();
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => StatefulBuilder(
        builder: (ctx, setLocalState) => AlertDialog(
          title: Text('${l.tr('group')} $gid · ${l.tr('group_role_admin')} / ${l.tr('group_role_member')}'),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              TextField(
                controller: idCtrl,
                autofocus: true,
                textCapitalization: TextCapitalization.characters,
                decoration: const InputDecoration(hintText: 'Node ID (16 HEX)'),
              ),
              const SizedBox(height: AppSpacing.sm),
              SegmentedButton<String>(
                segments: [
                  ButtonSegment<String>(value: 'member', label: Text(l.tr('group_role_member'))),
                  ButtonSegment<String>(value: 'admin', label: Text(l.tr('group_role_admin'))),
                ],
                selected: {role},
                onSelectionChanged: (v) => setLocalState(() => role = v.first),
              ),
            ],
          ),
          actions: [
            TextButton(onPressed: () => Navigator.pop(ctx, false), child: Text(l.tr('cancel'))),
            FilledButton(onPressed: () => Navigator.pop(ctx, true), child: Text(l.tr('ok'))),
          ],
        ),
      ),
    );
    if (ok != true) return;
    final subject = idCtrl.text.trim().toUpperCase();
    if (!RiftLinkBle.isValidFullNodeId(subject)) {
      _snack(l.tr('invalid_hex'), backgroundColor: context.palette.error);
      return;
    }
    final issued = await widget.ble.groupGrantIssue(
      groupUid: uid,
      subjectId: subject,
      role: role,
    );
    if (!issued) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
      return;
    }
    _snack('Grant: $subject -> $role');
  }

  Future<void> _showRevokeDialog(int gid) async {
    final l = context.l10n;
    final uid = _uidByChannel(gid);
    if (uid == null) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
      return;
    }
    final idCtrl = TextEditingController();
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text('${l.tr('group')} $gid · revoke'),
        content: TextField(
          controller: idCtrl,
          autofocus: true,
          textCapitalization: TextCapitalization.characters,
          decoration: const InputDecoration(hintText: 'Node ID (16 HEX)'),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: Text(l.tr('cancel'))),
          FilledButton(onPressed: () => Navigator.pop(ctx, true), child: Text(l.tr('delete'))),
        ],
      ),
    );
    if (ok != true) return;
    final subject = idCtrl.text.trim().toUpperCase();
    if (!RiftLinkBle.isValidFullNodeId(subject)) {
      _snack(l.tr('invalid_hex'), backgroundColor: context.palette.error);
      return;
    }
    final revoked = await widget.ble.groupRevoke(
      groupUid: uid,
      subjectId: subject,
      reason: 'ui_revoke',
    );
    if (!revoked) {
      _snack(l.tr('error'), backgroundColor: context.palette.error);
      return;
    }
    await widget.ble.groupRekey(groupUid: uid, reason: 'revoke_followup_rekey');
    _snack('Revoke + rekey: $subject');
  }

  Future<void> _copyInvite(int gid) async {
    final l = context.l10n;
    if (!widget.ble.isConnected) return;
    final v2 = _groupV2ByChannel[gid];
    if (v2 != null && v2.groupUid.trim().isNotEmpty) {
      final ok = await widget.ble.groupInviteCreate(
        groupUid: v2.groupUid,
        role: 'member',
      );
      if (!ok) {
        _snack(l.tr('error'), backgroundColor: context.palette.error);
        return;
      }
      try {
        final evt = await widget.ble.events
            .where((e) => e is RiftLinkGroupInviteEvent && (e as RiftLinkGroupInviteEvent).groupUid == v2.groupUid)
            .cast<RiftLinkGroupInviteEvent>()
            .first
            .timeout(const Duration(seconds: 3));
        await Clipboard.setData(ClipboardData(text: evt.invite));
        if (mounted) _snack(l.tr('group_invite_copied', {'id': '$gid'}));
        return;
      } catch (_) {
        _snack(l.tr('error'), backgroundColor: context.palette.error);
        return;
      }
    }
    _snack(l.tr('error'), backgroundColor: context.palette.error);
  }

  Future<void> _joinByInviteCode(String inviteCodeRaw) async {
    final l = context.l10n;
    final inviteCode = inviteCodeRaw.trim();
    if (inviteCode.isEmpty) {
      _snack(l.tr('group_invite_bad'), backgroundColor: context.palette.error);
      return;
    }
    final acceptedV2 = await widget.ble.groupInviteAccept(inviteCode);
    if (acceptedV2) {
      await _refresh();
      if (mounted) _snack(l.tr('group_invite_joined_v2'));
      return;
    }
    _snack(l.tr('group_invite_bad'), backgroundColor: context.palette.error);
  }

  Future<void> _pasteInviteAndJoin() async {
    final data = await Clipboard.getData(Clipboard.kTextPlain);
    final text = data?.text?.trim() ?? '';
    await _joinByInviteCode(text);
  }

  void _showJoinByCodeDialog() {
    if (!widget.ble.isConnected) return;
    final c = TextEditingController();
    final l = context.l10n;
    final p = context.palette;
    showAppDialog(
      context: context,
      builder: (ctx) => RiftDialogFrame(
        maxWidth: 380,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(
              l.tr('group_join_by_code'),
              style: Theme.of(context).textTheme.titleMedium?.copyWith(
                    fontWeight: FontWeight.w700,
                    color: p.onSurface,
                  ),
            ),
            const SizedBox(height: AppSpacing.sm),
            Text(
              l.tr('group_invite_code_hint'),
              style: AppTypography.labelBase().copyWith(
                color: p.onSurfaceVariant.withOpacity(0.92),
                height: 1.3,
              ),
            ),
            const SizedBox(height: AppSpacing.md),
            TextField(
              controller: c,
              maxLines: 2,
              minLines: 1,
              autofocus: true,
              textInputAction: TextInputAction.done,
              style: TextStyle(color: p.onSurface, fontSize: 15),
              decoration: InputDecoration(
                labelText: l.tr('group_join_by_code'),
                hintText: 'BASE64...',
              ),
              onSubmitted: (_) async {
                Navigator.pop(ctx);
                await _joinByInviteCode(c.text);
              },
            ),
            const SizedBox(height: AppSpacing.md),
            Row(
              mainAxisAlignment: MainAxisAlignment.end,
              children: [
                TextButton(
                  onPressed: () async {
                    final data = await Clipboard.getData(Clipboard.kTextPlain);
                    c.text = data?.text?.trim() ?? '';
                  },
                  child: Text(l.tr('paste')),
                ),
                const SizedBox(width: AppSpacing.xs),
                TextButton(
                  onPressed: () => Navigator.pop(ctx),
                  child: Text(l.tr('cancel')),
                ),
                const SizedBox(width: AppSpacing.xs),
                FilledButton(
                  onPressed: () async {
                    Navigator.pop(ctx);
                    await _joinByInviteCode(c.text);
                  },
                  child: Text(l.tr('ok')),
                ),
              ],
            ),
          ],
        ),
      ),
    );
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
        final p = context.palette;
        final bottomInset = MediaQuery.of(ctx).viewInsets.bottom;
        return Padding(
          padding: EdgeInsets.only(bottom: bottomInset),
          child: Container(
            decoration: BoxDecoration(
              color: p.card,
              borderRadius: const BorderRadius.vertical(top: Radius.circular(16)),
            ),
            child: SafeArea(
              top: false,
              child: SingleChildScrollView(
                child: Padding(
                  padding: const EdgeInsets.fromLTRB(
                    AppSpacing.lg,
                    AppSpacing.md,
                    AppSpacing.lg,
                    AppSpacing.xl,
                  ),
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    crossAxisAlignment: CrossAxisAlignment.stretch,
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
                      Row(
                        children: [
                          Icon(Icons.group_add_rounded, color: p.primary, size: 22),
                          const SizedBox(width: AppSpacing.sm + 2),
                          Expanded(
                            child: Text(
                              l.tr('add_group'),
                              style: AppTypography.screenTitleBase().copyWith(
                                fontSize: 17,
                                color: p.onSurface,
                              ),
                            ),
                          ),
                          IconButton(
                            icon: Icon(Icons.content_paste_rounded, color: p.onSurfaceVariant.withOpacity(0.85)),
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
                            icon: Icon(Icons.close_rounded, color: p.onSurfaceVariant.withOpacity(0.85)),
                            onPressed: () => Navigator.pop(ctx),
                            visualDensity: VisualDensity.compact,
                            padding: EdgeInsets.zero,
                            constraints: const BoxConstraints(minWidth: 36, minHeight: 36),
                          ),
                        ],
                      ),
                      const SizedBox(height: AppSpacing.sm - 2),
                      Text(
                        l.tr('groups_add_sheet_hint'),
                        maxLines: 3,
                        overflow: TextOverflow.ellipsis,
                        style: AppTypography.labelBase().copyWith(
                          fontSize: 12,
                          height: 1.32,
                          fontWeight: FontWeight.w400,
                          color: p.onSurfaceVariant.withOpacity(0.92),
                        ),
                      ),
                      const SizedBox(height: AppSpacing.md),
                      TextField(
                        controller: c,
                        keyboardType: TextInputType.number,
                        autofocus: true,
                        style: TextStyle(color: p.onSurface, fontSize: 16),
                        decoration: InputDecoration(
                          labelText: l.tr('group_id_hint'),
                          hintText: '42',
                          filled: true,
                          fillColor: p.surfaceVariant.withOpacity(0.55),
                          border: OutlineInputBorder(
                            borderRadius: BorderRadius.circular(AppRadius.md),
                            borderSide: BorderSide.none,
                          ),
                          contentPadding: const EdgeInsets.symmetric(
                            horizontal: AppSpacing.md + 2,
                            vertical: AppSpacing.md,
                          ),
                        ),
                      ),
                      const SizedBox(height: AppSpacing.md + 2),
                      AppPrimaryButton(
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
                          final ok = await widget.ble.groupCreate(
                            groupUid: _generateBase64Token(16),
                            displayName: '${l.tr('group')} $val',
                            channelId32: val,
                            groupTag: _generateBase64Token(8),
                          );
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
                        child: Text(
                          l.tr('add'),
                          style: const TextStyle(fontWeight: FontWeight.w600, fontSize: 15),
                        ),
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

  Widget _buildEmptyState(AppLocalizations l, AppPalette p) {
    return Center(
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            TweenAnimationBuilder<double>(
              tween: Tween(begin: 0.0, end: 1.0),
              duration: const Duration(milliseconds: 600),
              curve: Curves.easeOut,
              builder: (_, opacity, child) => Opacity(opacity: opacity, child: child),
              child: Icon(Icons.groups_outlined, size: 72, color: p.onSurfaceVariant.withOpacity(0.38)),
            ),
            const SizedBox(height: AppSpacing.lg),
            Text(
              l.tr('no_groups'),
              textAlign: TextAlign.center,
              style: AppTypography.bodyBase().copyWith(
                fontSize: 16,
                fontWeight: FontWeight.w600,
                color: p.onSurface,
              ),
            ),
            const SizedBox(height: AppSpacing.xl),
            FilledButton.icon(
              onPressed: widget.ble.isConnected ? _showAddSheet : null,
              icon: const Icon(Icons.group_add_rounded, size: 18),
              label: Text(
                l.tr('add_group'),
                style: const TextStyle(fontWeight: FontWeight.w600),
              ),
              style: FilledButton.styleFrom(
                backgroundColor: p.primary,
                foregroundColor: Colors.white,
                disabledBackgroundColor: p.primary.withOpacity(0.3),
                disabledForegroundColor: Colors.white.withOpacity(0.5),
                padding: const EdgeInsets.symmetric(horizontal: AppSpacing.xl, vertical: AppSpacing.md),
                shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(AppRadius.md)),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildGroupCard(int gid, AppLocalizations l, AppPalette p) {
    final v2 = _groupV2ByChannel[gid];
    final ver = _groupKeyVersion[gid] ?? 0;
    final canRotate = v2 != null && (v2.myRole == 'owner' || v2.myRole == 'admin');
    final roleLabel = switch (v2?.myRole) {
      'owner' => l.tr('group_role_owner'),
      'admin' => l.tr('group_role_admin'),
      'member' => l.tr('group_role_member'),
      _ => null,
    };
    final keyStateLabel = v2 == null
        ? null
        : (v2.ackApplied ? l.tr('group_key_actual') : l.tr('group_key_rekey_required_short'));

    return AppSectionCard(
      margin: const EdgeInsets.only(bottom: AppSpacing.sm),
      padding: EdgeInsets.zero,
      child: ListTile(
        onTap: () => HapticFeedback.lightImpact(),
        contentPadding: const EdgeInsets.symmetric(horizontal: AppSpacing.md, vertical: 4),
        leading: CircleAvatar(
          radius: 20,
          backgroundColor: p.primary.withOpacity(0.13),
          child: Text(
            '$gid',
            style: TextStyle(color: p.primary, fontWeight: FontWeight.w700, fontSize: 10),
          ),
        ),
        title: Row(
          children: [
            Expanded(
              child: Text(
                _groupDisplayName(gid),
                style: AppTypography.bodyBase().copyWith(
                  fontWeight: FontWeight.w600,
                  color: p.onSurface,
                ),
              ),
            ),
            if (v2 != null) Icon(Icons.lock_rounded, size: 16, color: p.primary),
          ],
        ),
        subtitle: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            AppStateChip(
              label: v2 != null ? 'V2 · v$ver' : 'V2: unresolved',
              kind: v2 != null ? AppStateKind.info : AppStateKind.neutral,
            ),
            if (v2 != null)
              Padding(
                padding: const EdgeInsets.only(top: 4),
                child: Text(
                  [
                    if (roleLabel != null) roleLabel,
                    if (keyStateLabel != null) keyStateLabel,
                  ].join(' · '),
                  style: AppTypography.labelBase().copyWith(
                    fontSize: 11.5,
                    fontWeight: FontWeight.w600,
                    color: v2.ackApplied ? p.onSurfaceVariant : p.error,
                  ),
                ),
              ),
          ],
        ),
        trailing: PopupMenuButton<String>(
          tooltip: l.tr('settings'),
          position: PopupMenuPosition.under,
          icon: Icon(Icons.more_vert_rounded, color: p.onSurfaceVariant),
          onSelected: (v) async {
            if (v == 'invite') await _copyInvite(gid);
            if (v == 'status') await _requestGroupStatus(gid);
            if (v == 'ack') await _ackCurrentGroupKey(gid);
            if (v == 'rekey_v2') await _rekeyGroupV2(gid);
            if (v == 'rename') await _renameCanonicalName(gid);
            if (v == 'grant') await _showGrantDialog(gid);
            if (v == 'revoke') await _showRevokeDialog(gid);
          },
          itemBuilder: (ctx) => [
            PopupMenuItem(value: 'status', child: Text('${l.tr('refresh')} status')),
            if (v2 != null) PopupMenuItem(value: 'invite', child: Text(l.tr('group_copy_invite'))),
            if (v2 != null && !(v2.ackApplied))
              PopupMenuItem(value: 'ack', child: Text('${l.tr('group_key_actual')} ACK')),
            if (v2 != null && canRotate)
              PopupMenuItem(value: 'rekey_v2', child: Text('V2 ${l.tr('group_rotate_key')}')),
            if (v2 != null && v2.myRole == 'owner')
              const PopupMenuItem(value: 'rename', child: Text('Rename canonical name')),
            if (v2 != null && v2.myRole == 'owner')
              const PopupMenuItem(value: 'grant', child: Text('Grant role')),
            if (v2 != null && (v2.myRole == 'owner' || v2.myRole == 'admin'))
              const PopupMenuItem(value: 'revoke', child: Text('Revoke member')),
          ],
        ),
      ),
    );
  }

  Widget _buildHintBanner(AppLocalizations l, AppPalette p) {
    final hintStyle = AppTypography.labelBase().copyWith(
      fontWeight: FontWeight.w400,
      height: 1.3,
      color: p.onSurfaceVariant.withOpacity(0.9),
    );
    if (widget.embedded) {
      return Padding(
        padding: const EdgeInsets.fromLTRB(AppSpacing.sm + AppSpacing.xs, 0, AppSpacing.sm + AppSpacing.xs, 0),
        child: Row(
          children: [
            Expanded(
              child: Text(
                l.tr('groups_hint'),
                maxLines: 2,
                overflow: TextOverflow.ellipsis,
                style: hintStyle.copyWith(fontSize: 12),
              ),
            ),
            TextButton.icon(
              style: TextButton.styleFrom(
                visualDensity: VisualDensity.compact,
                padding: const EdgeInsets.symmetric(horizontal: AppSpacing.sm, vertical: AppSpacing.xs),
                minimumSize: Size.zero,
                tapTargetSize: MaterialTapTargetSize.shrinkWrap,
              ),
              onPressed: widget.ble.isConnected && !_loading ? _refresh : null,
              icon: _loading
                  ? SizedBox(
                      width: 16,
                      height: 16,
                      child: CircularProgressIndicator(strokeWidth: 2, color: p.primary),
                    )
                  : Icon(Icons.refresh_rounded, size: 18, color: p.primary),
              label: Text(
                l.tr('refresh'),
                style: TextStyle(fontSize: 13, color: p.primary, fontWeight: FontWeight.w600),
              ),
            ),
          ],
        ),
      );
    }
    return Padding(
      padding: const EdgeInsets.fromLTRB(AppSpacing.lg, AppSpacing.sm, AppSpacing.lg, AppSpacing.xs),
      child: Text(
        l.tr('groups_hint'),
        maxLines: 2,
        overflow: TextOverflow.ellipsis,
        style: hintStyle.copyWith(fontSize: 12),
      ),
    );
  }

  Widget _buildTopActions(AppLocalizations l, AppPalette p) {
    return Padding(
      padding: EdgeInsets.fromLTRB(
        widget.embedded ? AppSpacing.sm + AppSpacing.xs : AppSpacing.lg,
        AppSpacing.xs,
        widget.embedded ? AppSpacing.sm + AppSpacing.xs : AppSpacing.lg,
        AppSpacing.sm,
      ),
      child: Row(
        children: [
          Expanded(
            child: FilledButton.icon(
              onPressed: widget.ble.isConnected ? _showAddSheet : null,
              icon: const Icon(Icons.group_add_rounded, size: 18),
              label: Text(l.tr('group_create')),
            ),
          ),
          const SizedBox(width: AppSpacing.sm),
          Expanded(
            child: OutlinedButton.icon(
              onPressed: widget.ble.isConnected ? _showJoinByCodeDialog : null,
              icon: const Icon(Icons.vpn_key_outlined, size: 18),
              label: Text(l.tr('group_join_by_code')),
            ),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final p = context.palette;

    final inner = Material(
      color: Colors.transparent,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          _buildHintBanner(l, p),
          _buildTopActions(l, p),
          Expanded(
            child: _groups.isEmpty
                ? _buildEmptyState(l, p)
                : ListView.builder(
                    padding: const EdgeInsets.fromLTRB(
                      AppSpacing.sm + AppSpacing.xs,
                      AppSpacing.xs,
                      AppSpacing.sm + AppSpacing.xs,
                      72,
                    ),
                    itemCount: _groups.length,
                    itemBuilder: (_, i) => _buildGroupCard(_groups[i], l, p),
                  ),
          ),
        ],
      ),
    );
    final body = widget.embedded ? inner : MeshBackgroundWrapper(child: inner);

    if (widget.embedded) {
      return Scaffold(
        backgroundColor: Colors.transparent,
        body: body,
      );
    }

    return Scaffold(
      backgroundColor: p.surface,
      appBar: riftAppBar(
        context,
        title: l.tr('groups'),
        showBack: true,
        actions: [
          IconButton(
            onPressed: _loading ? null : _refresh,
            icon: _loading
                ? SizedBox(
                    width: 22,
                    height: 22,
                    child: CircularProgressIndicator(strokeWidth: 2, color: p.onSurface),
                  )
                : Icon(Icons.refresh_rounded, color: p.onSurface),
            tooltip: l.tr('refresh'),
          ),
        ],
      ),
      body: body,
    );
  }
}
