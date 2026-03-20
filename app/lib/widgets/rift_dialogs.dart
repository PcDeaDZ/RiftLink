import 'package:flutter/material.dart';

import '../app_navigator.dart';
import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';

/// Общая оболочка: без «тяжёлого» AlertDialog, тонкая рамка, ограниченная ширина.
class RiftDialogFrame extends StatelessWidget {
  const RiftDialogFrame({
    super.key,
    required this.child,
    this.padding = const EdgeInsets.fromLTRB(22, 20, 18, 16),
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
      insetPadding: const EdgeInsets.symmetric(horizontal: 28, vertical: 22),
      child: ConstrainedBox(
        constraints: BoxConstraints(maxWidth: maxWidth),
        child: Material(
          color: p.card,
          elevation: 5,
          shadowColor: Colors.black.withOpacity(0.14),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(18),
            side: BorderSide(color: p.divider.withOpacity(0.4)),
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
                    padding: const EdgeInsets.only(top: 1, right: 10),
                    child: Icon(
                      icon,
                      size: 22,
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
            const SizedBox(height: 10),
            Text(
              message,
              style: TextStyle(
                fontSize: 14,
                height: 1.35,
                color: p.onSurfaceVariant,
              ),
            ),
            const SizedBox(height: 18),
            Row(
              mainAxisAlignment: MainAxisAlignment.end,
              children: [
                TextButton(
                  style: TextButton.styleFrom(
                    foregroundColor: p.onSurfaceVariant,
                    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                    minimumSize: Size.zero,
                    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                  ),
                  onPressed: () => Navigator.pop(ctx, false),
                  child: Text(cancelText),
                ),
                const SizedBox(width: 4),
                TextButton(
                  style: TextButton.styleFrom(
                    foregroundColor: danger ? p.error : p.primary,
                    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
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
        padding: const EdgeInsets.fromLTRB(20, 18, 16, 14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          mainAxisSize: MainAxisSize.min,
          children: [
            Row(
              children: [
                Icon(
                  ok ? Icons.check_circle_outline_rounded : Icons.error_outline_rounded,
                  size: 26,
                  color: accent,
                ),
                const SizedBox(width: 10),
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
                        style: TextStyle(
                          fontSize: 13,
                          height: 1.3,
                          color: p.onSurfaceVariant,
                        ),
                      ),
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: 14),
            Container(
              padding: const EdgeInsets.symmetric(vertical: 4),
              decoration: BoxDecoration(
                color: p.surfaceVariant.withOpacity(0.35),
                borderRadius: BorderRadius.circular(12),
              ),
              child: Column(
                children: [
                  _selftestRow(context, l.tr('selftest_radio'), evt.radioOk),
                  Divider(height: 1, thickness: 1, color: p.divider.withOpacity(0.25)),
                  _selftestRow(context, l.tr('selftest_antenna'), evt.antennaOk),
                  Divider(height: 1, thickness: 1, color: p.divider.withOpacity(0.25)),
                  _selftestRow(context, l.tr('selftest_display'), evt.displayOk),
                  if (evt.batteryMv > 0) ...[
                    Divider(height: 1, thickness: 1, color: p.divider.withOpacity(0.25)),
                    _selftestMetric(
                      context,
                      evt.charging ? Icons.battery_charging_full_rounded : Icons.battery_std_rounded,
                      evt.batteryPercent != null
                          ? '${evt.batteryPercent}% ($vStr V)${evt.charging ? ' ⚡' : ''}'
                          : l.tr('selftest_voltage', {'v': vStr}),
                    ),
                  ],
                  if (evt.heapFree > 0) ...[
                    Divider(height: 1, thickness: 1, color: p.divider.withOpacity(0.25)),
                    _selftestMetric(
                      context,
                      Icons.memory_rounded,
                      l.tr('selftest_heap', {'kb': '${evt.heapFree}'}),
                    ),
                  ],
                  if (lastInfo != null) ...[
                    Divider(height: 1, thickness: 1, color: p.divider.withOpacity(0.25)),
                    _selftestMetric(
                      context,
                      Icons.settings_input_antenna_rounded,
                      l.tr('selftest_modem', {'value': _modemInfoString(l, lastInfo)}),
                    ),
                  ],
                ],
              ),
            ),
            const SizedBox(height: 6),
            Align(
              alignment: Alignment.centerRight,
              child: TextButton(
                style: TextButton.styleFrom(
                  foregroundColor: p.primary,
                  padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
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
    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
    child: Row(
      children: [
        Expanded(
          child: Text(
            label,
            style: TextStyle(
              fontSize: 14,
              fontWeight: FontWeight.w600,
              color: p.onSurface,
            ),
          ),
        ),
        Icon(
          passed ? Icons.check_rounded : Icons.close_rounded,
          size: 20,
          color: passed ? p.success : p.error,
        ),
      ],
    ),
  );
}

Widget _selftestMetric(BuildContext context, IconData icon, String text) {
  final p = context.palette;
  return Padding(
    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 7),
    child: Row(
      children: [
        Icon(icon, size: 18, color: p.onSurfaceVariant),
        const SizedBox(width: 8),
        Expanded(
          child: Text(
            text,
            style: TextStyle(fontSize: 13, color: p.onSurface),
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
