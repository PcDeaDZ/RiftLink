import 'package:flutter/material.dart';
import '../app_navigator.dart';
import '../contacts/contacts_service.dart';
import '../l10n/app_localizations.dart';
import '../widgets/mesh_background.dart';
import '../widgets/app_snackbar.dart';
import '../theme/app_theme.dart';
import '../widgets/rift_dialogs.dart';

class ContactsScreen extends StatefulWidget {
  final List<String> neighbors;
  /// Без AppBar и кнопки «назад» — для вкладки в [ContactsGroupsHubScreen].
  final bool embedded;
  const ContactsScreen({super.key, this.neighbors = const [], this.embedded = false});
  @override
  State<ContactsScreen> createState() => _ContactsScreenState();
}

class _ContactsScreenState extends State<ContactsScreen> {
  List<Contact> _contacts = [];
  bool _loading = true;

  String _normalizeId(String raw) =>
      raw.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();

  @override
  void initState() { super.initState(); _load(); }

  Future<void> _load() async {
    setState(() => _loading = true);
    final list = (await ContactsService.load())
        .map((c) => Contact(id: _normalizeId(c.id), nickname: c.nickname))
        .where((c) => c.id.length >= 8)
        .toList();
    list.sort((a, b) {
      final an = a.nickname.trim().toLowerCase();
      final bn = b.nickname.trim().toLowerCase();
      if (an.isNotEmpty && bn.isNotEmpty) return an.compareTo(bn);
      if (an.isNotEmpty) return -1;
      if (bn.isNotEmpty) return 1;
      return a.id.compareTo(b.id);
    });
    if (mounted) setState(() { _contacts = list; _loading = false; });
  }

