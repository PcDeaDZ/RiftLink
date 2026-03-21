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
  final Map<int, bool> _groupPrivate = <int, bool>{};
  final Map<int, int> _groupKeyVersion = <int, int>{};
  bool _loading = false;
  StreamSubscription<RiftLinkEvent>? _sub;

  List<int> _normalizeGroups(Iterable<int> groups) {
    final out = groups.where((g) => g != kMeshBroadcastGroupId && g > 0).toSet().toList();
    out.sort();
    return out;
  }

  void _applyGroups(Iterable<int> groups, [List<bool>? privateFlags, List<int>? keyVersions]) {
    final normalized = _normalizeGroups(groups);
    final nextPriv = <int, bool>{};
    final nextVer = <int, int>{};
    for (var i = 0; i < normalized.length; i++) {
      final gid = normalized[i];
      bool isPriv = _groupPrivate[gid] ?? false;
      int ver = _groupKeyVersion[gid] ?? 0;
      if (privateFlags != null && i < privateFlags.length) isPriv = privateFlags[i];
      if (keyVersions != null && i < keyVersions.length) ver = keyVersions[i];
      nextPriv[gid] = isPriv;
      nextVer[gid] = ver;
    }
    _groups = normalized;
    _groupPrivate
      ..clear()
      ..addAll(nextPriv);
    _groupKeyVersion
      ..clear()
      ..addAll(nextVer);
  }

  @override
  void initState() {
    super.initState();
    _applyGroups(widget.initialGroups);
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      // Ответ на getGroups — и поле groups в evt:info (как в чате), иначе список не обновлялся без отдельного notify.
      if (evt is RiftLinkGroupsEvent) {
        setState(() => _applyGroups(evt.groups, evt.groupsPrivate, evt.groupsKeyVersion));
      } else if (evt is RiftLinkInfoEvent) {
        setState(() => _applyGroups(evt.groups, evt.groupsPrivate, evt.groupsKeyVersion));
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

  String _generateInviteTokenHex() {
    final rnd = Random.secure();
    final bytes = List<int>.generate(8, (_) => rnd.nextInt(256));
    final sb = StringBuffer();
    for (final b in bytes) {
      sb.write(b.toRadixString(16).padLeft(2, '0'));
    }
    return sb.toString().toUpperCase();
  }

  Future<void> _setGroupPrivate(int gid, bool privateMode) async {
    final l = context.l10n;
    if (!widget.ble.isConnected) return;
    if (privateMode) {
      final keyB64 = _generateGroupKeyB64();
      final ok = await widget.ble.setGroupKey(gid, keyB64);
      if (ok && mounted) {
        _snack(l.tr('group_set_private', {'id': '$gid'}));
        await _copyInvite(gid);
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
    final currentVer = _groupKeyVersion[gid] ?? 0;
    final nextVer = currentVer > 0 ? currentVer + 1 : 1;
    final keyB64 = _generateGroupKeyB64();
    final ok = await widget.ble.setGroupKey(gid, keyB64, keyVersion: nextVer);
    if (ok && mounted) {
      _snack(l.tr('group_key_rotated', {'id': '$gid'}));
      await _copyInvite(gid);
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
      final expiryEpochSec = DateTime.now().millisecondsSinceEpoch ~/ 1000 + 10 * 60;
      final token = _generateInviteTokenHex();
      await Clipboard.setData(ClipboardData(text: jsonEncode({
        'group': gid,
        'groupKey': evt.key,
        'keyVersion': evt.keyVersion > 0 ? evt.keyVersion : (_groupKeyVersion[gid] ?? 0),
        'inviteToken': token,
        'inviteExpiryEpochSec': expiryEpochSec,
      })));
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
      final keyVersion = int.tryParse('${m['keyVersion'] ?? ''}') ?? 0;
      final token = (m['inviteToken'] as String?)?.trim() ?? '';
      final expiryEpochSec = int.tryParse('${m['inviteExpiryEpochSec'] ?? ''}') ?? 0;
      if (group <= 1) throw Exception('bad');
      if (token.length != 16 || !RegExp(r'^[0-9A-Fa-f]{16}$').hasMatch(token)) throw Exception('bad');
      if (expiryEpochSec <= 0 || (DateTime.now().millisecondsSinceEpoch ~/ 1000) > expiryEpochSec) {
        _snack(l.tr('group_invite_expired'), backgroundColor: context.palette.error);
        return;
      }
      await widget.ble.addGroup(group);
      if (key != null && key.isNotEmpty) {
        await widget.ble.setGroupKey(group, key, keyVersion: keyVersion > 0 ? keyVersion : null);
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
    final isPrivate = _groupPrivate[gid] == true;
    final ver = _groupKeyVersion[gid] ?? 0;
    return Dismissible(
      key: ValueKey<int>(gid),
      direction: DismissDirection.endToStart,
      background: Container(
        alignment: Alignment.centerRight,
        padding: const EdgeInsets.symmetric(horizontal: AppSpacing.sm + 2),
        decoration: BoxDecoration(
          color: p.error.withOpacity(0.12),
          borderRadius: BorderRadius.circular(AppRadius.sm),
          border: Border.all(color: p.error.withOpacity(0.45)),
        ),
        child: Icon(Icons.delete_outline_rounded, color: p.error),
      ),
      confirmDismiss: (direction) async {
        final ok = await showRiftConfirmDialog(
          context: context,
          title: l.tr('delete'),
          message: '${l.tr('group')} $gid',
          cancelText: l.tr('cancel'),
          confirmText: l.tr('delete'),
          danger: true,
        );
        if (ok != true) return false;
        final removed = await widget.ble.removeGroup(gid);
        if (!mounted) return false;
        if (!removed) {
          _snack(l.tr('error'), backgroundColor: context.palette.error);
          return false;
        }
        return true;
      },
      onDismissed: (_) {
        setState(() {
          _applyGroups(_groups.where((g) => g != gid));
        });
      },
      child: AppSectionCard(
        margin: const EdgeInsets.only(bottom: AppSpacing.sm),
        padding: EdgeInsets.zero,
        child: Material(
          color: Colors.transparent,
          child: InkWell(
            borderRadius: BorderRadius.circular(AppRadius.card),
            onTap: () => HapticFeedback.lightImpact(),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: AppSpacing.sm, vertical: AppSpacing.xs + 2),
              child: Row(
                children: [
                  Padding(
                    padding: const EdgeInsets.only(left: AppSpacing.xs),
                    child: CircleAvatar(
                      radius: 20,
                      backgroundColor: p.primary.withOpacity(0.13),
                      child: Text(
                        '$gid',
                        style: TextStyle(color: p.primary, fontWeight: FontWeight.w700, fontSize: 10),
                      ),
                    ),
                  ),
                  const SizedBox(width: AppSpacing.sm + 2),
                    Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Row(
                          children: [
                            Flexible(
                              child: Text(
                                '${l.tr('group')} $gid',
                                style: AppTypography.bodyBase().copyWith(
                                  fontWeight: FontWeight.w600,
                                  color: p.onSurface,
                                ),
                              ),
                            ),
                            const SizedBox(width: AppSpacing.sm),
                            AnimatedSwitcher(
                              duration: const Duration(milliseconds: 300),
                              switchInCurve: Curves.easeOutBack,
                              switchOutCurve: Curves.easeIn,
                              transitionBuilder: (child, anim) => ScaleTransition(
                                scale: anim,
                                child: FadeTransition(opacity: anim, child: child),
                              ),
                              child: isPrivate
                                  ? Icon(Icons.lock_rounded, key: const ValueKey('locked'), size: 16, color: p.primary)
                                  : const SizedBox.shrink(key: ValueKey('unlocked')),
                            ),
                          ],
                        ),
                        const SizedBox(height: AppSpacing.xs),
                        AnimatedSwitcher(
                          duration: const Duration(milliseconds: 280),
                          switchInCurve: Curves.easeOut,
                          switchOutCurve: Curves.easeIn,
                          transitionBuilder: (child, anim) {
                            final scale = Tween<double>(begin: 0.85, end: 1.0).animate(anim);
                            return ScaleTransition(
                              scale: scale,
                              child: FadeTransition(opacity: anim, child: child),
                            );
                          },
                          child: isPrivate
                              ? AppStateChip(
                                  key: ValueKey('priv-$gid-$ver'),
                                  label: '${l.tr('group_private')}  v$ver',
                                  kind: AppStateKind.info,
                                )
                              : AppStateChip(
                                  key: ValueKey('pub-$gid'),
                                  label: l.tr('group_public'),
                                  kind: AppStateKind.neutral,
                                ),
                        ),
                      ],
                    ),
                  ),
                  PopupMenuButton<String>(
                    tooltip: l.tr('settings'),
                    position: PopupMenuPosition.under,
                    icon: Icon(Icons.more_vert_rounded, color: p.onSurfaceVariant),
                    onSelected: (v) async {
                      if (v == 'private') await _setGroupPrivate(gid, true);
                      if (v == 'public') await _setGroupPrivate(gid, false);
                      if (v == 'rotate') await _rotatePrivateKey(gid);
                      if (v == 'invite') await _copyInvite(gid);
                    },
                    itemBuilder: (ctx) => [
                      if (isPrivate)
                        PopupMenuItem(value: 'invite', child: Text(l.tr('group_copy_invite')))
                      else
                        PopupMenuItem(value: 'private', child: Text(l.tr('group_make_private'))),
                      if (isPrivate) PopupMenuItem(value: 'rotate', child: Text(l.tr('group_rotate_key'))),
                      if (isPrivate) PopupMenuItem(value: 'public', child: Text(l.tr('group_make_public'))),
                    ],
                  ),
                ],
              ),
            ),
          ),
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

    final fab = FloatingActionButton(
      heroTag: widget.embedded ? 'groups_embedded_fab' : 'groups_screen_fab',
      backgroundColor: p.primary,
      foregroundColor: Colors.white,
      onPressed: widget.ble.isConnected ? _showAddSheet : null,
      tooltip: l.tr('add_group'),
      child: const Icon(Icons.add_rounded),
    );

    if (widget.embedded) {
      return Scaffold(
        backgroundColor: Colors.transparent,
        body: body,
        floatingActionButton: fab,
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
      floatingActionButton: fab,
    );
  }
}
