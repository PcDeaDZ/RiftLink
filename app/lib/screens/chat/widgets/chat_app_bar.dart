import 'package:flutter/material.dart';

import '../../../theme/app_theme.dart';
import '../../../theme/design_tokens.dart';

class ChatAppBarTitle extends StatelessWidget {
  const ChatAppBarTitle({
    super.key,
    required this.chatIcon,
    this.chatIconColor,
    required this.label,
    this.subtitle,
    this.onLongPress,
  });

  final IconData chatIcon;
  final Color? chatIconColor;
  final String label;
  final String? subtitle;
  final VoidCallback? onLongPress;

  @override
  Widget build(BuildContext context) {
    final titleColor = context.palette.onSurface;
    final statusColor = context.palette.onSurfaceVariant;
    final row = Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Icon(
          chatIcon,
          size: AppIconSize.sm,
          color: chatIconColor ?? titleColor,
        ),
        const SizedBox(width: AppSpacing.sm),
        Flexible(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                label,
                style: AppTypography.navTitleBase().copyWith(
                  fontWeight: FontWeight.w600,
                  color: titleColor,
                ),
                overflow: TextOverflow.ellipsis,
              ),
              if (subtitle != null && subtitle!.trim().isNotEmpty)
                Text(
                  subtitle!,
                  style: AppTypography.captionBase().copyWith(
                    color: statusColor,
                  ),
                  overflow: TextOverflow.ellipsis,
                ),
            ],
          ),
        ),
      ],
    );
    if (onLongPress == null) return row;
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onLongPress: onLongPress,
      child: row,
    );
  }
}
