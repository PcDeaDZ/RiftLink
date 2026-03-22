import 'package:flutter/material.dart';

import '../../../theme/app_theme.dart';
import '../../../theme/design_tokens.dart';

class ChatStatusChipData {
  final String text;
  final IconData icon;
  final Color color;

  const ChatStatusChipData({
    required this.text,
    required this.icon,
    required this.color,
  });
}

class ChatStatusPanel extends StatelessWidget {
  const ChatStatusPanel({super.key, required this.chips});

  final List<ChatStatusChipData> chips;

  @override
  Widget build(BuildContext context) {
    if (chips.isEmpty) return const SizedBox.shrink();
    return Container(
      padding: const EdgeInsets.fromLTRB(
        AppSpacing.sm + 2,
        AppSpacing.xs,
        AppSpacing.sm + 2,
        AppSpacing.xs,
      ),
      decoration: BoxDecoration(
        color: context.palette.surface.withOpacity(0.9),
        border: Border(
          bottom: BorderSide(color: context.palette.divider.withOpacity(0.45)),
        ),
      ),
      child: SingleChildScrollView(
        scrollDirection: Axis.horizontal,
        child: Row(
          children: [
            for (var i = 0; i < chips.length; i++) ...[
              _StatusChip(chip: chips[i]),
              if (i != chips.length - 1) const SizedBox(width: AppSpacing.xs + 2),
            ],
          ],
        ),
      ),
    );
  }
}

class _StatusChip extends StatelessWidget {
  const _StatusChip({required this.chip});

  final ChatStatusChipData chip;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: AppSpacing.sm, vertical: 5),
      decoration: BoxDecoration(
        color: context.palette.surfaceVariant.withOpacity(0.48),
        borderRadius: BorderRadius.circular(AppRadius.sm + 2),
        border: Border.all(color: context.palette.divider.withOpacity(0.4)),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(chip.icon, size: 13, color: chip.color.withOpacity(0.95)),
          const SizedBox(width: AppSpacing.xs),
          Text(
            chip.text,
            style: AppTypography.chipBase().copyWith(
              color: context.palette.onSurfaceVariant,
              fontSize: 11,
              fontWeight: FontWeight.w500,
            ),
          ),
        ],
      ),
    );
  }
}
