import 'package:flutter/material.dart';

import '../app_navigator.dart';
import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../theme/design_tokens.dart';

// Согласовано с app_snackbar / app_popover_menu (единый overlay-слой).
final EdgeInsets _kDialogInsetPadding = EdgeInsets.symmetric(
  horizontal: AppSpacing.xxl,
  vertical: AppSpacing.xxl,
);

/// Общая оболочка: без «тяжёлого» AlertDialog, тонкая рамка, ограниченная ширина.
class RiftDialogFrame extends StatelessWidget {
  const RiftDialogFrame({
    super.key,
    required this.child,
    this.padding = const EdgeInsets.fromLTRB(AppSpacing.xl, AppSpacing.xl, AppSpacing.xl, AppSpacing.lg),
    this.maxWidth = 332,
  });

  final Widget child;
  final EdgeInsetsGeometry padding;
  final double maxWidth;

  @override
  Widget build(BuildContext context) {
    final p = context.palette;
    return Dialog(
      backgroundColor: Colors.transparent,
      insetPadding: _kDialogInsetPadding,
      child: ConstrainedBox(
        constraints: BoxConstraints(maxWidth: maxWidth),
        child: Material(
          color: p.card,
          elevation: AppElevation.dialog,
          shadowColor: Colors.black.withOpacity(0.14),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(AppRadius.overlay),
            side: BorderSide(color: p.divider.withOpacity(0.5)),
          ),
          clipBehavior: Clip.antiAlias,
          child: Padding(padding: padding, child: child),
        ),
      ),
    );
  }
}

Future<bool?> showRiftConfirmDialog({
  required BuildContext context,
  required String title,
  required String message,
  required String cancelText,
  required String confirmText,
  bool danger = false,
  IconData? icon,
}) {
  return showAppDialog<bool>(
    context: context,
    barrierDismissible: true,
    builder: (ctx) {
      final p = context.palette;
      return RiftDialogFrame(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          mainAxisSize: MainAxisSize.min,
          children: [
            Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                if (icon != null) ...[
                  Padding(
                    padding: const EdgeInsets.only(top: 1, right: AppSpacing.md),
                    child: Icon(
                      icon,
                      size: AppIconSize.lg,
                      color: danger ? p.error : p.onSurfaceVariant,
                    ),
                  ),
                ],
                Expanded(
                  child: Text(
                    title,
                    style: Theme.of(context).textTheme.titleMedium?.copyWith(
                          fontWeight: FontWeight.w700,
                          color: p.onSurface,
                          height: 1.25,
                        ),
                  ),
                ),
              ],
            ),
            const SizedBox(height: AppSpacing.md),
            Text(
              message,
              style: AppTypography.bodyBase().copyWith(
                fontSize: AppTypography.bodyLargeSize,
                color: p.onSurfaceVariant,
              ),
            ),
            const SizedBox(height: AppSpacing.lg),
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
                  onPressed: () => Navigator.pop(ctx, false),
                  child: Text(cancelText),
                ),
                const SizedBox(width: AppSpacing.sm),
                TextButton(
                  style: TextButton.styleFrom(
                    foregroundColor: danger ? p.error : p.primary,
                    padding: const EdgeInsets.symmetric(horizontal: AppSpacing.lg, vertical: AppSpacing.sm),
                    minimumSize: Size.zero,
                    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                    textStyle: const TextStyle(fontWeight: FontWeight.w600),
                  ),
                  onPressed: () => Navigator.pop(ctx, true),
                  child: Text(confirmText),
                ),
              ],
            ),
          ],
        ),
      );
    },
  );
}

