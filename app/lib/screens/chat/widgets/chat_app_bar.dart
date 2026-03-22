import 'package:flutter/material.dart';

import '../../../theme/app_theme.dart';
import '../../../theme/design_tokens.dart';

class ChatAppBarTitle extends StatelessWidget {
  const ChatAppBarTitle({
    super.key,
    required this.chatIcon,
    required this.label,
    this.subtitle,
  });

  final IconData chatIcon;
  final String label;
  final String? subtitle;

  @override
  Widget build(BuildContext context) {
    final titleColor = context.palette.onSurface;
    final statusColor = context.palette.onSurfaceVariant;
    final row = Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Icon(
          chatIcon,
          size: 18,
          color: titleColor,
        ),
        const SizedBox(width: AppSpacing.sm),
        Flexible(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                label,
                style: AppTypography.bodyBase().copyWith(
                  fontWeight: FontWeight.w600,
                  color: titleColor,
                ),
                overflow: TextOverflow.ellipsis,
              ),
              if (subtitle != null && subtitle!.trim().isNotEmpty)
                Text(
                  subtitle!,
                  style: AppTypography.chipBase().copyWith(
                    fontSize: 11,
                    color: statusColor,
                  ),
                  overflow: TextOverflow.ellipsis,
                ),
            ],
          ),
        ),
      ],
    );
    return row;
  }
}
