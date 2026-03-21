import 'package:flutter/material.dart';

import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../theme/design_tokens.dart';
import '../widgets/app_primitives.dart';
import '../widgets/mesh_background.dart';
import 'contacts_screen.dart';
import 'groups_screen.dart';

const EdgeInsets _kChipRowPadding = EdgeInsets.fromLTRB(12, 10, 12, 4);
const double _kChipSpacing = 8;
const double _kChipIconSize = 14;
const double _kChipFontSize = 11;
const EdgeInsets _kChipPadding = EdgeInsets.symmetric(horizontal: 10, vertical: 6);
const double _kChipRadius = 12;
const double _kTabViewTopInset = 6;

/// Контакты и группы: минимальный AppBar (стрелка назад),
/// сегментированная панель — часть контента (body).
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
          appBar: riftAppBar(context, title: '', showBack: true),
          body: Column(
            children: [
              Padding(
                padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl, vertical: AppSpacing.xs),
                child: Builder(
                  builder: (ctx) {
                    final tab = DefaultTabController.of(ctx);
                    return ListenableBuilder(
                      listenable: tab,
                      builder: (_, __) => RiftSegmentedBar(
                        labels: [l.tr('contacts'), l.tr('groups')],
                        selectedIndex: tab.index,
                        onSelected: (i) => tab.animateTo(i),
                      ),
                    );
                  },
                ),
              ),
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
