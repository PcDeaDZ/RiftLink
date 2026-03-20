import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app_navigator.dart';
import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../mesh_constants.dart';
import '../theme/app_theme.dart';
import '../widgets/mesh_background.dart';
import '../widgets/app_snackbar.dart';

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
  bool _loading = false;
  StreamSubscription? _sub;

  @override
  void initState() {
    super.initState();
    _groups = List.from(widget.initialGroups.where((g) => g != kMeshBroadcastGroupId));
    _sub = widget.ble.events.listen((evt) {
      if (evt is RiftLinkGroupsEvent && mounted) {
        setState(() => _groups = evt.groups.where((g) => g != kMeshBroadcastGroupId).toList());
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
                  padding: const EdgeInsets.fromLTRB(20, 12, 20, 24),
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      Center(
                        child: Container(
                          width: 40,
                          height: 4,
                          decoration: BoxDecoration(
                            color: context.palette.onSurfaceVariant.withOpacity(0.35),
                            borderRadius: BorderRadius.circular(2),
                          ),
                        ),
                      ),
                      const SizedBox(height: 16),
                      Row(
                        children: [
                          Container(
                            padding: const EdgeInsets.all(10),
                            decoration: BoxDecoration(
                              color: context.palette.primary.withOpacity(0.12),
                              borderRadius: BorderRadius.circular(14),
                            ),
                            child: Icon(Icons.group_add_rounded, color: context.palette.primary, size: 26),
                          ),
                          const SizedBox(width: 12),
                          Expanded(
                            child: Text(
                              l.tr('add_group'),
                              style: const TextStyle(
                                fontSize: 18,
                                fontWeight: FontWeight.w700,
                                color: context.palette.onSurface,
                              ),
                            ),
                          ),
                          IconButton(
                            icon: Icon(Icons.close_rounded, color: context.palette.onSurfaceVariant.withOpacity(0.85)),
                            onPressed: () => Navigator.pop(ctx),
                            visualDensity: VisualDensity.compact,
                          ),
                        ],
                      ),
                      const SizedBox(height: 8),
                      Text(
                        l.tr('groups_add_sheet_hint'),
                        style: TextStyle(
                          fontSize: 13,
                          height: 1.4,
                          color: context.palette.onSurfaceVariant.withOpacity(0.95),
                        ),
                      ),
                      const SizedBox(height: 16),
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
                          contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
                        ),
                      ),
                      const SizedBox(height: 20),
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
                          if (ok && mounted) setState(() => _groups.add(val));
                          await _refresh();
                          if (mounted) {
                            _snack('${l.tr('group')} $val ${l.tr('added')}');
                          }
                        },
                        style: FilledButton.styleFrom(
                          backgroundColor: context.palette.primary,
                          foregroundColor: Colors.white,
                          padding: const EdgeInsets.symmetric(vertical: 14),
                          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
                        ),
                        child: Text(l.tr('add'), style: const TextStyle(fontWeight: FontWeight.w600)),
                      ),
                    ],
                  ),
                ),
              ),
            ),
          ),
        );
      },
    ).then((_) => c.dispose());
  }

  Future<void> _removeGroup(int gid) async {
    await widget.ble.removeGroup(gid);
    if (mounted) setState(() => _groups.remove(gid));
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;

    final body = MeshBackgroundWrapper(
      child: Material(
        color: Colors.transparent,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 14, 16, 12),
              child: Container(
                padding: const EdgeInsets.all(14),
                decoration: BoxDecoration(
                  color: context.palette.surfaceVariant.withOpacity(0.55),
                  borderRadius: BorderRadius.circular(12),
                  border: Border.all(color: context.palette.divider),
                ),
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Icon(Icons.info_outline_rounded, size: 22, color: context.palette.primary.withOpacity(0.95)),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Text(
                        l.tr('groups_hint'),
                        style: TextStyle(
                          fontSize: 13,
                          height: 1.4,
                          color: context.palette.onSurfaceVariant.withOpacity(0.95),
                        ),
                      ),
                    ),
                  ],
                ),
              ),
            ),
            Divider(height: 1, color: context.palette.divider),
            Expanded(
              child: _groups.isEmpty
                  ? Center(
                      child: Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 32),
                        child: Column(
                          mainAxisAlignment: MainAxisAlignment.center,
                          children: [
                            Icon(Icons.group_off, size: 64, color: context.palette.onSurfaceVariant.withOpacity(0.45)),
                            const SizedBox(height: 16),
                            Text(
                              l.tr('no_groups'),
                              textAlign: TextAlign.center,
                              style: TextStyle(color: context.palette.onSurfaceVariant.withOpacity(0.95), fontSize: 14, height: 1.35),
                            ),
                            const SizedBox(height: 20),
                            FilledButton.tonal(
                              onPressed: widget.ble.isConnected ? _showAddSheet : null,
                              style: FilledButton.styleFrom(
                                padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 12),
                                shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
                              ),
                              child: Row(
                                mainAxisSize: MainAxisSize.min,
                                children: [
                                  const Icon(Icons.add, size: 20),
                                  const SizedBox(width: 8),
                                  Text(l.tr('add_group'), style: const TextStyle(fontWeight: FontWeight.w600)),
                                ],
                              ),
                            ),
                          ],
                        ),
                      ),
                    )
                  : ListView.separated(
                      padding: const EdgeInsets.only(bottom: 88),
                      itemCount: _groups.length,
                      separatorBuilder: (_, __) => Divider(height: 1, indent: 72, color: context.palette.divider),
                      itemBuilder: (_, i) {
                        final gid = _groups[i];
                        return ListTile(
                          contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 4),
                          leading: CircleAvatar(
                            backgroundColor: context.palette.primary.withOpacity(0.15),
                            child: Text(
                              '$gid',
                              style: TextStyle(color: context.palette.primary, fontWeight: FontWeight.w600, fontSize: 12),
                            ),
                          ),
                          title: Text(
                            '${l.tr('group')} $gid',
                            style: TextStyle(color: context.palette.onSurface, fontWeight: FontWeight.w500),
                          ),
                          trailing: IconButton(
                            icon: Icon(Icons.remove_circle_outline, color: context.palette.error),
                            tooltip: l.tr('delete'),
                            onPressed: () => _removeGroup(gid),
                          ),
                        );
                      },
                    ),
            ),
          ],
        ),
      ),
    );

    final fab = FloatingActionButton(
      backgroundColor: context.palette.primary,
      foregroundColor: Colors.white,
      onPressed: widget.ble.isConnected ? _showAddSheet : null,
      tooltip: l.tr('add_group'),
      child: const Icon(Icons.add),
    );

    if (widget.embedded) {
      return Scaffold(
        backgroundColor: context.palette.surface,
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
                ? const SizedBox(
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