  void _showAddDialog({String? prefilledId}) {
    final raw = _normalizeId(prefilledId ?? '');
    final existing = raw.isNotEmpty ? _contacts.where((c) => c.id == raw).firstOrNull : null;
    final idC = TextEditingController(text: raw.length > 8 ? raw.substring(0, 8) : raw);
    final nickC = TextEditingController(text: existing?.nickname ?? '');
    final l = context.l10n;
    final p = context.palette;
    showAppDialog(
      context: context,
      builder: (ctx) => RiftDialogFrame(
        maxWidth: 360,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(
              prefilledId != null ? l.tr('edit_contact') : l.tr('add_contact'),
              style: Theme.of(context).textTheme.titleMedium?.copyWith(
                    fontWeight: FontWeight.w700,
                    color: p.onSurface,
                  ),
            ),
            const SizedBox(height: 14),
            TextField(
              controller: idC,
              style: TextStyle(color: p.onSurface),
              decoration: InputDecoration(labelText: l.tr('node_id_hex'), hintText: 'A1B2C3D4'),
              maxLength: 8,
              enabled: prefilledId == null,
            ),
            const SizedBox(height: 10),
            TextField(
              controller: nickC,
              style: TextStyle(color: p.onSurface),
              decoration: InputDecoration(labelText: l.tr('contact_nickname'), hintText: l.tr('contact_name_hint')),
              maxLength: 16,
            ),
            const SizedBox(height: 8),
            Row(
              mainAxisAlignment: MainAxisAlignment.end,
              children: [
                TextButton(
                  style: TextButton.styleFrom(
                    foregroundColor: p.onSurfaceVariant,
                    minimumSize: Size.zero,
                    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                  ),
                  onPressed: () => Navigator.pop(ctx),
                  child: Text(l.tr('cancel')),
                ),
                const SizedBox(width: 4),
                TextButton(
                  style: TextButton.styleFrom(
                    foregroundColor: p.primary,
                    minimumSize: Size.zero,
                    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                    textStyle: const TextStyle(fontWeight: FontWeight.w600),
                  ),
                  onPressed: () async {
                    final id = _normalizeId(idC.text.trim());
                    if (id.length != 8) {
                      showAppSnackBar(context, l.tr('invalid_hex'), kind: AppSnackKind.error);
                      return;
                    }
                    Navigator.pop(ctx);
                    await ContactsService.add(Contact(id: id, nickname: nickC.text.trim()));
                    await _load();
                  },
                  child: Text(l.tr('save')),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }

  void _showEditDialog(Contact c) {
    final nickC = TextEditingController(text: c.nickname);
    final l = context.l10n;
    final p = context.palette;
    showAppDialog(
      context: context,
      builder: (ctx) => RiftDialogFrame(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(
              l.tr('edit_contact'),
              style: Theme.of(context).textTheme.titleMedium?.copyWith(
                    fontWeight: FontWeight.w700,
                    color: p.onSurface,
                  ),
            ),
            const SizedBox(height: 14),
            TextField(
              controller: nickC,
              autofocus: true,
              style: TextStyle(color: p.onSurface),
              decoration: InputDecoration(labelText: l.tr('contact_nickname')),
              maxLength: 16,
            ),
            const SizedBox(height: 8),
            Row(
              mainAxisAlignment: MainAxisAlignment.end,
              children: [
                TextButton(
                  style: TextButton.styleFrom(foregroundColor: p.onSurfaceVariant, minimumSize: Size.zero, tapTargetSize: MaterialTapTargetSize.shrinkWrap),
                  onPressed: () => Navigator.pop(ctx),
                  child: Text(l.tr('cancel')),
                ),
                TextButton(
                  style: TextButton.styleFrom(
                    foregroundColor: p.primary,
                    minimumSize: Size.zero,
                    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                    textStyle: const TextStyle(fontWeight: FontWeight.w600),
                  ),
                  onPressed: () async {
                    Navigator.pop(ctx);
                    await ContactsService.add(Contact(id: _normalizeId(c.id), nickname: nickC.text.trim()));
                    await _load();
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

  Future<void> _delete(Contact c) async {
    final l = context.l10n;
    final ok = await showRiftConfirmDialog(
      context: context,
      title: l.tr('delete_contact'),
      message: '${c.nickname.isNotEmpty ? "${c.nickname} " : ""}(${c.id})',
      cancelText: l.tr('cancel'),
      confirmText: l.tr('delete'),
      danger: true,
      icon: Icons.person_remove_outlined,
    );
    if (ok == true) { await ContactsService.remove(c.id); await _load(); }
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final inner = Material(
        color: Colors.transparent,
        child: _loading
            ? Center(child: CircularProgressIndicator(color: context.palette.primary))
            : Column(
                children: [
                  if (widget.neighbors.isNotEmpty)
                    Padding(
                      padding: EdgeInsets.fromLTRB(widget.embedded ? 12 : 0, widget.embedded ? 4 : 0, widget.embedded ? 12 : 0, widget.embedded ? 10 : 0),
                      child: Container(
                        width: double.infinity,
                        padding: const EdgeInsets.fromLTRB(16, 14, 16, 14),
                        decoration: BoxDecoration(
                          color: widget.embedded ? context.palette.card : context.palette.primary.withOpacity(0.06),
                          borderRadius: BorderRadius.circular(widget.embedded ? 18 : 0),
                          border: widget.embedded
                              ? Border.all(color: context.palette.divider.withOpacity(0.7))
                              : null,
                          boxShadow: widget.embedded
                              ? [
                                  BoxShadow(
                                    color: context.palette.primary.withOpacity(0.06),
                                    blurRadius: 18,
                                    offset: const Offset(0, 6),
                                  ),
                                ]
                              : null,
                        ),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Row(
                              children: [
                                Icon(Icons.hub_outlined, size: 20, color: context.palette.primary),
                                const SizedBox(width: 8),
                                Expanded(
                                  child: Text(
                                    l.tr('add_from_neighbors'),
                                    style: TextStyle(fontSize: 14, fontWeight: FontWeight.w700, color: context.palette.onSurface),
                                  ),
                                ),
                              ],
                            ),
                            const SizedBox(height: 12),
                            Wrap(
                              spacing: 8,
                              runSpacing: 8,
                              children: widget.neighbors
                                  .map(_normalizeId)
                                  .where((id) => id.isNotEmpty)
                                  .map((id) {
                                final shortId = id.length > 8 ? id.substring(0, 8) : id;
                                final existing = _contacts.where((c) => c.id == shortId).firstOrNull;
                                return Material(
                                  color: context.palette.surfaceVariant.withOpacity(0.75),
                                  borderRadius: BorderRadius.circular(12),
                                  child: InkWell(
                                    borderRadius: BorderRadius.circular(12),
                                    onTap: () => _showAddDialog(prefilledId: shortId),
                                    child: Padding(
                                      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                                      child: Text(
                                        existing != null && existing.nickname.isNotEmpty ? '${existing.nickname} · $shortId' : shortId,
                                        style: TextStyle(fontFamily: 'monospace', fontSize: 12.5, fontWeight: FontWeight.w500, color: context.palette.onSurface),
                                      ),
                                    ),
                                  ),
                                );
                              }).toList(),
                            ),
                          ],
                        ),
                      ),
                    ),
                  Expanded(
                    child: _contacts.isEmpty
                        ? Center(
                            child: Column(
                              mainAxisAlignment: MainAxisAlignment.center,
                              children: [
                                Icon(Icons.people_outline, size: 64, color: context.palette.onSurfaceVariant.withOpacity(0.45)),
                                const SizedBox(height: 16),
                                Text(l.tr('contacts_empty'), style: TextStyle(color: context.palette.onSurface, fontSize: 16)),
                                const SizedBox(height: 8),
                                Padding(
                                  padding: const EdgeInsets.symmetric(horizontal: 32),
                                  child: Text(
                                    l.tr('contacts_hint'),
                                    textAlign: TextAlign.center,
                                    style: TextStyle(color: context.palette.onSurfaceVariant.withOpacity(0.9), fontSize: 13),
                                  ),
                                ),
                              ],
                            ),
                          )
                        : ListView.builder(
                            padding: EdgeInsets.fromLTRB(widget.embedded ? 12 : 0, 4, widget.embedded ? 12 : 0, 88),
                            itemCount: _contacts.length,
                            itemBuilder: (_, i) {
                              final c = _contacts[i];
                              return Padding(
                                padding: const EdgeInsets.only(bottom: 10),
                                child: Material(
                                  color: context.palette.card,
                                  elevation: 0,
                                  shape: RoundedRectangleBorder(
                                    borderRadius: BorderRadius.circular(18),
                                    side: BorderSide(color: context.palette.divider.withOpacity(0.65)),
                                  ),
                                  clipBehavior: Clip.antiAlias,
                                  child: InkWell(
                                    onTap: () => _showEditDialog(c),
                                    onLongPress: () => _delete(c),
                                    child: Padding(
                                      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
                                      child: Row(
                                        children: [
                                          CircleAvatar(
                                            radius: 22,
                                            backgroundColor: context.palette.primary.withOpacity(0.14),
                                            child: Text(
                                              (c.nickname.isNotEmpty
                                                      ? c.nickname[0]
                                                      : (c.id.isNotEmpty ? c.id[0] : '?'))
                                                  .toUpperCase(),
                                              style: TextStyle(color: context.palette.primary, fontWeight: FontWeight.w700, fontSize: 18),
                                            ),
                                          ),
                                          const SizedBox(width: 14),
                                          Expanded(
                                            child: Column(
                                              crossAxisAlignment: CrossAxisAlignment.start,
                                              children: [
                                                Text(
                                                  c.nickname.isNotEmpty ? c.nickname : c.id,
                                                  style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: context.palette.onSurface),
                                                ),
                                                const SizedBox(height: 4),
                                                Text(
                                                  c.id,
                                                  style: TextStyle(
                                                    fontFamily: 'monospace',
                                                    fontSize: 12.5,
                                                    letterSpacing: 0.2,
                                                    color: context.palette.onSurfaceVariant.withOpacity(0.95),
                                                  ),
                                                ),
                                              ],
                                            ),
                                          ),
                                          Icon(Icons.chevron_right_rounded, color: context.palette.onSurfaceVariant.withOpacity(0.4)),
                                        ],
                                      ),
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
      heroTag: widget.embedded ? 'contacts_embedded_fab' : 'contacts_screen_fab',
      backgroundColor: context.palette.primary,
      foregroundColor: Colors.white,
      onPressed: () => _showAddDialog(),
      tooltip: l.tr('add_contact'),
      child: const Icon(Icons.person_add_alt_1),
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
        title: Text(l.tr('contacts')),
        leading: IconButton(icon: const Icon(Icons.arrow_back), onPressed: () => Navigator.pop(context)),
      ),
      body: body,
      floatingActionButton: fab,
    );
  }
}
