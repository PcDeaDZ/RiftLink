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
    if (chips.length <= 2) return const SizedBox.shrink();
    return Container(
      padding: const EdgeInsets.fromLTRB(
        AppSpacing.sm + 2,
        AppSpacing.sm,
        AppSpacing.sm + 2,
        AppSpacing.xs / 2,
      ),
      color: context.palette.surface.withOpacity(0.92),
      child: Wrap(
        spacing: AppSpacing.sm,
        runSpacing: AppSpacing.sm,
        children: chips.map((chip) => _StatusChip(chip: chip)).toList(),
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
      padding: const EdgeInsets.symmetric(horizontal: AppSpacing.sm, vertical: AppSpacing.xs),
      decoration: BoxDecoration(
        color: chip.color.withOpacity(0.12),
        borderRadius: BorderRadius.circular(AppRadius.sm),
        border: Border.all(color: chip.color.withOpacity(0.35)),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(chip.icon, size: 14, color: chip.color),
          const SizedBox(width: AppSpacing.xs),
          Text(
            chip.text,
            style: AppTypography.chipBase().copyWith(color: chip.color),
          ),
        ],
      ),
    );
  }
}
