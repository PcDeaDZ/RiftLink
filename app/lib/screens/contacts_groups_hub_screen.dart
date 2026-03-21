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
          body: Column(
            children: [
              Padding(
                padding: const EdgeInsets.fromLTRB(10, 8, 10, 2),
                child: Wrap(
                  spacing: 6,
                  runSpacing: 6,
                  children: [
                    _uxChip(context, Icons.lock_outline_rounded, l.tr('pairwise_key_required')),
                    _uxChip(context, Icons.priority_high_rounded, l.tr('critical_lane_label')),
                    _uxChip(context, Icons.schedule_rounded, l.tr('time_capsule_label')),
                  ],
                ),
              ),
              Expanded(
                child: TabBarView(
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
            ],
          ),
        ),
      ),
    );
  }

  Widget _uxChip(BuildContext context, IconData icon, String text) {
    final p = context.palette;
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      decoration: BoxDecoration(
        color: p.primary.withOpacity(0.12),
        borderRadius: BorderRadius.circular(999),
        border: Border.all(color: p.primary.withOpacity(0.3)),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(icon, size: 14, color: p.primary),
          const SizedBox(width: 4),
          Text(
            text,
            style: TextStyle(fontSize: 11, fontWeight: FontWeight.w600, color: p.primary),
          ),
        ],
      ),
    );
  }
}
