import 'package:flutter/material.dart';

import '../theme/app_theme.dart';
import '../theme/design_tokens.dart';

/// Карточка секции: фон [AppPalette.card], обводка [AppPalette.divider].
class AppSectionCard extends StatelessWidget {
  const AppSectionCard({
    super.key,
    required this.child,
    this.padding,
    this.margin,
    this.showBorder = true,
  });

  final Widget child;
  final EdgeInsetsGeometry? padding;
  final EdgeInsetsGeometry? margin;
  final bool showBorder;

  @override
  Widget build(BuildContext context) {
    final p = context.palette;
    return Padding(
      padding: margin ?? EdgeInsets.zero,
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: p.card,
          borderRadius: BorderRadius.circular(AppRadius.card),
          border: showBorder ? Border.all(color: p.divider) : null,
        ),
        child: Padding(
          padding: padding ?? const EdgeInsets.all(AppSpacing.lg),
          child: child,
        ),
      ),
    );
  }
}

/// Основная кнопка (filled): стили из [AppTheme.filledButtonTheme], опционально на всю ширину.
class AppPrimaryButton extends StatelessWidget {
  const AppPrimaryButton({
    super.key,
    required this.onPressed,
    required this.child,
    this.fullWidth = true,
  });

  final VoidCallback? onPressed;
  final Widget child;
  final bool fullWidth;

  @override
  Widget build(BuildContext context) {
    final button = FilledButton(
      onPressed: onPressed,
      child: child,
    );
    if (!fullWidth) return button;
    return SizedBox(width: double.infinity, child: button);
  }
}

/// Вторичная кнопка (outlined): стили из [AppTheme.outlinedButtonTheme].
class AppSecondaryButton extends StatelessWidget {
  const AppSecondaryButton({
    super.key,
    required this.onPressed,
    required this.child,
    this.fullWidth = false,
  });

  final VoidCallback? onPressed;
  final Widget child;
  final bool fullWidth;

  @override
  Widget build(BuildContext context) {
    final button = OutlinedButton(
      onPressed: onPressed,
      child: child,
    );
    if (!fullWidth) return button;
    return SizedBox(width: double.infinity, child: button);
  }
}

/// Семантика состояния для [AppStateChip].
enum AppStateKind {
  success,
  error,
  info,
  neutral,
}

/// Компактный чип статуса (цвета из [AppPalette]).
class AppStateChip extends StatelessWidget {
  const AppStateChip({
    super.key,
    required this.label,
    required this.kind,
  });

  final String label;
  final AppStateKind kind;

  Color _foreground(AppPalette p) {
    switch (kind) {
      case AppStateKind.success:
        return p.success;
      case AppStateKind.error:
        return p.error;
      case AppStateKind.info:
        return p.primary;
      case AppStateKind.neutral:
        return p.onSurfaceVariant;
    }
  }

  @override
  Widget build(BuildContext context) {
    final p = context.palette;
    final fg = _foreground(p);
    return Container(
      padding: const EdgeInsets.symmetric(
        horizontal: AppSpacing.sm,
        vertical: AppSpacing.xs,
      ),
      decoration: BoxDecoration(
        color: fg.withOpacity(0.12),
        borderRadius: BorderRadius.circular(AppRadius.sm),
        border: Border.all(color: fg.withOpacity(0.35)),
      ),
      child: Text(
        label,
        style: AppTypography.chipBase().copyWith(color: fg),
      ),
    );
  }
}

/// Заголовок экрана (не AppBar): типографика и цвет из палитры.
class AppScreenTitle extends StatelessWidget {
  const AppScreenTitle(
    this.text, {
    super.key,
    this.maxLines = 1,
    this.overflow = TextOverflow.ellipsis,
  });

  final String text;
  final int maxLines;
  final TextOverflow overflow;

  @override
  Widget build(BuildContext context) {
    return Text(
      text,
      maxLines: maxLines,
      overflow: overflow,
      style: AppTypography.screenTitleBase().copyWith(
        color: context.palette.onSurface,
      ),
    );
  }
}
