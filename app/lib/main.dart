import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'app_navigator.dart';
import 'ble/riftlink_ble.dart';
import 'ble/riftlink_ble_scope.dart';
import 'l10n/app_localizations.dart';
import 'locale_notifier.dart';
import 'notifications/local_notifications_service.dart';
import 'screens/scan_screen.dart';
import 'chat/chat_event_ingestor.dart';
import 'chat/chat_repository.dart';
import 'theme/app_theme.dart';
import 'theme/theme_notifier.dart';
import 'theme/design_tokens.dart';
import 'connection/reconnect_overlay_controller.dart';
import 'connection/transport_reconnect_manager.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();

  await AppLocalizations.loadLocale();
  localeNotifier.value = AppLocalizations.currentLocale;
  await loadThemeMode();
  await LocalNotificationsService.init();
  runApp(const RiftLinkApp());
}

class RiftLinkApp extends StatefulWidget {
  const RiftLinkApp({super.key});

  @override
  State<RiftLinkApp> createState() => _RiftLinkAppState();
}

class _RiftLinkAppState extends State<RiftLinkApp> {
  final RiftLinkBle _ble = RiftLinkBle();
  ChatEventIngestor? _ingestor;
  TransportReconnectManager? _transportReconnectManager;

  @override
  void initState() {
    super.initState();
    ChatRepository.instance.init();
    _ingestor = ChatEventIngestor(ble: _ble, repo: ChatRepository.instance);
    _ingestor!.start();
    _transportReconnectManager = TransportReconnectManager(_ble)..start();
    transportReconnectManager = _transportReconnectManager;
    themeModeNotifier.addListener(_onSettingsChanged);
    localeNotifier.addListener(_onSettingsChanged);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _updateSystemUi();
    });
  }

  void _updateSystemUi() {
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
  }

  void _onSettingsChanged() {
    if (mounted) {
      setState(() {});
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) _updateSystemUi();
      });
    }
  }

  @override
  void dispose() {
    _ingestor?.stop();
    _transportReconnectManager?.stop();
    transportReconnectManager = null;
    themeModeNotifier.removeListener(_onSettingsChanged);
    localeNotifier.removeListener(_onSettingsChanged);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return RiftLinkBleScope(
      ble: _ble,
      child: MaterialApp(
        navigatorKey: navigatorKey,
        scaffoldMessengerKey: scaffoldMessengerKey,
        title: 'RiftLink',
        locale: localeNotifier.value,
        supportedLocales: const [Locale('en'), Locale('ru')],
        localizationsDelegates: const [
          GlobalMaterialLocalizations.delegate,
          GlobalWidgetsLocalizations.delegate,
          GlobalCupertinoLocalizations.delegate,
        ],
        theme: AppTheme.light,
        darkTheme: AppTheme.dark,
        themeMode: themeModeNotifier.value,
        themeAnimationDuration: Duration.zero,
        builder: (context, child) => Directionality(
          textDirection: TextDirection.ltr,
          child: Stack(
            fit: StackFit.expand,
            children: [
              child!,
              AnimatedBuilder(
                animation: reconnectOverlayController,
                builder: (context, _) {
                  if (!reconnectOverlayController.visible) {
                    return const SizedBox.shrink();
                  }
                  final l = context.l10n;
                  final p = context.palette;
                  return ColoredBox(
                    color: Colors.black54,
                    child: Center(
                      child: ConstrainedBox(
                        constraints: const BoxConstraints(maxWidth: 420),
                        child: Card(
                          color: p.card,
                          margin: const EdgeInsets.symmetric(horizontal: 20),
                          child: Padding(
                            padding: const EdgeInsets.all(24),
                            child: Column(
                              mainAxisSize: MainAxisSize.min,
                              children: [
                                Icon(
                                  Icons.portable_wifi_off_rounded,
                                  size: 38,
                                  color: p.error,
                                ),
                                const SizedBox(height: 12),
                                Text(
                                  l.tr('reconnect_failed_modal_title'),
                                  textAlign: TextAlign.center,
                                  style: TextStyle(
                                    color: p.onSurface,
                                    fontSize: 18,
                                    fontWeight: FontWeight.w700,
                                  ),
                                ),
                                const SizedBox(height: 8),
                                Text(
                                  l.tr('reconnect_failed_modal_subtitle'),
                                  textAlign: TextAlign.center,
                                  style: TextStyle(
                                    color: p.onSurfaceVariant,
                                    height: 1.35,
                                  ),
                                ),
                                const SizedBox(height: 16),
                                SizedBox(
                                  width: double.infinity,
                                  child: FilledButton.icon(
                                    onPressed: reconnectOverlayController.busy
                                        ? null
                                        : () => reconnectOverlayController.runRetry(_ble),
                                    icon: const Icon(Icons.refresh_rounded),
                                    label: Text(l.tr('reconnect_action_retry')),
                                  ),
                                ),
                                const SizedBox(height: 8),
                                SizedBox(
                                  width: double.infinity,
                                  child: OutlinedButton.icon(
                                    onPressed: reconnectOverlayController.busy
                                        ? null
                                        : () async {
                                            _transportReconnectManager?.suppressAutoReconnectUntilNextConnection();
                                            reconnectOverlayController.hide();
                                            navigatorKey.currentState?.pushAndRemoveUntil(
                                              appPageRoute(
                                                ScanScreen(
                                                  initialMessage: l.tr('reconnect_failed'),
                                                ),
                                              ),
                                              (route) => false,
                                            );
                                            unawaited(
                                              _ble.disconnectTransport().timeout(
                                                const Duration(seconds: 2),
                                                onTimeout: () {},
                                              ),
                                            );
                                          },
                                    icon: const Icon(Icons.login_rounded),
                                    label: Text(l.tr('reconnect_action_go_login')),
                                  ),
                                ),
                              ],
                            ),
                          ),
                        ),
                      ),
                    ),
                  );
                },
              ),
              if (_transportReconnectManager != null)
                ValueListenableBuilder<TransportReconnectUiState>(
                  valueListenable: _transportReconnectManager!.uiState,
                  builder: (context, state, _) {
                    if (!state.reconnecting) return const SizedBox.shrink();
                    final l = context.l10n;
                    final p = context.palette;
                    return ColoredBox(
                      color: Colors.black54,
                      child: Center(
                        child: ConstrainedBox(
                          constraints: const BoxConstraints(maxWidth: 460),
                          child: Card(
                            margin: const EdgeInsets.symmetric(horizontal: 18),
                            clipBehavior: Clip.antiAlias,
                            color: p.card.withOpacity(0.97),
                            shape: RoundedRectangleBorder(
                              borderRadius: BorderRadius.circular(16),
                              side: BorderSide(color: p.divider),
                            ),
                            child: Column(
                              mainAxisSize: MainAxisSize.min,
                              children: [
                                Padding(
                                  padding: const EdgeInsets.fromLTRB(16, 16, 12, 12),
                                  child: Row(
                                    children: [
                                      SizedBox(
                                        width: 18,
                                        height: 18,
                                        child: CircularProgressIndicator(
                                          strokeWidth: 2.4,
                                          color: p.primary,
                                        ),
                                      ),
                                      const SizedBox(width: 12),
                                      Expanded(
                                        child: Column(
                                          crossAxisAlignment: CrossAxisAlignment.start,
                                          mainAxisSize: MainAxisSize.min,
                                          children: [
                                            Text(
                                              l.tr('reconnect_failed'),
                                              maxLines: 1,
                                              overflow: TextOverflow.ellipsis,
                                              style: TextStyle(
                                                color: p.onSurface,
                                                fontSize: 15,
                                                fontWeight: FontWeight.w700,
                                              ),
                                            ),
                                            const SizedBox(height: 2),
                                            Text(
                                              l.tr('reconnecting_compact'),
                                              maxLines: 1,
                                              overflow: TextOverflow.ellipsis,
                                              style: TextStyle(
                                                color: p.onSurfaceVariant,
                                                fontSize: 13.5,
                                                fontWeight: FontWeight.w500,
                                              ),
                                            ),
                                          ],
                                        ),
                                      ),
                                      const SizedBox(width: 10),
                                      Container(
                                        padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 4),
                                        decoration: BoxDecoration(
                                          color: p.primary.withOpacity(0.14),
                                          borderRadius: BorderRadius.circular(999),
                                        ),
                                        child: Text(
                                          '${state.attempt}/3',
                                          style: TextStyle(
                                            color: p.primary,
                                            fontSize: 13,
                                            fontWeight: FontWeight.w700,
                                          ),
                                        ),
                                      ),
                                    ],
                                  ),
                                ),
                                ClipRRect(
                                  borderRadius: const BorderRadius.only(
                                    bottomLeft: Radius.circular(16),
                                    bottomRight: Radius.circular(16),
                                  ),
                                  child: LinearProgressIndicator(
                                    minHeight: 4,
                                    value: state.attemptProgress,
                                    backgroundColor: p.divider.withOpacity(0.35),
                                    valueColor: AlwaysStoppedAnimation<Color>(p.primary),
                                  ),
                                ),
                              ],
                            ),
                          ),
                        ),
                      ),
                    );
                  },
                ),
            ],
          ),
        ),
        home: const ScanScreen(),
      ),
    );
  }
}
