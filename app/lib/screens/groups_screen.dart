import 'dart:async';
import 'package:flutter/material.dart';
import '../app_navigator.dart';
import '../widgets/mesh_background.dart';
import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';

class GroupsScreen extends StatefulWidget {
  final RiftLinkBle ble;
  final List<int> initialGroups;
  const GroupsScreen({super.key, required this.ble, required this.initialGroups});
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
    _groups = List.from(widget.initialGroups);
    _sub = widget.ble.events.listen((evt) { if (evt is RiftLinkGroupsEvent && mounted) setState(() => _groups = List.from(evt.groups)); });
    _refresh();
  }

  @override
  void dispose() { _sub?.cancel(); super.dispose(); }

  Future<void> _refresh() async {
    if (!widget.ble.isConnected) return;
    setState(() => _loading = true);
    await widget.ble.getGroups();
    if (mounted) setState(() => _loading = false);
  }

  void _showAddDialog() {
    final c = TextEditingController();
    final l = context.l10n;
    showAppDialog(context: context, builder: (ctx) => AlertDialog(
      backgroundColor: AppColors.card,
      title: Text(l.tr('add_group'), style: const TextStyle(color: AppColors.onSurface)),
      content: TextField(controller: c, style: const TextStyle(color: AppColors.onSurface), decoration: InputDecoration(labelText: l.tr('group_id_hint'), hintText: '1'), keyboardType: TextInputType.number, autofocus: true),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx), child: Text(l.tr('cancel'))),
        ElevatedButton(
          style: ElevatedButton.styleFrom(backgroundColor: AppColors.primary, foregroundColor: Colors.white),
          onPressed: () async {
            final val = int.tryParse(c.text.trim());
            Navigator.pop(ctx);
            if (val == null || val <= 0 || val > 0xFFFFFFFF) { if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(l.tr('invalid_group_id')))); return; }
            final ok = await widget.ble.addGroup(val);
            if (ok) setState(() => _groups.add(val));
            await _refresh();
            if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('${l.tr('group')} $val ${l.tr('added')}')));
          },
          child: Text(l.tr('add')),
        ),
      ],
    ));
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    return Scaffold(
      backgroundColor: AppColors.surface,
      appBar: AppBar(
        title: Text(l.tr('groups')),
        leading: IconButton(icon: const Icon(Icons.arrow_back), onPressed: () => Navigator.pop(context)),
        actions: [
          IconButton(
            onPressed: _loading ? null : _refresh,
            icon: _loading ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2, color: AppColors.onSurface)) : const Icon(Icons.refresh),
            tooltip: l.tr('refresh'),
          ),
        ],
      ),
      body: Material(
        color: AppColors.surface,
        child: Column(children: [
        Padding(padding: const EdgeInsets.all(16), child: Text(l.tr('groups_hint'), style: const TextStyle(color: AppColors.onSurfaceVariant, fontSize: 14))),
        const Divider(height: 1, color: AppColors.divider),
        Expanded(
          child: _groups.isEmpty
            ? Center(child: Column(mainAxisAlignment: MainAxisAlignment.center, children: [
                Icon(Icons.group_off, size: 64, color: AppColors.onSurfaceVariant),
                const SizedBox(height: 16),
                Text(l.tr('no_groups'), style: const TextStyle(color: AppColors.onSurfaceVariant, fontSize: 14)),
              ]))
            : ListView.separated(
                itemCount: _groups.length,
                separatorBuilder: (_, __) => const Divider(height: 1, indent: 72, color: AppColors.divider),
                itemBuilder: (_, i) {
                  final gid = _groups[i];
                  return ListTile(
                    leading: CircleAvatar(backgroundColor: AppColors.primary.withOpacity(0.2), child: Text('$gid', style: const TextStyle(color: AppColors.primary, fontWeight: FontWeight.w600, fontSize: 13))),
                    title: Text('${l.tr('group')} $gid', style: const TextStyle(color: AppColors.onSurface)),
                    trailing: IconButton(icon: const Icon(Icons.remove_circle_outline, color: AppColors.error), onPressed: () async { await widget.ble.removeGroup(gid); setState(() => _groups.remove(gid)); }),
                  );
                },
              ),
        ),
      ]),
        ),
      floatingActionButton: FloatingActionButton(backgroundColor: AppColors.primary, foregroundColor: Colors.white, onPressed: widget.ble.isConnected ? _showAddDialog : null, tooltip: l.tr('add_group'), child: const Icon(Icons.add)),
    );
  }
}
