import 'package:flutter/material.dart';

import '../app_navigator.dart';
import '../contacts/contacts_service.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../theme/design_tokens.dart';
import '../widgets/app_primitives.dart';
import '../widgets/app_snackbar.dart';
import '../widgets/mesh_background.dart';
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

  static const double _fabClearance = AppSpacing.xxl + 56 + AppSpacing.sm;

  EdgeInsets get _pageHorizontal =>
      widget.embedded ? const EdgeInsets.symmetric(horizontal: AppSpacing.md) : const EdgeInsets.symmetric(horizontal: AppSpacing.lg);

  String _normalizeId(String raw) =>
      raw.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();

  @override
  void initState() {
    super.initState();
    _load();
  }

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
    if (mounted) setState(() {
      _contacts = list;
      _loading = false;
    });
  }

  Widget _sectionTitleRow(BuildContext context, {required IconData icon, required String title}) {
    final p = context.palette;
    return Row(
      children: [
        Icon(icon, size: 20, color: p.primary),
        const SizedBox(width: AppSpacing.sm),
        Expanded(
          child: Text(
            title,
            style: AppTypography.labelBase().copyWith(
              fontSize: 14,
              fontWeight: FontWeight.w700,
              color: p.onSurface,
              height: 1.2,
            ),
          ),
        ),
      ],
    );
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
                    height: 1.25,
                  ),
            ),
            const SizedBox(height: AppSpacing.md + 2),
            TextField(
              controller: idC,
              style: TextStyle(color: p.onSurface, fontSize: AppTypography.bodySize),
              decoration: InputDecoration(labelText: l.tr('node_id_hex'), hintText: 'A1B2C3D4'),
              maxLength: 8,
              enabled: prefilledId == null,
            ),
            const SizedBox(height: AppSpacing.sm + 2),
            TextField(
              controller: nickC,
              style: TextStyle(color: p.onSurface, fontSize: AppTypography.bodySize),
              decoration: InputDecoration(
                labelText: l.tr('contact_nickname'),
                hintText: l.tr('contact_name_hint'),
              ),
              maxLength: 16,
            ),
            const SizedBox(height: AppSpacing.sm),
            Row(
              mainAxisAlignment: MainAxisAlignment.end,
              children: [
                TextButton(
                  style: TextButton.styleFrom(
                    foregroundColor: p.onSurfaceVariant,
                    padding: const EdgeInsets.symmetric(horizontal: AppSpacing.lg, vertical: AppSpacing.sm),
                    minimumSize: Size.zero,
                    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                  ),
                  onPressed: () => Navigator.pop(ctx),
                  child: Text(l.tr('cancel')),
                ),
                const SizedBox(width: AppSpacing.xs),
                FilledButton(
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
        maxWidth: 360,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(
              l.tr('edit_contact'),
              style: Theme.of(context).textTheme.titleMedium?.copyWith(
                    fontWeight: FontWeight.w700,
                    color: p.onSurface,
                    height: 1.25,
                  ),
            ),
            const SizedBox(height: AppSpacing.md + 2),
            TextField(
              controller: nickC,
              autofocus: true,
              style: TextStyle(color: p.onSurface, fontSize: AppTypography.bodySize),
              decoration: InputDecoration(labelText: l.tr('contact_nickname')),
              maxLength: 16,
            ),
            const SizedBox(height: AppSpacing.sm),
            Row(
              mainAxisAlignment: MainAxisAlignment.end,
              children: [
                TextButton(
                  style: TextButton.styleFrom(
                    foregroundColor: p.onSurfaceVariant,
                    padding: const EdgeInsets.symmetric(horizontal: AppSpacing.lg, vertical: AppSpacing.sm),
                    minimumSize: Size.zero,
                    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                  ),
                  onPressed: () => Navigator.pop(ctx),
                  child: Text(l.tr('cancel')),
                ),
                const SizedBox(width: AppSpacing.xs),
                FilledButton(
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
    if (ok == true) {
      await ContactsService.remove(c.id);
      await _load();
    }
  }

  Widget _buildNeighborChips() {
    final p = context.palette;
    return Wrap(
      spacing: AppSpacing.sm,
      runSpacing: AppSpacing.sm,
      children: widget.neighbors
          .map(_normalizeId)
          .where((id) => id.isNotEmpty)
          .map((id) {
        final shortId = id.length > 8 ? id.substring(0, 8) : id;
        final existing = _contacts.where((c) => c.id == shortId).firstOrNull;
        return Material(
          color: p.surfaceVariant.withOpacity(0.85),
          borderRadius: BorderRadius.circular(AppRadius.md),
          child: InkWell(
            borderRadius: BorderRadius.circular(AppRadius.md),
            onTap: () => _showAddDialog(prefilledId: shortId),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: AppSpacing.md, vertical: AppSpacing.sm),
              child: Text(
                existing != null && existing.nickname.isNotEmpty ? '${existing.nickname} · $shortId' : shortId,
                style: AppTypography.chipBase().copyWith(
                  fontFamily: 'monospace',
                  fontWeight: FontWeight.w500,
                  fontSize: 12.5,
                  color: p.onSurface,
                ),
              ),
            ),
          ),
        );
      }).toList(),
    );
  }

  Widget _buildEmptyState() {
    final p = context.palette;
    final l = context.l10n;
    return Center(
      child: Padding(
        padding: _pageHorizontal,
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(Icons.people_outline_rounded, size: 64, color: p.onSurfaceVariant.withOpacity(0.45)),
            const SizedBox(height: AppSpacing.lg),
            Text(
              l.tr('contacts_empty'),
              textAlign: TextAlign.center,
              style: AppTypography.bodyBase().copyWith(
                color: p.onSurface,
                fontWeight: FontWeight.w600,
              ),
            ),
            const SizedBox(height: AppSpacing.sm),
            Text(
              l.tr('contacts_hint'),
              textAlign: TextAlign.center,
              style: AppTypography.labelBase().copyWith(
                color: p.onSurfaceVariant.withOpacity(0.95),
                height: AppTypography.bodyHeight,
                fontWeight: FontWeight.w400,
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildContactTile(Contact c) {
    final p = context.palette;
    return AppSectionCard(
      margin: const EdgeInsets.only(bottom: AppSpacing.sm + 2),
      padding: EdgeInsets.zero,
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          borderRadius: BorderRadius.circular(AppRadius.card),
          onTap: () => _showEditDialog(c),
          onLongPress: () => _delete(c),
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: AppSpacing.lg, vertical: AppSpacing.md + 2),
            child: Row(
              children: [
                CircleAvatar(
                  radius: 22,
                  backgroundColor: p.primary.withOpacity(0.14),
                  child: Text(
                    (c.nickname.isNotEmpty ? c.nickname[0] : (c.id.isNotEmpty ? c.id[0] : '?')).toUpperCase(),
                    style: TextStyle(color: p.primary, fontWeight: FontWeight.w700, fontSize: 18),
                  ),
                ),
                const SizedBox(width: AppSpacing.md + 2),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        c.nickname.isNotEmpty ? c.nickname : c.id,
                        style: AppTypography.bodyBase().copyWith(
                          fontWeight: FontWeight.w600,
                          fontSize: 16,
                          color: p.onSurface,
                        ),
                      ),
                      const SizedBox(height: AppSpacing.xs),
                      Text(
                        c.id,
                        style: TextStyle(
                          fontFamily: 'monospace',
                          fontSize: 12.5,
                          letterSpacing: 0.2,
                          color: p.onSurfaceVariant.withOpacity(0.95),
                        ),
                      ),
                    ],
                  ),
                ),
                Icon(Icons.chevron_right_rounded, color: p.onSurfaceVariant.withOpacity(0.4)),
              ],
            ),
          ),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final p = context.palette;

    final content = Material(
      color: Colors.transparent,
      child: _loading
          ? Center(child: CircularProgressIndicator(color: p.primary))
          : Column(
              children: [
                if (widget.neighbors.isNotEmpty)
                  Padding(
                    padding: EdgeInsets.fromLTRB(
                      widget.embedded ? AppSpacing.md : AppSpacing.lg,
                      widget.embedded ? AppSpacing.xs : AppSpacing.sm,
                      widget.embedded ? AppSpacing.md : AppSpacing.lg,
                      AppSpacing.sm + 2,
                    ),
                    child: AppSectionCard(
                      margin: EdgeInsets.zero,
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          _sectionTitleRow(context, icon: Icons.hub_outlined, title: l.tr('add_from_neighbors')),
                          const SizedBox(height: AppSpacing.md),
                          _buildNeighborChips(),
                        ],
                      ),
                    ),
                  ),
                Expanded(
                  child: _contacts.isEmpty
                      ? _buildEmptyState()
                      : ListView.builder(
                          padding: _pageHorizontal.copyWith(
                            top: AppSpacing.xs,
                            bottom: _fabClearance,
                          ),
                          itemCount: _contacts.length,
                          itemBuilder: (_, i) => _buildContactTile(_contacts[i]),
                        ),
                ),
              ],
            ),
    );

    final body = widget.embedded ? content : MeshBackgroundWrapper(child: content);

    final fab = FloatingActionButton(
      heroTag: widget.embedded ? 'contacts_embedded_fab' : 'contacts_screen_fab',
      backgroundColor: p.primary,
      foregroundColor: Colors.white,
      elevation: 2,
      onPressed: () => _showAddDialog(),
      tooltip: l.tr('add_contact'),
      child: const Icon(Icons.person_add_alt_1_rounded),
    );

    if (widget.embedded) {
      return Scaffold(
        backgroundColor: Colors.transparent,
        body: body,
        floatingActionButton: fab,
        floatingActionButtonLocation: FloatingActionButtonLocation.endFloat,
      );
    }

    return Scaffold(
      backgroundColor: p.surface,
      appBar: AppBar(
        title: Text(l.tr('contacts')),
        leading: IconButton(
          icon: const Icon(Icons.arrow_back_rounded),
          onPressed: () => Navigator.pop(context),
        ),
      ),
      body: body,
      floatingActionButton: fab,
      floatingActionButtonLocation: FloatingActionButtonLocation.endFloat,
    );
  }
}
