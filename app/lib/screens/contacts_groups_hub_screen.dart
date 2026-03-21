import 'package:flutter/material.dart';

import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../widgets/mesh_background.dart';
import 'contacts_screen.dart';
import 'groups_screen.dart';

// Визуальные константы экрана (табы, чипы, отступы) — одна шкала радиусов и отступов.
const double _kAppBarToolbarHeight = 48;
const double _kTabBarSlotHeight = 40;
const double _kTabContainerRadius = 12;
const double _kTabIndicatorRadius = 10;
const EdgeInsets _kTabIndicatorPadding = EdgeInsets.symmetric(horizontal: 4, vertical: 4);
const EdgeInsets _kChipRowPadding = EdgeInsets.fromLTRB(12, 10, 12, 4);
const double _kChipSpacing = 8;
const double _kChipIconSize = 14;
const double _kChipFontSize = 11;
const EdgeInsets _kChipPadding = EdgeInsets.symmetric(horizontal: 10, vertical: 6);
const double _kChipRadius = 12;
const double _kTabViewTopInset = 6;

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
            toolbarHeight: _kAppBarToolbarHeight,
            elevation: 0,
            scrolledUnderElevation: 0,
            backgroundColor: p.surface.withOpacity(0.94),
            surfaceTintColor: Colors.transparent,
            foregroundColor: p.onSurface,
            iconTheme: IconThemeData(color: p.onSurface, size: 22),
            leading: IconButton(
              style: IconButton.styleFrom(foregroundColor: p.onSurface),
              icon: const Icon(Icons.arrow_back_rounded, size: 22),
              tooltip: MaterialLocalizations.of(context).backButtonTooltip,
              onPressed: () => Navigator.pop(context),
            ),
            titleSpacing: 8,
            centerTitle: false,
            title: SizedBox(
              height: _kTabBarSlotHeight,
              child: Material(
                color: p.surfaceVariant.withOpacity(0.92),
                elevation: 0,
                shadowColor: Colors.transparent,
                shape: RoundedRectangleBorder(
                  borderRadius: BorderRadius.circular(_kTabContainerRadius),
                  side: BorderSide(color: p.divider),
                ),
                clipBehavior: Clip.antiAlias,
                child: TabBar(
                  indicatorSize: TabBarIndicatorSize.tab,
                  dividerColor: Colors.transparent,
                  indicatorPadding: _kTabIndicatorPadding,
                  labelColor: p.primary,
                  unselectedLabelColor: p.onSurfaceVariant,
                  labelStyle: const TextStyle(fontWeight: FontWeight.w600, fontSize: 13),
                  unselectedLabelStyle: const TextStyle(fontWeight: FontWeight.w500, fontSize: 13),
                  indicator: BoxDecoration(
                    color: p.primary.withOpacity(0.18),
                    borderRadius: BorderRadius.circular(_kTabIndicatorRadius),
                  ),
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
                padding: _kChipRowPadding,
                child: Wrap(
                  spacing: _kChipSpacing,
                  runSpacing: _kChipSpacing,
                  children: [
                    _uxChip(context, p, Icons.lock_outline_rounded, l.tr('pairwise_key_required')),
                    _uxChip(context, p, Icons.priority_high_rounded, l.tr('critical_lane_label')),
                    _uxChip(context, p, Icons.schedule_rounded, l.tr('time_capsule_label')),
                  ],
                ),
              ),
              Expanded(
                child: TabBarView(
                  physics: const BouncingScrollPhysics(),
                  children: [
                    Padding(
                      padding: const EdgeInsets.only(top: _kTabViewTopInset),
                      child: ContactsScreen(neighbors: neighbors, embedded: true),
                    ),
                    Padding(
                      padding: const EdgeInsets.only(top: _kTabViewTopInset),
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

  /// Чипы в том же визуальном семействе, что и контейнер табов: surfaceVariant + граница divider.
  Widget _uxChip(BuildContext context, AppPalette p, IconData icon, String text) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: p.surfaceVariant.withOpacity(0.92),
        borderRadius: BorderRadius.circular(_kChipRadius),
        border: Border.all(color: p.divider),
      ),
      child: Padding(
        padding: _kChipPadding,
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(icon, size: _kChipIconSize, color: p.primary),
            SizedBox(width: _kChipSpacing / 2),
            Text(
              text,
              style: TextStyle(
                fontSize: _kChipFontSize,
                fontWeight: FontWeight.w600,
                color: p.onSurfaceVariant,
              ),
            ),
          ],
        ),
      ),
    );
  }
}
