import 'package:flutter/material.dart';

import '../../../l10n/app_localizations.dart';
import '../../../theme/app_theme.dart';
import '../../../theme/design_tokens.dart';

class ChatMessageList<T> extends StatelessWidget {
  const ChatMessageList({
    super.key,
    required this.messages,
    required this.scrollController,
    required this.itemBuilder,
    required this.l10n,
  });

  final List<T> messages;
  final ScrollController scrollController;
  final Widget Function(T msg) itemBuilder;
  final AppLocalizations l10n;

  @override
  Widget build(BuildContext context) {
    return Stack(
      children: [
        Container(
          color: Colors.transparent,
          child: messages.isEmpty
              ? Center(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Icon(
                        Icons.chat_bubble_outline,
                        size: 48,
                        color: context.palette.onSurfaceVariant.withOpacity(0.4),
                      ),
                      const SizedBox(height: AppSpacing.md),
                      Text(
                        l10n.tr('no_messages'),
                        style: AppTypography.bodyBase().copyWith(
                          fontSize: 14,
                          color: context.palette.onSurfaceVariant.withOpacity(0.7),
                        ),
                      ),
                    ],
                  ),
                )
              : ListView(
                  controller: scrollController,
                  padding: const EdgeInsets.symmetric(
                    horizontal: AppSpacing.md,
                    vertical: AppSpacing.sm,
                  ),
                  children: messages.map(itemBuilder).toList(),
                ),
        ),
        Positioned(
          left: 0,
          right: 0,
          bottom: 0,
          height: AppSpacing.xxl,
          child: IgnorePointer(
            child: Container(
              decoration: BoxDecoration(
                gradient: LinearGradient(
                  begin: Alignment.topCenter,
                  end: Alignment.bottomCenter,
                  colors: [
                    context.palette.surface.withOpacity(0),
                    context.palette.surface.withOpacity(0.95),
                  ],
                ),
              ),
            ),
          ),
        ),
      ],
    );
  }
}
