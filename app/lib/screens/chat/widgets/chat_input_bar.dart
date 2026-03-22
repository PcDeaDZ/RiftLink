import 'package:flutter/material.dart';

import '../../../theme/app_theme.dart';
import '../../../theme/design_tokens.dart';

class ChatInputBar extends StatelessWidget {
  const ChatInputBar({
    super.key,
    required this.bottomInset,
    required this.child,
  });

  final double bottomInset;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: EdgeInsets.fromLTRB(
        AppSpacing.xl,
        AppSpacing.sm + 2,
        AppSpacing.sm,
        AppSpacing.sm + 2 + bottomInset,
      ),
      decoration: BoxDecoration(
        color: context.palette.surfaceVariant.withOpacity(0.92),
        border: Border(top: BorderSide(color: context.palette.divider, width: 0.5)),
      ),
      child: child,
    );
  }
}
