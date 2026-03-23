import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

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

/// Animated segmented bar used for region/channel/mode pickers.
class RiftSegmentedBar extends StatelessWidget {
  final List<String> labels;
  final int? selectedIndex;
  final ValueChanged<int> onSelected;
  final bool enabled;
  final IconData? leadingIcon;

  const RiftSegmentedBar({
    super.key,
    required this.labels,
    required this.selectedIndex,
    required this.onSelected,
    this.enabled = true,
    this.leadingIcon,
  });

  static const _slide = AppMotion.segmentSlide;
  static const _text = AppMotion.segmentCross;
  static const _curve = AppMotion.easeOutCubic;

  @override
  Widget build(BuildContext context) {
    final pal = context.palette;
    final inactive = pal.onSurfaceVariant.withOpacity(enabled ? 1.0 : 0.45);
    final n = labels.length;
    return Material(
      color: Colors.transparent,
      child: Container(
        decoration: BoxDecoration(
          color: pal.surfaceVariant,
          borderRadius: BorderRadius.circular(AppRadius.lg),
          border: Border.all(color: pal.divider),
        ),
        padding: const EdgeInsets.all(AppSpacing.xs),
        child: Row(children: [
          if (leadingIcon != null) ...[
            Padding(
              padding: const EdgeInsets.only(left: AppSpacing.sm, right: AppSpacing.xs),
              child: AnimatedSwitcher(
                duration: _text,
                switchInCurve: Curves.easeOut,
                switchOutCurve: Curves.easeIn,
                transitionBuilder: (child, anim) => ScaleTransition(scale: anim, child: FadeTransition(opacity: anim, child: child)),
                child: Icon(leadingIcon, key: ValueKey(leadingIcon), size: AppIconSize.md, color: pal.primary.withOpacity(enabled ? 1 : 0.4)),
              ),
            ),
          ],
          Expanded(
            child: n == 0
                ? const SizedBox.shrink()
                : LayoutBuilder(builder: (context, constraints) {
                    final segW = constraints.maxWidth / n;
                    final idx = selectedIndex;
                    final show = idx != null && idx >= 0 && idx < n;
                    final left = show ? segW * idx : 0.0;
                    return ClipRRect(
                      borderRadius: BorderRadius.circular(AppRadius.md),
                      child: Stack(clipBehavior: Clip.none, children: [
                        AnimatedPositioned(
                          duration: _slide, curve: _curve, left: left, width: show ? segW : 0, top: 0, bottom: 0,
                          child: AnimatedOpacity(
                            duration: AppMotion.standard, opacity: show ? 1 : 0,
                            child: DecoratedBox(
                              decoration: BoxDecoration(
                                borderRadius: BorderRadius.circular(AppRadius.md),
                                color: pal.primary.withOpacity(enabled ? 0.22 : 0.12),
                                border: Border.all(color: pal.primary.withOpacity(enabled ? 1 : 0.45), width: 1.5),
                                boxShadow: AppShadow.primarySegment(pal.primary),
                              ),
                            ),
                          ),
                        ),
                        Row(children: List.generate(n, (i) {
                          final sel = idx == i;
                          return Expanded(
                            child: Material(
                              color: Colors.transparent,
                              child: InkWell(
                                borderRadius: BorderRadius.circular(AppRadius.md),
                                onTap: enabled ? () { HapticFeedback.selectionClick(); onSelected(i); } : null,
                                child: Padding(
                                  padding: const EdgeInsets.symmetric(vertical: AppSpacing.md, horizontal: AppSpacing.segmentInnerH),
                                  child: Center(
                                    child: AnimatedDefaultTextStyle(
                                      duration: _slide, curve: _curve,
                                      style: AppTypography.labelBase().copyWith(
                                        letterSpacing: 0.3,
                                        fontWeight: sel ? FontWeight.w800 : FontWeight.w500,
                                        color: sel ? pal.primary : inactive,
                                      ),
                                      child: FittedBox(fit: BoxFit.scaleDown, child: Text(labels[i], maxLines: 1)),
                                    ),
                                  ),
                                ),
                              ),
                            ),
                          );
                        })),
                      ]),
                    );
                  }),
          ),
        ]),
      ),
    );
  }
}

/// Unified AppBar factory for all screens.
AppBar riftAppBar(
  BuildContext context, {
  required String title,
  bool showBack = false,
  List<Widget>? actions,
  Widget? titleWidget,
  PreferredSizeWidget? bottom,
}) {
  final pal = context.palette;
  return AppBar(
    toolbarHeight: AppBarMetrics.toolbarHeight,
    backgroundColor: pal.surface,
    foregroundColor: pal.onSurface,
    elevation: 0,
    scrolledUnderElevation: 0,
    surfaceTintColor: Colors.transparent,
    leading: showBack
        ? IconButton(
            icon: const Icon(Icons.arrow_back_rounded),
            iconSize: AppIconSize.md,
            onPressed: () => Navigator.pop(context),
          )
        : null,
    automaticallyImplyLeading: false,
    title: titleWidget ?? Text(title, style: AppTypography.navTitleBase().copyWith(color: pal.onSurface)),
    centerTitle: false,
    titleSpacing: showBack ? 0 : AppSpacing.lg,
    actions: actions,
    bottom: bottom,
  );
}

/// Animated toggle switch with haptic feedback, consistent style across the app.
class RiftSwitch extends StatelessWidget {
  final bool value;
  final ValueChanged<bool>? onChanged;

  const RiftSwitch({super.key, required this.value, this.onChanged});

  static const _dur = AppMotion.segmentCross;

  @override
  Widget build(BuildContext context) {
    final pal = context.palette;
    final active = onChanged != null;
    return GestureDetector(
      onTap: active ? () { HapticFeedback.selectionClick(); onChanged!(!value); } : null,
      child: AnimatedContainer(
        duration: _dur, curve: AppMotion.easeOutCubic,
        width: AppSwitchMetrics.trackWidth, height: AppSwitchMetrics.trackHeight,
        padding: const EdgeInsets.all(AppSwitchMetrics.padding),
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(AppRadius.switchTrack),
          color: value
              ? pal.primary.withOpacity(active ? 1 : 0.5)
              : pal.surfaceVariant.withOpacity(active ? 1 : 0.5),
          border: Border.all(
            color: value ? pal.primary.withOpacity(0.6) : pal.divider,
            width: 1,
          ),
        ),
        child: AnimatedAlign(
          duration: _dur, curve: AppMotion.easeOutCubic,
          alignment: value ? Alignment.centerRight : Alignment.centerLeft,
          child: Container(
            width: AppSwitchMetrics.knobSize, height: AppSwitchMetrics.knobSize,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: value ? Colors.white : pal.onSurfaceVariant.withOpacity(0.7),
              boxShadow: AppShadow.switchKnob,
            ),
          ),
        ),
      ),
    );
  }
}

/// ListTile with a RiftSwitch trailing widget.
class RiftSwitchTile extends StatelessWidget {
  final bool value;
  final ValueChanged<bool>? onChanged;
  final Widget? title;
  final Widget? subtitle;
  final Widget? leading;

  const RiftSwitchTile({super.key, required this.value, this.onChanged, this.title, this.subtitle, this.leading});

  @override
  Widget build(BuildContext context) {
    return ListTile(
      contentPadding: EdgeInsets.zero,
      leading: leading,
      title: title,
      subtitle: subtitle,
      trailing: RiftSwitch(value: value, onChanged: onChanged),
      onTap: onChanged != null ? () { HapticFeedback.selectionClick(); onChanged!(!value); } : null,
    );
  }
}
