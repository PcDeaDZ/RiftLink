import 'package:flutter/material.dart';

import '../theme/app_theme.dart';

// Единые токены overlay-слоёв (см. app_popover_menu, rift_dialogs).
const double _kOverlayRadius = 16;
const EdgeInsets _kSnackContentPadding =
    EdgeInsets.symmetric(horizontal: 16, vertical: 14);

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
const EdgeInsets kSnackBarMarginChat = EdgeInsets.only(left: 16, right: 16, bottom: 100);

/// Единый стиль SnackBar: скругление, отступы, тени, палитра.
void showAppSnackBar(
  BuildContext context,
  String message, {
  AppSnackKind kind = AppSnackKind.neutral,
  Duration duration = const Duration(seconds: 3),
  EdgeInsetsGeometry margin = const EdgeInsets.fromLTRB(16, 0, 16, 20),
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
        style: TextStyle(color: fg, fontSize: 15, height: 1.35),
      ),
      backgroundColor: bg,
      behavior: SnackBarBehavior.floating,
      margin: margin,
      padding: _kSnackContentPadding,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(_kOverlayRadius),
      ),
      elevation: 8,
      duration: duration,
    ),
  );
}