Future<void> showRiftSelftestDialog(
  BuildContext context,
  RiftLinkSelftestEvent evt,
  {RiftLinkInfoEvent? lastInfo}
) {
  final l = context.l10n;
  final p = context.palette;
  final ok = evt.radioOk && evt.displayOk && evt.antennaOk;
  final accent = ok ? p.success : p.error;

  return showAppDialog<void>(
    context: context,
    barrierDismissible: true,
    builder: (ctx) {
      final vStr = evt.batteryMv > 0 ? (evt.batteryMv / 1000).toStringAsFixed(2) : '';

      return RiftDialogFrame(
        padding: const EdgeInsets.fromLTRB(AppSpacing.xl, AppSpacing.xl, AppSpacing.xl, AppSpacing.buttonPrimaryV),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          mainAxisSize: MainAxisSize.min,
          children: [
            Row(
              children: [
                Icon(
                  ok ? Icons.check_circle_outline_rounded : Icons.error_outline_rounded,
                  size: AppIconSize.xxl,
                  color: accent,
                ),
                const SizedBox(width: AppSpacing.md),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        l.tr('selftest'),
                        style: Theme.of(ctx).textTheme.titleMedium?.copyWith(
                              fontWeight: FontWeight.w700,
                              color: p.onSurface,
                              height: 1.2,
                            ),
                      ),
                      const SizedBox(height: 2),
                      Text(
                        ok ? l.tr('selftest_summary_ok') : l.tr('selftest_summary_fail'),
                        style: AppTypography.labelBase().copyWith(
                          height: 1.3,
                          color: p.onSurfaceVariant,
                        ),
                      ),
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: AppSpacing.lg),
            Container(
              padding: const EdgeInsets.symmetric(vertical: AppSpacing.xs),
              decoration: BoxDecoration(
                color: p.surfaceVariant.withOpacity(0.4),
                borderRadius: BorderRadius.circular(AppRadius.md),
              ),
              child: Column(
                children: [
                  _selftestRow(context, l.tr('selftest_radio'), evt.radioOk),
                  Divider(height: 1, thickness: 1, color: p.divider.withOpacity(0.35)),
                  _selftestRow(context, l.tr('selftest_antenna'), evt.antennaOk),
                  Divider(height: 1, thickness: 1, color: p.divider.withOpacity(0.35)),
                  _selftestRow(context, l.tr('selftest_display'), evt.displayOk),
                  if (evt.batteryMv > 0) ...[
                    Divider(height: 1, thickness: 1, color: p.divider.withOpacity(0.35)),
                    _selftestMetric(
                      context,
                      evt.charging ? Icons.battery_charging_full_rounded : Icons.battery_std_rounded,
                      evt.batteryPercent != null
                          ? '${evt.batteryPercent}% ($vStr V)${evt.charging ? ' ⚡' : ''}'
                          : l.tr('selftest_voltage', {'v': vStr}),
                    ),
                  ],
                  if (evt.heapFree > 0) ...[
                    Divider(height: 1, thickness: 1, color: p.divider.withOpacity(0.35)),
                    _selftestMetric(
                      context,
                      Icons.memory_rounded,
                      l.tr('selftest_heap', {'kb': '${evt.heapFree}'}),
                    ),
                  ],
                  if (lastInfo != null) ...[
                    Divider(height: 1, thickness: 1, color: p.divider.withOpacity(0.35)),
                    _selftestMetric(
                      context,
                      Icons.settings_input_antenna_rounded,
                      l.tr('selftest_modem', {'value': _modemInfoString(l, lastInfo)}),
                    ),
                  ],
                ],
              ),
            ),
            const SizedBox(height: AppSpacing.sm),
            Align(
              alignment: Alignment.centerRight,
              child: TextButton(
                style: TextButton.styleFrom(
                  foregroundColor: p.primary,
                  padding: const EdgeInsets.symmetric(horizontal: AppSpacing.lg, vertical: AppSpacing.sm),
                  minimumSize: Size.zero,
                  tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                ),
                onPressed: () => Navigator.pop(ctx),
                child: Text(l.tr('ok')),
              ),
            ),
          ],
        ),
      );
    },
  );
}

Widget _selftestRow(BuildContext context, String label, bool passed) {
  final p = context.palette;
  return Padding(
    padding: const EdgeInsets.symmetric(horizontal: AppSpacing.lg, vertical: AppSpacing.sm),
    child: Row(
      children: [
        Expanded(
          child: Text(
            label,
            style: AppTypography.bodyBase().copyWith(
              fontSize: AppTypography.bodyLargeSize,
              fontWeight: FontWeight.w600,
              color: p.onSurface,
            ),
          ),
        ),
        Icon(
          passed ? Icons.check_rounded : Icons.close_rounded,
          size: AppIconSize.md,
          color: passed ? p.success : p.error,
        ),
      ],
    ),
  );
}

Widget _selftestMetric(BuildContext context, IconData icon, String text) {
  final p = context.palette;
  return Padding(
    padding: const EdgeInsets.symmetric(horizontal: AppSpacing.lg, vertical: AppSpacing.sm),
    child: Row(
      children: [
        Icon(icon, size: AppIconSize.sm, color: p.onSurfaceVariant),
        const SizedBox(width: AppSpacing.sm),
        Expanded(
          child: Text(
            text,
            style: AppTypography.labelBase().copyWith(color: p.onSurface),
          ),
        ),
      ],
    ),
  );
}

String _modemInfoString(AppLocalizations l, RiftLinkInfoEvent info) {
  final preset = info.modemPreset;
  if (preset == null) return '—';
  if (preset == 4) {
    final sf = info.sf?.toString() ?? '?';
    final bw = info.bw?.toStringAsFixed(1) ?? '?';
    final cr = info.cr?.toString() ?? '?';
    return '${l.tr('modem_preset_custom')} (SF$sf / BW$bw / CR$cr)';
  }
  const presetKeys = <int, String>{
    0: 'modem_preset_speed',
    1: 'modem_preset_normal',
    2: 'modem_preset_range',
    3: 'modem_preset_maxrange',
  };
  return l.tr(presetKeys[preset] ?? 'modem_preset_normal');
}
