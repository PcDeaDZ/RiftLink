import 'package:flutter/material.dart';

/// Глобальный ключ навигатора — для показа диалогов при любой локали
final navigatorKey = GlobalKey<NavigatorState>();

/// Показывает диалог через корневой контекст (обходит проблемы с русской локалью)
Future<T?> showAppDialog<T>({
  required BuildContext context,
  required Widget Function(BuildContext) builder,
  bool barrierDismissible = true,
  Color? barrierColor,
}) {
  final ctx = navigatorKey.currentContext ?? context;
  return showDialog<T>(
    context: ctx,
    useRootNavigator: true,
    barrierDismissible: barrierDismissible,
    barrierColor: barrierColor ?? Colors.black54,
    builder: builder,
  );
}

/// Показывает bottom sheet через корневой контекст
Future<T?> showAppModalBottomSheet<T>({
  required BuildContext context,
  required Widget Function(BuildContext) builder,
  Color? backgroundColor,
  ShapeBorder? shape,
  bool isScrollControlled = false,
}) {
  final ctx = navigatorKey.currentContext ?? context;
  return showModalBottomSheet<T>(
    context: ctx,
    backgroundColor: backgroundColor,
    shape: shape,
    isScrollControlled: isScrollControlled,
    builder: builder,
  );
}
