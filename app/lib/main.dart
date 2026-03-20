import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'app_navigator.dart';
import 'l10n/app_localizations.dart';
import 'locale_notifier.dart';
import 'screens/scan_screen.dart';
import 'theme/app_theme.dart';
import 'theme/theme_notifier.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();

  await AppLocalizations.loadLocale();
  localeNotifier.value = AppLocalizations.currentLocale;
  await loadThemeMode();
  runApp(const RiftLinkApp());
}

class RiftLinkApp extends StatelessWidget {
  const RiftLinkApp({super.key});

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([localeNotifier, themeModeNotifier]),
      builder: (_, __) {
        final locale = localeNotifier.value;
        return MaterialApp(
          navigatorKey: navigatorKey,
          key: ValueKey('${locale.languageCode}_${themeModeNotifier.value.name}'),
          title: 'RiftLink',
          locale: locale,
          supportedLocales: const [Locale('en'), Locale('ru')],
          localizationsDelegates: const [
            GlobalMaterialLocalizations.delegate,
            GlobalWidgetsLocalizations.delegate,
            GlobalCupertinoLocalizations.delegate,
          ],
          theme: AppTheme.light,
          darkTheme: AppTheme.dark,
          themeMode: themeModeNotifier.value,
          builder: (context, child) {
            final brightness = Theme.of(context).brightness;
            final isDark = brightness == Brightness.dark;
            SystemChrome.setSystemUIOverlayStyle(SystemUiOverlayStyle(
              statusBarColor: Colors.transparent,
              statusBarIconBrightness: isDark ? Brightness.light : Brightness.dark,
              statusBarBrightness: isDark ? Brightness.dark : Brightness.light,
              systemNavigationBarColor: Theme.of(context).scaffoldBackgroundColor,
              systemNavigationBarIconBrightness: isDark ? Brightness.light : Brightness.dark,
              systemNavigationBarDividerColor: Theme.of(context).dividerColor,
            ));
            return Directionality(
              textDirection: TextDirection.ltr,
              child: child!,
            );
          },
          home: const ScanScreen(),
        );
      },
    );
  }
}
