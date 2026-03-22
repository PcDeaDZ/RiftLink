import 'package:flutter/material.dart';

import '../../../theme/app_theme.dart';
import '../../../theme/design_tokens.dart';

class ChatAppBarTitle extends StatelessWidget {
  const ChatAppBarTitle({
    super.key,
    required this.label,
    required this.isConnected,
    required this.showBattery,
    required this.batteryBadge,
    required this.offlinePending,
    required this.onConnectedTap,
    required this.onDisconnectedTap,
  });

  final String label;
  final bool isConnected;
  final bool showBattery;
  final Widget batteryBadge;
  final int? offlinePending;
  final VoidCallback onConnectedTap;
  final VoidCallback onDisconnectedTap;

  @override
  Widget build(BuildContext context) {
    final row = Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Icon(
          isConnected ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
          size: 18,
          color: isConnected ? context.palette.success : context.palette.onSurfaceVariant,
        ),
        const SizedBox(width: AppSpacing.sm),
        Flexible(
          child: Text(
            label,
            style: AppTypography.bodyBase().copyWith(
              fontWeight: FontWeight.w600,
              color: isConnected ? context.palette.success : context.palette.onSurfaceVariant,
            ),
            overflow: TextOverflow.ellipsis,
          ),
        ),
        if (showBattery) ...[
          const SizedBox(width: AppSpacing.sm),
          batteryBadge,
        ],
        if (offlinePending != null && offlinePending! > 0) ...[
          const SizedBox(width: AppSpacing.sm),
          Container(
            padding: const EdgeInsets.symmetric(
              horizontal: AppSpacing.sm,
              vertical: AppSpacing.xs / 2,
            ),
            decoration: BoxDecoration(
              color: context.palette.primary.withOpacity(0.16),
              borderRadius: BorderRadius.circular(AppRadius.sm),
              border: Border.all(color: context.palette.primary.withOpacity(0.45)),
            ),
            child: Text(
              '$offlinePending',
              style: AppTypography.chipBase().copyWith(
                fontWeight: FontWeight.w700,
                color: context.palette.primary,
              ),
            ),
          ),
        ],
      ],
    );
    return GestureDetector(
      onTap: isConnected ? onConnectedTap : onDisconnectedTap,
      child: row,
    );
  }
}
