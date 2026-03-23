import 'package:flutter/material.dart';

import '../app_navigator.dart';
import '../theme/app_theme.dart';
import '../theme/design_tokens.dart';

final EdgeInsets _kSnackContentPadding = EdgeInsets.symmetric(
  horizontal: AppSpacing.lg,
  vertical: AppSpacing.buttonPrimaryV,
);

/// Тип тоста — единая палитра по всему приложению.
enum AppSnackKind {
  /// Обычное уведомление.
  neutral,
  /// Успех (зелёный оттенок).
  success,
  /// Ошибка (красный фон, белый текст).
  error,
}

/// Отступ для тоста над плавающей панелью ввода в чате / полноэкранными оверлеями.
final EdgeInsets kSnackBarMarginChat = EdgeInsets.only(
  left: AppSpacing.lg,
  right: AppSpacing.lg,
  bottom: 100,
);

/// Единый стиль SnackBar: скругление, отступы, тени, палитра.
void showAppSnackBar(
  BuildContext context,
  String message, {
  AppSnackKind kind = AppSnackKind.neutral,
  Duration duration = const Duration(seconds: 3),
  EdgeInsetsGeometry margin = const EdgeInsets.fromLTRB(AppSpacing.lg, 0, AppSpacing.lg, AppSpacing.xl),
}) {
  if (!context.mounted) return;
  final messenger = ScaffoldMessenger.of(context);
  messenger.hideCurrentSnackBar();
  final p = context.palette;

  late final Color bg;
  late final Color fg;
  switch (kind) {
    case AppSnackKind.error:
      bg = p.error.withOpacity(0.92);
      fg = Colors.white;
      break;
    case AppSnackKind.success:
      bg = p.success.withOpacity(0.34);
      fg = p.onSurface;
      break;
    case AppSnackKind.neutral:
      bg = p.card;
      fg = p.onSurface;
      break;
  }

  messenger.showSnackBar(
    SnackBar(
      content: Text(
        message,
        style: AppTypography.bodyBase().copyWith(color: fg),
      ),
      backgroundColor: bg,
      behavior: SnackBarBehavior.floating,
      margin: margin,
      padding: _kSnackContentPadding,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(AppRadius.overlay),
      ),
      elevation: AppElevation.snackBar,
      duration: duration,
    ),
  );
}

void showGlobalAppSnackBar(
  String message, {
  AppSnackKind kind = AppSnackKind.neutral,
  Duration duration = const Duration(seconds: 3),
  EdgeInsetsGeometry margin = const EdgeInsets.fromLTRB(AppSpacing.lg, 0, AppSpacing.lg, AppSpacing.xl),
}) {
  final messenger = scaffoldMessengerKey.currentState;
  final context = navigatorKey.currentContext;
  if (messenger == null || context == null || !context.mounted) return;

  late final Color bg;
  late final Color fg;
  final p = context.palette;
  switch (kind) {
    case AppSnackKind.error:
      bg = p.error.withOpacity(0.92);
      fg = Colors.white;
      break;
    case AppSnackKind.success:
      bg = p.success.withOpacity(0.34);
      fg = p.onSurface;
      break;
    case AppSnackKind.neutral:
      bg = p.card;
      fg = p.onSurface;
      break;
  }

  messenger.hideCurrentSnackBar();
  messenger.showSnackBar(
    SnackBar(
      content: Text(
        message,
        style: AppTypography.bodyBase().copyWith(color: fg),
      ),
      backgroundColor: bg,
      behavior: SnackBarBehavior.floating,
      margin: margin,
      padding: _kSnackContentPadding,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(AppRadius.overlay),
      ),
      elevation: AppElevation.snackBar,
      duration: duration,
    ),
  );
}
