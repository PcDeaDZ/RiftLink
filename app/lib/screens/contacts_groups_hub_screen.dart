import 'package:flutter/material.dart';
import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import 'contacts_screen.dart';
import 'groups_screen.dart';

/// Один экран с вкладками: контакты (телефонная книга) и группы mesh — без дублирования в меню чата.
class ContactsGroupsHubScreen extends StatelessWidget {
  final RiftLinkBle ble;
  final List<String> neighbors;
  final List<int> initialGroups;

  const ContactsGroupsHubScreen({
    super.key,
    required this.ble,
    required this.neighbors,
    required this.initialGroups,
  });

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    return DefaultTabController(
      length: 2,
      child: Scaffold(
        backgroundColor: context.palette.surface,
        appBar: AppBar(
          title: Text(l.tr('contacts_groups_title')),
          leading: IconButton(
            icon: const Icon(Icons.arrow_back),
            onPressed: () => Navigator.pop(context),
          ),
          bottom: PreferredSize(
            preferredSize: const Size.fromHeight(38),
            child: TabBar(
              indicatorWeight: 2,
              labelPadding: const EdgeInsets.symmetric(horizontal: 8),
              labelColor: context.palette.primary,
              unselectedLabelColor: context.palette.onSurfaceVariant,
              indicatorColor: context.palette.primary,
              labelStyle: const TextStyle(fontWeight: FontWeight.w600, fontSize: 12.5, height: 1.1),
              tabs: [
                Tab(text: l.tr('contacts')),
                Tab(text: l.tr('groups')),
              ],
            ),
          ),
        ),
        body: TabBarView(
          children: [
            ContactsScreen(neighbors: neighbors, embedded: true),
            GroupsScreen(ble: ble, initialGroups: initialGroups, embedded: true),
          ],
        ),
      ),
    );
  }
}
