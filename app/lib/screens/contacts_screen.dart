import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app_navigator.dart';
import '../ble/riftlink_ble.dart';
import '../ble/riftlink_ble_scope.dart';
import '../chat/chat_models.dart';
import '../chat/chat_repository.dart';
import '../contacts/contacts_service.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../theme/design_tokens.dart';
import '../widgets/app_primitives.dart';
import '../widgets/app_snackbar.dart';
import '../widgets/mesh_background.dart';
import '../widgets/rift_dialogs.dart';
import 'chat_screen.dart';

class ContactsScreen extends StatefulWidget {
  final List<String> neighbors;
  final RiftLinkBle? ble;

  /// Без AppBar и кнопки «назад» — для вкладки в [ContactsGroupsHubScreen].
  final bool embedded;
  const ContactsScreen({
    super.key,
    this.neighbors = const [],
    this.ble,
    this.embedded = false,
  });
  @override
  State<ContactsScreen> createState() => _ContactsScreenState();
}

class _ContactsScreenState extends State<ContactsScreen> {
  final ChatRepository _chatRepo = ChatRepository.instance;
  final TextEditingController _searchController = TextEditingController();
  final FocusNode _searchFocusNode = FocusNode();
  List<Contact> _contacts = [];
  List<String> _neighbors = const [];
  StreamSubscription<RiftLinkEvent>? _bleSub;
  bool _loading = true;
  String _searchQuery = '';
  bool _searchMode = false;

  static const double _fabClearance = AppSpacing.xxl + 56 + AppSpacing.sm;

  EdgeInsets get _pageHorizontal =>
      widget.embedded ? const EdgeInsets.symmetric(horizontal: AppSpacing.md) : const EdgeInsets.symmetric(horizontal: AppSpacing.lg);

  String _normalizeId(String raw) =>
      raw.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();

  List<String> _normalizeNeighborList(Iterable<String> raw) {
    final out = <String>{};
    for (final item in raw) {
      final id = _normalizeId(item);
      if (id.length == 16) out.add(id);
    }
    final list = out.toList()..sort();
    return list;
  }

  void _reconcileNeighborsFromBleLastInfo() {
    final ble = widget.ble;
    if (ble == null) return;
    final li = ble.lastInfo;
    if (li == null || li.neighbors.isEmpty) return;
    if (_neighbors.isNotEmpty) return;
    _setNeighbors(li.neighbors);
  }

  void _setNeighbors(Iterable<String> raw) {
    final normalized = _normalizeNeighborList(raw);
    if (!mounted) {
      _neighbors = normalized;
      return;
    }
    if (_neighbors.length == normalized.length) {
      var same = true;
      for (var i = 0; i < _neighbors.length; i++) {
        if (_neighbors[i] != normalized[i]) {
          same = false;
          break;
        }
      }
      if (same) return;
    }
    setState(() => _neighbors = normalized);
  }

  void _bindBleStream() {
    _bleSub?.cancel();
    final ble = widget.ble;
    if (ble == null) return;
    final li = ble.lastInfo;
    if (li != null) _setNeighbors(li.neighbors);
    ble.getInfo();
    _bleSub = ble.events.listen((evt) {
      if (!mounted) return;
      if (evt is RiftLinkInfoEvent) {
        _setNeighbors(evt.neighbors);
      }
    });
    unawaited(Future<void>.delayed(const Duration(milliseconds: 400), () {
      if (!mounted) return;
      _reconcileNeighborsFromBleLastInfo();
    }));
  }

  @override
  void initState() {
    super.initState();
    _neighbors = _normalizeNeighborList(widget.neighbors);
    _bindBleStream();
    _load();
  }

