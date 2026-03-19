import 'package:flutter/material.dart';
import '../app_navigator.dart';
import '../contacts/contacts_service.dart';
import '../l10n/app_localizations.dart';
import '../widgets/mesh_background.dart';
import '../theme/app_theme.dart';

class ContactsScreen extends StatefulWidget {
  final List<String> neighbors;
  const ContactsScreen({super.key, this.neighbors = const []});
  @override
  State<ContactsScreen> createState() => _ContactsScreenState();
}

class _ContactsScreenState extends State<ContactsScreen> {
  List<Contact> _contacts = [];
  bool _loading = true;

  @override
  void initState() { super.initState(); _load(); }

  Future<void> _load() async {
    setState(() => _loading = true);
    final list = await ContactsService.load();
    if (mounted) setState(() { _contacts = list; _loading = false; });
  }

  void _showAddDialog({String? prefilledId}) {
    final raw = prefilledId?.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase() ?? '';
    final idC = TextEditingController(text: raw.length > 8 ? raw.substring(0, 8) : raw);
    final nickC = TextEditingController();
    final l = context.l10n;
    showAppDialog(context: context, builder: (ctx) => AlertDialog(
      backgroundColor: AppColors.card,
      title: Text(prefilledId != null ? l.tr('edit_contact') : l.tr('add_contact'), style: const TextStyle(color: AppColors.onSurface)),
      content: Column(mainAxisSize: MainAxisSize.min, children: [
        TextField(controller: idC, style: const TextStyle(color: AppColors.onSurface), decoration: InputDecoration(labelText: l.tr('node_id_hex'), hintText: 'A1B2C3D4'), maxLength: 8, enabled: prefilledId == null),
        const SizedBox(height: 12),
        TextField(controller: nickC, style: const TextStyle(color: AppColors.onSurface), decoration: InputDecoration(labelText: l.tr('contact_nickname'), hintText: l.tr('contact_name_hint')), maxLength: 16),
      ]),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx), child: Text(l.tr('cancel'))),
        ElevatedButton(
          style: ElevatedButton.styleFrom(backgroundColor: AppColors.primary, foregroundColor: AppColors.card),
          onPressed: () async {
            final id = idC.text.trim().toUpperCase();
            if (id.length != 8) { ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(l.tr('invalid_hex')))); return; }
            Navigator.pop(ctx);
            await ContactsService.add(Contact(id: id, nickname: nickC.text.trim()));
            await _load();
          },
          child: Text(l.tr('save')),
        ),
      ],
    )).then((_) { idC.dispose(); nickC.dispose(); });
  }

  void _showEditDialog(Contact c) {
    final nickC = TextEditingController(text: c.nickname);
    final l = context.l10n;
    showDialog(context: context, builder: (ctx) => AlertDialog(
      backgroundColor: AppColors.card,
      title: Text(l.tr('edit_contact'), style: const TextStyle(color: AppColors.onSurface)),
      content: TextField(controller: nickC, style: const TextStyle(color: AppColors.onSurface), decoration: InputDecoration(labelText: l.tr('contact_nickname')), maxLength: 16, autofocus: true),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx), child: Text(l.tr('cancel'))),
        ElevatedButton(
          style: ElevatedButton.styleFrom(backgroundColor: AppColors.primary, foregroundColor: AppColors.card),
          onPressed: () async { Navigator.pop(ctx); await ContactsService.add(Contact(id: c.id, nickname: nickC.text.trim())); await _load(); },
          child: Text(l.tr('ok')),
        ),
      ],
    )).then((_) => nickC.dispose());
  }

  Future<void> _delete(Contact c) async {
    final l = context.l10n;
    final ok = await showAppDialog<bool>(context: context, builder: (ctx) => AlertDialog(
      backgroundColor: AppColors.card,
      title: Text(l.tr('delete_contact'), style: const TextStyle(color: AppColors.onSurface)),
      content: Text('${c.nickname.isNotEmpty ? "${c.nickname} " : ""}(${c.id})', style: const TextStyle(color: AppColors.onSurface)),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx, false), child: Text(l.tr('cancel'))),
        ElevatedButton(style: ElevatedButton.styleFrom(backgroundColor: const Color(0xFFD32F2F), foregroundColor: AppColors.card), onPressed: () => Navigator.pop(ctx, true), child: Text(l.tr('delete'))),
      ],
    ));
    if (ok == true) { await ContactsService.remove(c.id); await _load(); }
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    return Scaffold(
      backgroundColor: AppColors.surface,
      appBar: AppBar(title: Text(l.tr('contacts')), leading: IconButton(icon: const Icon(Icons.arrow_back), onPressed: () => Navigator.pop(context))),
      body: MeshBackgroundWrapper(
        child: Material(
          color: Colors.transparent,
          child: _loading ? const Center(child: CircularProgressIndicator(color: AppColors.primary))
          : Column(children: [
            if (widget.neighbors.isNotEmpty)
              Container(
                width: double.infinity,
                padding: const EdgeInsets.all(16),
                color: const Color(0xFFE3F2FD),
                child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                  Text(l.tr('add_from_neighbors'), style: const TextStyle(fontSize: 13, fontWeight: FontWeight.w600, color: AppColors.onSurface)),
                  const SizedBox(height: 8),
                  Wrap(spacing: 8, runSpacing: 4, children: widget.neighbors.map((id) {
                    final shortId = id.length > 8 ? id.substring(0, 8) : id;
                    final existing = _contacts.where((c) => c.id == shortId).firstOrNull;
                    return ActionChip(
                      backgroundColor: AppColors.card,
                      side: const BorderSide(color: Color(0xFFBDBDBD)),
                      label: Text(existing != null && existing.nickname.isNotEmpty ? '${existing.nickname} ($shortId)' : shortId, style: const TextStyle(fontFamily: 'monospace', fontSize: 12, color: AppColors.onSurface)),
                      onPressed: () => _showAddDialog(prefilledId: shortId),
                    );
                  }).toList()),
                ]),
              ),
            Expanded(
              child: _contacts.isEmpty
                ? Center(child: Column(mainAxisAlignment: MainAxisAlignment.center, children: [
                    Icon(Icons.people_outline, size: 64, color: Colors.grey.shade400),
                    const SizedBox(height: 16),
                    Text(l.tr('contacts_empty'), style: const TextStyle(color: AppColors.onSurface, fontSize: 16)),
                    const SizedBox(height: 8),
                    Text(l.tr('contacts_hint'), style: const TextStyle(color: Color(0xFF9E9E9E), fontSize: 13)),
                  ]))
                : ListView.separated(
                    itemCount: _contacts.length,
                    separatorBuilder: (_, __) => const Divider(height: 1, indent: 72, color: Color(0xFFEEEEEE)),
                    itemBuilder: (_, i) {
                      final c = _contacts[i];
                      return ListTile(
                        leading: CircleAvatar(backgroundColor: const Color(0xFFE3F2FD), child: Text((c.nickname.isNotEmpty ? c.nickname[0] : c.id[0]).toUpperCase(), style: const TextStyle(color: AppColors.primary, fontWeight: FontWeight.w600))),
                        title: Text(c.nickname.isNotEmpty ? c.nickname : c.id, style: const TextStyle(color: AppColors.onSurface)),
                        subtitle: Text(c.id, style: const TextStyle(fontFamily: 'monospace', fontSize: 12, color: Color(0xFF9E9E9E))),
                        onTap: () => _showEditDialog(c),
                        onLongPress: () => _delete(c),
                      );
                    },
                  ),
            ),
          ]),
        ),
        ),
      floatingActionButton: FloatingActionButton(backgroundColor: AppColors.primary, foregroundColor: AppColors.card, onPressed: () => _showAddDialog(), tooltip: l.tr('add_contact'), child: const Icon(Icons.add)),
    );
  }
}
