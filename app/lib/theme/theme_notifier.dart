import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../app_navigator.dart';
import '../l10n/app_localizations.dart';
import 'app_theme.dart';

const _themeModeKey = 'riftlink_theme_mode';

final themeModeNotifier = ValueNotifier<ThemeMode>(ThemeMode.system);

Future<void> loadThemeMode() async {
  final prefs = await SharedPreferences.getInstance();
  final v = prefs.getString(_themeModeKey);
  themeModeNotifier.value = switch (v) {
    'light' => ThemeMode.light,
    'dark' => ThemeMode.dark,
    _ => ThemeMode.system,
  };
}

Future<void> setThemeMode(ThemeMode mode) async {
  themeModeNotifier.value = mode;
  final prefs = await SharedPreferences.getInstance();
  final s = switch (mode) {
    ThemeMode.light => 'light',
    ThemeMode.dark => 'dark',
    ThemeMode.system => 'system',
  };
  await prefs.setString(_themeModeKey, s);
}

/// Выбор дневной / ночной темы (общий лист для экрана сканирования и настроек).
void showThemeModeSheet(BuildContext context) {
  final l = context.l10n;
  showAppModalBottomSheet<void>(
    context: context,
    backgroundColor: Colors.transparent,
    shape: const RoundedRectangleBorder(
      borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
    ),
    builder: (ctx) {
      return ListenableBuilder(
        listenable: themeModeNotifier,
        builder: (innerCtx, _) {
          final p = innerCtx.palette;
          Widget row(ThemeMode mode, String title) {
            return ListTile(
              title: Text(title, style: TextStyle(color: p.onSurface)),
              trailing: themeModeNotifier.value == mode ? Icon(Icons.check, color: p.primary) : null,
              onTap: () async {
                if (themeModeNotifier.value != mode) {
                  await setThemeMode(mode);
                }
                if (ctx.mounted && Navigator.of(ctx).canPop()) {
                  Navigator.of(ctx).pop();
                }
              },
            );
          }
          return Container(
            decoration: BoxDecoration(
              color: p.card,
              borderRadius: const BorderRadius.vertical(top: Radius.circular(16)),
            ),
            child: SafeArea(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  row(ThemeMode.system, l.tr('theme_system')),
                  row(ThemeMode.light, l.tr('theme_light')),
                  row(ThemeMode.dark, l.tr('theme_dark')),
                ],
              ),
            ),
          );
        },
      );
    },
  );
}