  @override
  void didUpdateWidget(covariant ContactsScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.ble != widget.ble) {
      _bindBleStream();
      return;
    }
    if (oldWidget.neighbors != widget.neighbors && widget.ble == null) {
      _setNeighbors(widget.neighbors);
    }
  }

  @override
  void dispose() {
    _bleSub?.cancel();
    _searchController.dispose();
    _searchFocusNode.dispose();
    super.dispose();
  }

  void _toggleSearch() {
    setState(() {
      _searchMode = !_searchMode;
      if (!_searchMode) {
        _searchFocusNode.unfocus();
        _searchController.clear();
        _searchQuery = '';
      } else {
        _searchFocusNode.requestFocus();
      }
    });
  }

  Future<void> _load() async {
    setState(() => _loading = true);
    final list = (await ContactsService.load())
        .map((c) => Contact(id: _normalizeId(c.id), nickname: c.nickname, legacy: c.legacy))
        .where((c) => c.id.length == 16 || c.id.length == 8)
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
    final idC = TextEditingController(text: raw);
    final nickC = TextEditingController(text: existing?.nickname ?? '');
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
                          Icon(Icons.person_add_alt_1_rounded, color: p.primary, size: 22),
                          const SizedBox(width: AppSpacing.sm + 2),
                          Expanded(
                            child: Text(
                              prefilledId != null ? l.tr('edit_contact') : l.tr('add_contact'),
                              style: AppTypography.screenTitleBase().copyWith(
                                fontSize: 17,
                                color: p.onSurface,
                              ),
                            ),
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
                      const SizedBox(height: AppSpacing.md),
                      TextField(
                        controller: idC,
                        autofocus: prefilledId == null,
                        style: TextStyle(color: p.onSurface, fontSize: 16),
                        maxLength: 16,
                        enabled: prefilledId == null,
                        decoration: InputDecoration(
                          labelText: l.tr('node_id_hex'),
                          hintText: 'A1B2C3D4E5F60708',
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
                      const SizedBox(height: AppSpacing.md),
                      TextField(
                        controller: nickC,
                        autofocus: prefilledId != null,
                        style: TextStyle(color: p.onSurface, fontSize: 16),
                        maxLength: 16,
                        decoration: InputDecoration(
                          labelText: l.tr('contact_nickname'),
                          hintText: l.tr('contact_name_hint'),
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
                          final id = _normalizeId(idC.text.trim());
                          if (id.length != 16) {
                            showAppSnackBar(context, l.tr('invalid_hex'), kind: AppSnackKind.error);
                            return;
                          }
                          Navigator.pop(ctx);
                          await ContactsService.add(Contact(id: id, nickname: nickC.text.trim()));
                          await _load();
                        },
                        child: Text(
                          l.tr('save'),
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

  Future<void> _openDirectChat(Contact c) async {
    final peerId = _normalizeId(c.id);
    if (peerId.length != 16) return;
    final conversationId = ChatRepository.directConversationId(peerId);
    if (!mounted) return;
    final ble = RiftLinkBleScope.of(context);
    await appPush(
      context,
      ChatScreen(
        ble: ble,
        conversationId: null,
        initialPeerId: peerId,
      ),
    );
  }

  Future<void> _copyContactNodeId(Contact c) async {
    final nodeId = _normalizeId(c.id);
    if (nodeId.isEmpty) return;
    await Clipboard.setData(ClipboardData(text: nodeId));
    if (!mounted) return;
    showAppSnackBar(context, context.l10n.tr('copied'));
  }

  bool _isKnownContactId(String fullId) {
    for (final c in _contacts) {
      final known = _normalizeId(c.id);
      if (known.isEmpty) continue;
      if (known.length == 16 && known == fullId) return true;
      if (known.length == 8 && fullId.startsWith(known)) return true;
    }
    return false;
  }

  List<String> _neighborSuggestions() {
    final out = <String>{};
    for (final raw in _neighbors) {
      final id = _normalizeId(raw);
      if (id.length != 16) continue;
      if (_isKnownContactId(id)) continue;
      out.add(id);
    }
    final list = out.toList()..sort();
    return list;
  }

  List<Contact> _filteredContacts() {
    final q = _searchQuery.trim().toLowerCase();
    if (q.isEmpty) return _contacts;
    return _contacts.where((c) {
      final nick = c.nickname.trim().toLowerCase();
      final id = _normalizeId(c.id).toLowerCase();
      return nick.contains(q) || id.contains(q);
    }).toList();
  }

  Widget _buildNeighborChips(List<String> suggestions) {
    final p = context.palette;
    return Wrap(
      spacing: AppSpacing.sm,
      runSpacing: AppSpacing.sm,
      children: suggestions.map((id) {
        return Material(
          color: p.surfaceVariant.withOpacity(0.85),
          borderRadius: BorderRadius.circular(AppRadius.md),
          child: InkWell(
            borderRadius: BorderRadius.circular(AppRadius.md),
            onTap: () => _showAddDialog(prefilledId: id),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: AppSpacing.md, vertical: AppSpacing.sm),
              child: Text(
                id,
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
            TweenAnimationBuilder<double>(
              tween: Tween(begin: 0.0, end: 1.0),
              duration: const Duration(milliseconds: 600),
              curve: Curves.easeOut,
              builder: (_, opacity, child) => Opacity(opacity: opacity, child: child),
              child: Icon(Icons.people_outline_rounded, size: 72, color: p.onSurfaceVariant.withOpacity(0.38)),
            ),
            const SizedBox(height: AppSpacing.lg),
            Text(
              l.tr('contacts_empty'),
              textAlign: TextAlign.center,
              style: AppTypography.bodyBase().copyWith(
                color: p.onSurface,
                fontWeight: FontWeight.w600,
                fontSize: 16,
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
            const SizedBox(height: AppSpacing.xl),
            FilledButton.icon(
              onPressed: () => _showAddDialog(),
              icon: const Icon(Icons.person_add_alt_1_rounded, size: 18),
              label: Text(
                l.tr('add_contact'),
                style: const TextStyle(fontWeight: FontWeight.w600),
              ),
              style: FilledButton.styleFrom(
                backgroundColor: p.primary,
                foregroundColor: Colors.white,
                padding: const EdgeInsets.symmetric(horizontal: AppSpacing.xl, vertical: AppSpacing.md),
                shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(AppRadius.md)),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildContactTile(Contact c) {
    final p = context.palette;
    final l = context.l10n;
    return Dismissible(
      key: ValueKey('contact-${c.id}'),
      direction: DismissDirection.endToStart,
      background: Container(
        alignment: Alignment.centerRight,
        padding: const EdgeInsets.symmetric(horizontal: AppSpacing.lg),
        decoration: BoxDecoration(
          color: p.error.withOpacity(0.12),
          borderRadius: BorderRadius.circular(AppRadius.card),
          border: Border.all(color: p.error.withOpacity(0.45)),
        ),
        child: Icon(Icons.delete_outline_rounded, color: p.error),
      ),
      confirmDismiss: (_) => showRiftConfirmDialog(
        context: context,
        title: l.tr('delete_contact'),
        message: l.tr('delete_contact_confirm', {
          'name': c.nickname.isNotEmpty ? c.nickname : c.id,
        }),
        cancelText: l.tr('cancel'),
        confirmText: l.tr('delete'),
        danger: true,
        icon: Icons.delete_outline_rounded,
      ),
      onDismissed: (_) async {
        await ContactsService.remove(c.id);
        _load();
      },
      child: AppSectionCard(
        margin: const EdgeInsets.only(bottom: AppSpacing.sm),
        padding: EdgeInsets.zero,
        child: Material(
          color: Colors.transparent,
          child: InkWell(
            borderRadius: BorderRadius.circular(AppRadius.card),
            onTap: () => _openDirectChat(c),
            onLongPress: () => _copyContactNodeId(c),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: AppSpacing.lg, vertical: AppSpacing.md + 2),
              child: Row(
                children: [
                  CircleAvatar(
                    radius: 20,
                    backgroundColor: p.primary.withOpacity(0.13),
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
                  IconButton(
                    tooltip: l.tr('edit_contact'),
                    icon: Icon(Icons.edit_outlined, color: p.onSurfaceVariant.withOpacity(0.8)),
                    onPressed: () => _showEditDialog(c),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildSearchField() {
    final p = context.palette;
    final l = context.l10n;
    return Padding(
      padding: EdgeInsets.fromLTRB(
        widget.embedded ? AppSpacing.md : AppSpacing.lg,
        widget.embedded ? AppSpacing.xs : AppSpacing.sm,
        widget.embedded ? AppSpacing.md : AppSpacing.lg,
        AppSpacing.sm + 2,
      ),
      child: AppSectionCard(
        margin: EdgeInsets.zero,
        padding: const EdgeInsets.symmetric(horizontal: AppSpacing.md, vertical: AppSpacing.xs),
        child: TextField(
          controller: _searchController,
          onChanged: (v) => setState(() => _searchQuery = v),
          textInputAction: TextInputAction.search,
          style: TextStyle(color: p.onSurface, fontSize: 14.5),
          decoration: InputDecoration(
            hintText: l.tr('search_contacts_hint'),
            prefixIcon: Icon(Icons.search_rounded, color: p.onSurfaceVariant),
            suffixIcon: _searchQuery.trim().isEmpty
                ? null
                : IconButton(
                    tooltip: l.tr('cancel'),
                    onPressed: () {
                      _searchController.clear();
                      setState(() => _searchQuery = '');
                    },
                    icon: Icon(Icons.close_rounded, color: p.onSurfaceVariant),
                  ),
            border: InputBorder.none,
            isDense: true,
          ),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final p = context.palette;
    final suggestions = _neighborSuggestions();
    final filteredContacts = _filteredContacts();

    final content = Material(
      color: Colors.transparent,
      child: _loading
          ? Center(child: CircularProgressIndicator(color: p.primary))
          : Column(
              children: [
                if (widget.embedded && _contacts.isNotEmpty) _buildSearchField(),
                if (suggestions.isNotEmpty)
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
                          _buildNeighborChips(suggestions),
                        ],
                      ),
                    ),
                  ),
                Expanded(
                  child: _contacts.isEmpty
                      ? _buildEmptyState()
                      : filteredContacts.isEmpty
                          ? Center(
                              child: Padding(
                                padding: _pageHorizontal,
                                child: Text(
                                  l.tr('contacts_search_empty'),
                                  textAlign: TextAlign.center,
                                  style: AppTypography.labelBase().copyWith(
                                    color: p.onSurfaceVariant.withOpacity(0.9),
                                    fontWeight: FontWeight.w500,
                                  ),
                                ),
                              ),
                            )
                          : ListView.builder(
                          padding: _pageHorizontal.copyWith(
                            top: AppSpacing.xs,
                            bottom: _fabClearance,
                          ),
                          itemCount: filteredContacts.length,
                          itemBuilder: (_, i) => _buildContactTile(filteredContacts[i]),
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
      appBar: riftAppBar(
        context,
        title: _searchMode ? '' : l.tr('contacts'),
        showBack: true,
        titleWidget: _searchMode
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
                  textInputAction: TextInputAction.search,
                  onChanged: (v) => setState(() => _searchQuery = v),
                  style: TextStyle(color: p.onSurface, fontSize: 15.5),
                  decoration: InputDecoration(
                    hintText: l.tr('search_contacts_hint'),
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
            : null,
        actions: [
          IconButton(
            tooltip: l.tr('search_contacts_hint'),
            icon: Icon(_searchMode ? Icons.close : Icons.search_rounded),
            onPressed: _toggleSearch,
          ),
        ],
      ),
      body: body,
      floatingActionButton: fab,
      floatingActionButtonLocation: FloatingActionButtonLocation.endFloat,
    );
  }
}
