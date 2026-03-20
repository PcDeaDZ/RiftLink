import 'package:flutter/material.dart';

import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../widgets/mesh_background.dart';
import 'contacts_screen.dart';
import 'groups_screen.dart';

/// Контакты и группы: табы в [AppBar.title] — без дубля «заголовок + те же вкладки».
/// Стрелка «назад» слева, табы справа от неё — обычный паттерн Material.
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
    final p = context.palette;
    return DefaultTabController(
      length: 2,
      child: MeshBackgroundWrapper(
        child: Scaffold(
          backgroundColor: Colors.transparent,
          appBar: AppBar(
            toolbarHeight: 48,
            backgroundColor: p.surface.withOpacity(0.94),
            surfaceTintColor: Colors.transparent,
            leading: IconButton(
              icon: const Icon(Icons.arrow_back_rounded, size: 22),
              tooltip: MaterialLocalizations.of(context).backButtonTooltip,
              onPressed: () => Navigator.pop(context),
            ),
            titleSpacing: 8,
            centerTitle: false,
            title: SizedBox(
              height: 40,
              child: Material(
                color: p.surfaceVariant.withOpacity(0.85),
                borderRadius: BorderRadius.circular(12),
                clipBehavior: Clip.antiAlias,
                child: TabBar(
                  indicatorSize: TabBarIndicatorSize.tab,
                  dividerColor: Colors.transparent,
                  indicatorPadding: const EdgeInsets.symmetric(horizontal: 4, vertical: 4),
                  indicator: BoxDecoration(
                    color: p.primary.withOpacity(0.18),
                    borderRadius: BorderRadius.circular(10),
                  ),
                  labelColor: p.primary,
                  unselectedLabelColor: p.onSurfaceVariant,
                  tabs: [
                    Tab(
                      icon: Icon(
                        Icons.person_outline_rounded,
                        size: 22,
                        semanticLabel: l.tr('contacts'),
                      ),
                    ),
                    Tab(
                      icon: Icon(
                        Icons.groups_outlined,
                        size: 22,
                        semanticLabel: l.tr('groups'),
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ),
          body: TabBarView(
            physics: const BouncingScrollPhysics(),
            children: [
              Padding(
                padding: const EdgeInsets.only(top: 6),
                child: ContactsScreen(neighbors: neighbors, embedded: true),
              ),
              Padding(
                padding: const EdgeInsets.only(top: 6),
                child: GroupsScreen(ble: ble, initialGroups: initialGroups, embedded: true),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
