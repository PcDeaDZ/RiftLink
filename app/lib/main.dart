import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'l10n/app_localizations.dart';
import 'locale_notifier.dart';
import 'screens/scan_screen.dart';
import 'theme/app_theme.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();

  // Тёмная тема в стиле MeshCore
  SystemChrome.setSystemUIOverlayStyle(const SystemUiOverlayStyle(
    statusBarColor: Colors.transparent,
    statusBarIconBrightness: Brightness.light,
    statusBarBrightness: Brightness.dark,
    systemNavigationBarColor: Color(0xFF121212),
    systemNavigationBarIconBrightness: Brightness.light,
    systemNavigationBarDividerColor: Color(0xFF2D2D2D),
  ));

  await AppLocalizations.loadLocale();
  localeNotifier.value = AppLocalizations.currentLocale;
  runApp(const RiftLinkApp());
}

class RiftLinkApp extends StatelessWidget {
  const RiftLinkApp({super.key});

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: localeNotifier,
      builder: (_, __) => MaterialApp(
        title: 'RiftLink',
        locale: localeNotifier.value,
        supportedLocales: const [Locale('en'), Locale('ru')],
        theme: AppTheme.dark,
        home: const ScanScreen(),
      ),
    );
  }
}
