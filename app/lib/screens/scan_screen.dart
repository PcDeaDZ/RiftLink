import 'dart:async';

import 'package:connectivity_plus/connectivity_plus.dart';
import 'package:flutter/material.dart';
import 'package:package_info_plus/package_info_plus.dart';
import '../theme/app_theme.dart';
import '../theme/design_tokens.dart';
import '../widgets/app_primitives.dart';
import '../widgets/app_snackbar.dart';
import '../prefs/mesh_prefs.dart';
import '../widgets/mesh_background.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import '../ble/riftlink_ble.dart';
import '../ble/riftlink_ble_scope.dart';
import '../app_navigator.dart';
import '../l10n/app_localizations.dart';
import '../locale_notifier.dart';
import '../theme/theme_notifier.dart';
import '../recent_devices/recent_devices_service.dart';
import '../wifi/mdns_discovery.dart';
import '../widgets/rift_dialogs.dart';
import 'chat_screen.dart';
import 'debug_screen.dart';

class ScanScreen extends StatefulWidget {
  const ScanScreen({super.key, this.initialMessage});
  final String? initialMessage;
  @override
  State<ScanScreen> createState() => _ScanScreenState();
}

/// Заголовок секции списка (не экрана): 16 / w600.
TextStyle _sectionHeaderStyle(BuildContext context) {
  final p = context.palette;
  return AppTypography.screenTitleBase().copyWith(
    fontSize: 16,
    color: p.onSurface,
  );
}

/// Подпись вторичного текста (подсказки, подписи полей).
TextStyle _captionStyle(BuildContext context) {
  return AppTypography.labelBase().copyWith(
    color: context.palette.onSurfaceVariant,
  );
}

/// Очень мелкий поясняющий текст (баннеры, метки списков).
TextStyle _microCaptionStyle(BuildContext context) {
  return AppTypography.chipBase().copyWith(
    fontWeight: FontWeight.w500,
    color: context.palette.onSurfaceVariant,
  );
}

String _formatBleError(BuildContext context, Object e) {
  final l10n = context.l10n;
  final s = e.toString().toLowerCase();
  if (s.contains('timeout') || s.contains('timed out')) return l10n.tr('ble_timeout');
  if (s.contains('connection') && s.contains('refused')) return l10n.tr('ble_refused');
  if (s.contains('device') && s.contains('disconnect')) return l10n.tr('ble_disconnect');
  if (s.contains('bluetooth') && s.contains('turn')) return l10n.tr('ble_turn_on');
  if (s.contains('permission') || s.contains('denied')) return l10n.tr('ble_permission');
  return e.toString();
}

class _ScanScreenState extends State<ScanScreen> with TickerProviderStateMixin {
  RiftLinkBle get _ble => RiftLinkBleScope.of(context);
  final Connectivity _connectivity = Connectivity();
  bool _scanning = false;
  bool _meshAnimationEnabled = true;
  AnimationController? _meshAnimController;
  int _titleTapCount = 0;
  List<ScanResult> _results = [];
  List<RecentDevice> _recent = [];
  String? _error;
  String? _connectingToRemoteId;
  String? _connectingToDeviceName;
  StreamSubscription? _scanSub;
  Completer<void>? _scanCompleter;
  bool _scanStoppedByUser = false;
  bool _wifiConnecting = false;
  int _transportTab = 0; // 0=BLE, 1=Wi-Fi
  late final TextEditingController _wifiIpController;
  List<String> _recentWifiIps = [];
  List<MdnsNode> _mdnsNodes = [];
  bool _wifiDiscovering = false;
  bool _wifiNetworkAvailable = false;
  StreamSubscription<List<ConnectivityResult>>? _connectivitySub;
  int? _wifiActionFlash; // 0 = last IP, 1 = mDNS
  Timer? _wifiActionFlashTimer;
  bool _wifiIpTouched = false;
  Map<String, WifiDeviceMeta> _wifiMeta = {};

  @override
  void initState() {
    super.initState();
    _wifiIpController = TextEditingController(text: '192.168.4.1');
    _wifiIpController.addListener(_onWifiIpChanged);
    _bindConnectivity();
    _loadRecent();
    _loadRecentWifiIps();
    _loadMeshPrefs();
    if (widget.initialMessage != null) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) {
          showAppSnackBar(context, widget.initialMessage!, kind: AppSnackKind.error);
        }
      });
    }
  }

  @override
  void dispose() {
    _scanSub?.cancel();
    _connectivitySub?.cancel();
    _wifiActionFlashTimer?.cancel();
    _scanSub = null;
    RiftLinkBle.stopScan();  // fire-and-forget, очистка при закрытии
    _wifiIpController.removeListener(_onWifiIpChanged);
    _wifiIpController.dispose();
    _meshAnimController?.dispose();
    super.dispose();
  }

  bool _isWifiConnected(List<ConnectivityResult> results) {
    return results.contains(ConnectivityResult.wifi);
  }

  Future<void> _bindConnectivity() async {
    try {
      final current = await _connectivity.checkConnectivity();
      if (!mounted) return;
      setState(() => _wifiNetworkAvailable = _isWifiConnected(current));
    } catch (_) {}
    _connectivitySub = _connectivity.onConnectivityChanged.listen((results) {
      if (!mounted) return;
      final available = _isWifiConnected(results);
      if (!available && _transportTab == 1) {
        setState(() {
          _transportTab = 0;
          _wifiNetworkAvailable = false;
          _mdnsNodes = [];
        });
        _snack(context.l10n.tr('wifi_tab_locked'));
        return;
      }
      setState(() => _wifiNetworkAvailable = available);
    });
  }

  Future<void> _loadMeshPrefs() async {
    final enabled = await MeshPrefs.getAnimationEnabled();
    if (mounted) {
      setState(() => _meshAnimationEnabled = enabled);
      _updateMeshAnimation(enabled);
    }
  }

  void _updateMeshAnimation(bool enabled) {
    if (enabled) {
      _meshAnimController ??= AnimationController(
        vsync: this,
        duration: const Duration(seconds: 4),
      )..repeat();
    } else {
      _meshAnimController?.stop();
      _meshAnimController?.dispose();
      _meshAnimController = null;
    }
    if (mounted) setState(() {});
  }

  Future<void> _loadRecent() async {
    final list = await RecentDevicesService.load();
    if (mounted) setState(() => _recent = list);
  }

  Future<void> _loadRecentWifiIps() async {
    final list = await RecentDevicesService.loadRecentWifiIps();
    final meta = await RecentDevicesService.loadAllWifiMeta();
    if (!mounted) return;
    setState(() { _recentWifiIps = list; _wifiMeta = meta; });
    if (list.isNotEmpty && (_wifiIpController.text.trim().isEmpty || _wifiIpController.text.trim() == '192.168.4.1')) {
      _wifiIpController.text = list.first;
      _wifiIpController.selection = TextSelection.collapsed(offset: _wifiIpController.text.length);
    }
  }

  void _onWifiIpChanged() {
    if (!_wifiIpTouched) {
      setState(() => _wifiIpTouched = true);
      return;
    }
    if (mounted) setState(() {});
  }

  bool get _wifiIpValid {
    final ip = _wifiIpController.text.trim();
    final ipv4 = RegExp(r'^((25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.){3}(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)$');
    return ipv4.hasMatch(ip);
  }

  String? _wifiIpFieldError(BuildContext context) {
    final ip = _wifiIpController.text.trim();
    if (!_wifiIpTouched) return null;
    if (ip.isEmpty) return context.l10n.tr('wifi_ip_required');
    if (!_wifiIpValid) return context.l10n.tr('wifi_ip_invalid');
    return null;
  }

  void _useLastWifiIp() {
    if (_recentWifiIps.isEmpty) return;
    final last = _recentWifiIps.first;
    _wifiIpController.text = last;
    _wifiIpController.selection = TextSelection.collapsed(offset: last.length);
    setState(() => _wifiIpTouched = true);
  }

  void _useWifiIp(String ip) {
    _wifiIpController.text = ip;
    _wifiIpController.selection = TextSelection.collapsed(offset: ip.length);
    setState(() => _wifiIpTouched = true);
  }

  Future<void> _removeWifiIp(String ip) async {
    await RecentDevicesService.removeRecentWifiIp(ip);
    if (!mounted) return;
    final updated = await RecentDevicesService.loadRecentWifiIps();
    if (!mounted) return;
    setState(() => _recentWifiIps = updated);
  }

  void _flashWifiAction(int index) {
    _wifiActionFlashTimer?.cancel();
    setState(() => _wifiActionFlash = index);
    _wifiActionFlashTimer = Timer(const Duration(milliseconds: 240), () {
      if (!mounted) return;
      setState(() => _wifiActionFlash = null);
    });
  }

  Future<void> _discoverWifiNodes() async {
    if (!_wifiNetworkAvailable) {
      _snack(context.l10n.tr('wifi_tab_locked'));
      return;
    }
    if (_wifiDiscovering) return;
    setState(() {
      _wifiDiscovering = true;
      _error = null;
    });
    try {
      final nodes = await discoverRiftLinkMdns();
      if (!mounted) return;
      setState(() {
        _mdnsNodes = nodes;
        if (nodes.isNotEmpty) {
          _wifiIpController.text = nodes.first.ip;
          _wifiIpController.selection = TextSelection.collapsed(offset: nodes.first.ip.length);
          _wifiIpTouched = true;
        }
      });
      if (nodes.isEmpty) {
        _snack(context.l10n.tr('wifi_mdns_none'));
      } else {
        _snack('${context.l10n.tr('wifi_mdns_found')}: ${nodes.length}');
      }
    } catch (e) {
      if (!mounted) return;
      setState(() => _error = '${context.l10n.tr('wifi_mdns_scan')}: $e');
    } finally {
      if (mounted) setState(() => _wifiDiscovering = false);
    }
  }

  void _snack(String message) {
    showAppSnackBar(context, message);
  }

  Future<void> _startScan({String? connectToRemoteId, String? connectToDeviceName}) async {
    String displayName = (connectToDeviceName != null && connectToDeviceName.isNotEmpty)
        ? connectToDeviceName
        : '';
    if (displayName.isEmpty && connectToRemoteId != null && connectToRemoteId!.isNotEmpty) {
      displayName = connectToRemoteId!.length > 16 ? connectToRemoteId!.substring(connectToRemoteId!.length - 16) : connectToRemoteId!;
    }
    if (displayName.isEmpty && connectToRemoteId != null) {
      displayName = connectToRemoteId!;
    }
    // При подключении к недавнему: не перезаписывать имя пустым — сохранить из тапа
    final keepExisting = connectToRemoteId != null && displayName.isEmpty && (_connectingToDeviceName != null && _connectingToDeviceName!.isNotEmpty);
    final nameToShow = connectToRemoteId != null ? (displayName.isNotEmpty ? displayName : (keepExisting ? _connectingToDeviceName! : connectToRemoteId)) : null;
    setState(() { _scanning = true; _scanStoppedByUser = false; _results = []; _connectingToRemoteId = connectToRemoteId; _connectingToDeviceName = nameToShow; _error = null; });
    try {
      if (!await FlutterBluePlus.isSupported) {
        throw Exception('Bluetooth not supported on this device');
      }
      if (await Permission.bluetoothScan.isDenied) await Permission.bluetoothScan.request();
      if (await Permission.bluetoothConnect.isDenied) await Permission.bluetoothConnect.request();
      if (await Permission.locationWhenInUse.isDenied) await Permission.locationWhenInUse.request();
      if (!await FlutterBluePlus.isOn) {
        await FlutterBluePlus.turnOn();
        await Future<void>.delayed(const Duration(milliseconds: 800));
      }
      await RiftLinkBle.stopScan();
      await Future<void>.delayed(const Duration(milliseconds: 800));
      _scanSub = FlutterBluePlus.scanResults.listen((results) {
        if (!mounted) return;
        final found = results.where(RiftLinkBle.isRiftLink).toList();
        for (final r in found) {
          final rStr = r.device.remoteId.toString();
          final match = connectToRemoteId != null && RiftLinkBle.remoteIdsMatch(rStr, connectToRemoteId!);
          if (match) {
            _scanSub?.cancel();
            _scanSub = null;
            RiftLinkBle.stopScan();
            final name = _connectingToDeviceName ?? connectToDeviceName ?? (r.device.advName.isNotEmpty ? r.device.advName
                : r.device.platformName.isNotEmpty ? r.device.platformName
                : r.device.remoteId.toString());
            _connect(r.device, displayName: name.isNotEmpty ? name : r.device.remoteId.toString());
            return;
          }
          if (connectToRemoteId == null && !_results.any((x) => x.device.remoteId == r.device.remoteId)) {
            setState(() { _results = [..._results, r]; if (_error != null) _error = null; });
          }
        }
      });
      const scanDuration = Duration(seconds: 12);
      _scanCompleter = Completer<void>();
      await RiftLinkBle.startScan(timeout: scanDuration);
      await Future.any([Future.delayed(scanDuration), _scanCompleter!.future]);
    } catch (e) {
      if (mounted) setState(() => _error = _formatBleError(context, e));
    } finally {
      await _scanSub?.cancel();
      _scanSub = null;
      await RiftLinkBle.stopScan();
      if (mounted) {
        setState(() {
          _scanning = false;
          _connectingToRemoteId = null;
          _connectingToDeviceName = null;
          if (_error == null && _results.isEmpty && connectToRemoteId == null && !_scanStoppedByUser) {
            _error = context.l10n.tr('scan_no_devices');
          }
        });
      }
    }
  }

  Future<void> _connect(BluetoothDevice device, {String? displayName}) async {
    final name = displayName ?? (device.advName.isNotEmpty ? device.advName
        : device.platformName.isNotEmpty ? device.platformName
        : device.remoteId.toString().length > 12 ? device.remoteId.toString().substring(device.remoteId.toString().length - 12) : device.remoteId.toString());
    final resolvedName = name.isNotEmpty ? name : context.l10n.tr('device');
    setState(() {
      _error = null;
      _connectingToRemoteId = device.remoteId.toString();
      _connectingToDeviceName = resolvedName;
    });
    try {
      final ok = await _ble.connect(device);
      if (!mounted) return;
      if (ok) {
        final rid = device.remoteId.toString();
        final nodeKey = RiftLinkBle.nodeIdHintFromDevice(device) ?? rid;
        unawaited(RecentDevicesService.addOrUpdate(remoteId: rid, nodeId: nodeKey, nickname: null));
        Navigator.of(context).pushReplacement(MaterialPageRoute(builder: (_) => ChatScreen(ble: _ble)));
      } else {
        setState(() { _error = context.l10n.tr('ble_no_service'); _connectingToRemoteId = null; _connectingToDeviceName = null; });
      }
    } catch (e) {
      if (mounted) setState(() { _error = _formatBleError(context, e); _connectingToRemoteId = null; _connectingToDeviceName = null; });
    }
  }

  Future<void> _connectWifi() async {
    if (!_wifiNetworkAvailable) {
      setState(() => _error = context.l10n.tr('wifi_tab_locked'));
      return;
    }
    final ip = _wifiIpController.text.trim();
    if (!_wifiIpValid) {
      setState(() {
        _wifiIpTouched = true;
        _error = context.l10n.tr(ip.isEmpty ? 'wifi_ip_required' : 'wifi_ip_invalid');
      });
      return;
    }
    if (_scanning) {
      _scanStoppedByUser = true;
      _scanCompleter?.complete();
    }
    setState(() {
      _wifiConnecting = true;
      _error = null;
      _connectingToRemoteId = null;
      _connectingToDeviceName = null;
    });
    try {
      final ok = await _ble.connectWifi(ip);
      if (!mounted) return;
      if (ok) {
        await RecentDevicesService.addRecentWifiIp(ip);
        if (mounted) {
          final updated = await RecentDevicesService.loadRecentWifiIps();
          if (mounted) setState(() => _recentWifiIps = updated);
        }
        Navigator.of(context).pushReplacement(MaterialPageRoute(builder: (_) => ChatScreen(ble: _ble)));
      } else {
        setState(() => _error = context.l10n.tr('wifi_connect_failed'));
      }
    } catch (e) {
      if (!mounted) return;
      setState(() => _error = '${context.l10n.tr('wifi_connect_failed')}: $e');
    } finally {
      if (mounted) setState(() => _wifiConnecting = false);
    }
  }

  void _connectToRecent(RecentDevice dev, {String? displayLabel}) {
    final name = (displayLabel != null && displayLabel.isNotEmpty)
        ? displayLabel
        : (dev.displayName.isNotEmpty ? dev.displayName : dev.nodeId.isNotEmpty ? dev.nodeId : dev.remoteId);
    final connectName = name.isNotEmpty ? name : dev.remoteId;
    _startScan(connectToRemoteId: dev.remoteId, connectToDeviceName: connectName);
  }

  Future<void> _confirmForgetDevice(RecentDevice d) async {
    final l10n = context.l10n;
    final confirm = await showRiftConfirmDialog(
      context: context,
      title: l10n.tr('forget_device'),
      message: l10n.tr('forget_device_confirm', {'name': d.displayName}),
      cancelText: l10n.tr('cancel'),
      confirmText: l10n.tr('delete'),
      danger: true,
      icon: Icons.delete_outline_rounded,
    );
    if (confirm == true && mounted) {
      await RecentDevicesService.remove(d.remoteId);
      _loadRecent();
    }
  }

  void _showAbout() {
    final l10n = context.l10n;
    final dim = Theme.of(context).brightness == Brightness.dark ? 0.68 : 0.45;
    showDialog(
      context: context,
      barrierColor: Colors.black.withOpacity(dim),
      barrierDismissible: true,
      builder: (ctx) => _AboutDialog(l10n: l10n),
    );
  }

  void _showQuickSettings() {
    showModalBottomSheet<void>(
      context: context,
      backgroundColor: Colors.transparent,
      shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(16))),
      builder: (ctx) {
        return ListenableBuilder(
          listenable: Listenable.merge([themeModeNotifier, localeNotifier]),
          builder: (ctx, _) {
            final p = ctx.palette;
            final l10n = ctx.l10n;
            final themeIdx = switch (themeModeNotifier.value) { ThemeMode.system => 0, ThemeMode.light => 1, ThemeMode.dark => 2 };
            final curLang = AppLocalizations.currentLocale.languageCode == 'ru' ? 0 : 1;
            return Container(
              decoration: BoxDecoration(
                color: p.card,
                borderRadius: const BorderRadius.vertical(top: Radius.circular(16)),
              ),
              child: SafeArea(
                child: Padding(
                  padding: const EdgeInsets.fromLTRB(AppSpacing.lg, AppSpacing.lg, AppSpacing.lg, AppSpacing.sm),
                  child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.stretch, children: [
                    Center(child: Container(width: 36, height: 3, decoration: BoxDecoration(color: p.onSurfaceVariant.withOpacity(0.3), borderRadius: BorderRadius.circular(2)))),
                    const SizedBox(height: AppSpacing.lg),
                    Text(l10n.tr('theme'), style: AppTypography.labelBase().copyWith(color: p.onSurfaceVariant, fontWeight: FontWeight.w600)),
                    const SizedBox(height: AppSpacing.sm),
                    RiftSegmentedBar(
                      labels: [l10n.tr('theme_system'), l10n.tr('theme_light'), l10n.tr('theme_dark')],
                      selectedIndex: themeIdx,
                      onSelected: (i) {
                        final mode = switch (i) { 0 => ThemeMode.system, 1 => ThemeMode.light, _ => ThemeMode.dark };
                        setThemeMode(mode);
                      },
                    ),
                    const SizedBox(height: AppSpacing.lg),
                    Text(l10n.tr('lang'), style: AppTypography.labelBase().copyWith(color: p.onSurfaceVariant, fontWeight: FontWeight.w600)),
                    const SizedBox(height: AppSpacing.sm),
                    RiftSegmentedBar(
                      labels: [l10n.tr('lang_ru'), l10n.tr('lang_en')],
                      selectedIndex: curLang,
                      onSelected: (i) {
                        final target = i == 0 ? 'ru' : 'en';
                        if (AppLocalizations.currentLocale.languageCode != target) {
                          AppLocalizations.switchLocale(() { localeNotifier.value = AppLocalizations.currentLocale; });
                        }
                      },
                    ),
                    const SizedBox(height: AppSpacing.sm),
                    Divider(color: p.divider),
                    ListTile(
                      contentPadding: EdgeInsets.zero,
                      leading: Icon(Icons.info_outline_rounded, color: p.onSurfaceVariant),
                      title: Text(l10n.tr('about'), style: TextStyle(color: p.onSurface)),
                      trailing: Icon(Icons.chevron_right, color: p.onSurfaceVariant),
                      onTap: () { Navigator.pop(ctx); _showAbout(); },
                    ),
                  ]),
                ),
              ),
            );
          },
        );
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    final l10n = context.l10n;
    return Scaffold(
      backgroundColor: context.palette.surface,
      appBar: riftAppBar(
        context,
        title: '',
        titleWidget: GestureDetector(
          onTap: () {
            _titleTapCount++;
            if (_titleTapCount >= 5) {
              _titleTapCount = 0;
              Navigator.push(context, MaterialPageRoute(builder: (_) => const DebugScreen()));
            }
          },
          child: Text(l10n.tr('app_title')),
        ),
        actions: [
          IconButton(
            icon: const Icon(Icons.settings_outlined),
            tooltip: l10n.tr('settings'),
            onPressed: _showQuickSettings,
          ),
        ],
      ),
      body: Stack(
        fit: StackFit.expand,
        children: [
          Positioned.fill(
            child: IgnorePointer(
              child: ColoredBox(
                color: context.palette.surface,
                child: ListenableBuilder(
                  listenable: _meshAnimationEnabled && _meshAnimController != null ? _meshAnimController! : const AlwaysStoppedAnimation(0),
                  builder: (_, __) => CustomPaint(
                    painter: MeshBackgroundPainter(
                      progress: _meshAnimController?.value ?? 0,
                      animated: _meshAnimationEnabled,
                      palette: context.palette,
                    ),
                  ),
                ),
              ),
            ),
          ),
          Material(
            color: Colors.transparent,
            child: SafeArea(
              child: SingleChildScrollView(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.center,
                  children: [
                  SizedBox(height: AppSpacing.xl),
                  Icon(_transportTab == 0 ? Icons.bluetooth_searching : Icons.wifi_find_rounded, size: 56, color: context.palette.primary),
                  SizedBox(height: AppSpacing.md),
                  AppScreenTitle(
                    _transportTab == 0 ? l10n.tr('find_device') : l10n.tr('wifi_connect_title'),
                  ),
                  SizedBox(height: AppSpacing.md),
                  Padding(
                    padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
                    child: RiftSegmentedBar(
                      labels: [l10n.tr('transport_ble'), l10n.tr('transport_wifi')],
                      selectedIndex: _transportTab,
                      enabled: _transportTab == 1 || _wifiNetworkAvailable,
                      onSelected: (i) {
                        if (i == 1 && !_wifiNetworkAvailable) {
                          _snack(l10n.tr('wifi_tab_locked'));
                          return;
                        }
                        if (i == 1 && _scanning) {
                          _scanStoppedByUser = true;
                          _scanCompleter?.complete();
                        }
                        setState(() => _transportTab = i);
                        if (i == 1 && _recentWifiIps.isNotEmpty && (_wifiIpController.text.trim().isEmpty || _wifiIpController.text.trim() == '192.168.4.1')) {
                          _useLastWifiIp();
                        }
                      },
                    ),
                  ),
                  SizedBox(height: AppSpacing.md),
                  if (!_wifiNetworkAvailable)
                    Padding(
                      padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
                      child: Text(
                        l10n.tr('wifi_tab_locked'),
                        textAlign: TextAlign.center,
                        style: _microCaptionStyle(context),
                      ),
                    ),
                  if (!_wifiNetworkAvailable) SizedBox(height: AppSpacing.sm),
                  if (_error != null)
                    Padding(
                      padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
                      child: DecoratedBox(
                        decoration: BoxDecoration(
                          color: context.palette.error.withOpacity(0.12),
                          borderRadius: BorderRadius.circular(AppRadius.sm),
                          border: Border.all(color: context.palette.error.withOpacity(0.45)),
                        ),
                        child: Padding(
                          padding: EdgeInsets.all(AppSpacing.md),
                          child: Column(
                            mainAxisSize: MainAxisSize.min,
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Row(
                                children: [
                                  Icon(Icons.error_outline, color: context.palette.error, size: AppSpacing.lg + 4),
                                  SizedBox(width: AppSpacing.sm),
                                  Expanded(
                                    child: Text(
                                      _error!,
                                      style: AppTypography.labelBase().copyWith(color: context.palette.error, height: AppTypography.bodyHeight),
                                    ),
                                  ),
                                ],
                              ),
                              if (_error!.toLowerCase().contains('permission') || _error!.toLowerCase().contains('denied')) ...[
                                SizedBox(height: AppSpacing.sm),
                                TextButton.icon(
                                  onPressed: () => openAppSettings(),
                                  icon: Icon(Icons.settings, size: 18, color: context.palette.primary),
                                  label: Text(l10n.tr('open_settings'), style: TextStyle(color: context.palette.primary)),
                                ),
                              ],
                            ],
                          ),
                        ),
                      ),
                    ),
                  if (_error != null) SizedBox(height: AppSpacing.md),
                  SizedBox(height: AppSpacing.md),
                  if (_transportTab == 0) ...[
                    Padding(
                      padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
                      child: AppPrimaryButton(
                        onPressed: _connectingToRemoteId != null ? null : () {
                          if (_scanning) {
                            _scanStoppedByUser = true;
                            _scanCompleter?.complete();
                            return;
                          }
                          _startScan();
                        },
                        child: Row(
                          mainAxisAlignment: MainAxisAlignment.center,
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            SizedBox(
                              width: 20,
                              height: 20,
                              child: (_scanning || _connectingToRemoteId != null) && _connectingToRemoteId == null
                                  ? CircularProgressIndicator(
                                      strokeWidth: 2,
                                      color: Theme.of(context).colorScheme.onPrimary,
                                    )
                                  : _connectingToRemoteId != null
                                      ? Icon(Icons.bluetooth_searching, color: Theme.of(context).colorScheme.onPrimary, size: 20)
                                      : Icon(Icons.search, color: Theme.of(context).colorScheme.onPrimary, size: 20),
                            ),
                            SizedBox(width: AppSpacing.sm),
                            Flexible(
                              child: Builder(
                                builder: (_) {
                                  String labelText = l10n.tr('search_devices');
                                  if (_connectingToRemoteId != null) {
                                    String name = _connectingToDeviceName ?? '';
                                    final rid = _connectingToRemoteId!;
                                    if (name.isEmpty) {
                                      final norm = (String s) => s.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();
                                      final d = _recent.where((x) => norm(x.remoteId) == norm(rid)).firstOrNull;
                                      name = d != null ? (d.displayName.isNotEmpty ? d.displayName : d.nodeId.isNotEmpty ? d.nodeId : d.remoteId) : '';
                                    }
                                    if (name.isEmpty) name = rid.length > 16 ? rid.substring(rid.length - 16) : rid;
                                    if (name.isEmpty) name = l10n.tr('device');
                                    labelText = l10n.tr('connecting_to', {'name': name});
                                  } else if (_scanning) {
                                    labelText = l10n.tr('stop_scan');
                                  }
                                  return Text(
                                    labelText,
                                    style: AppTypography.bodyBase().copyWith(color: Theme.of(context).colorScheme.onPrimary),
                                    textAlign: TextAlign.center,
                                    maxLines: 2,
                                    overflow: TextOverflow.ellipsis,
                                  );
                                },
                              ),
                            ),
                          ],
                        ),
                      ),
                    ),
                  ] else ...[
                    Padding(
                      padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
                      child: Text(
                        l10n.tr('wifi_connect_hint'),
                        textAlign: TextAlign.center,
                        style: _captionStyle(context),
                      ),
                    ),
                    SizedBox(height: AppSpacing.sm + 2),
                    Padding(
                      padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
                      child: TextField(
                        controller: _wifiIpController,
                        enabled: !_wifiConnecting,
                        keyboardType: const TextInputType.numberWithOptions(decimal: true),
                        decoration: InputDecoration(
                          labelText: l10n.tr('wifi_ip'),
                          hintText: '192.168.4.1',
                          errorText: _wifiIpFieldError(context),
                          suffixIcon: IconButton(
                            onPressed: (_wifiConnecting || !_wifiIpValid) ? null : _connectWifi,
                            icon: _wifiConnecting
                                ? SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2, color: context.palette.primary))
                                : Icon(Icons.arrow_forward_rounded, color: (_wifiConnecting || !_wifiIpValid) ? context.palette.onSurfaceVariant.withOpacity(0.3) : context.palette.primary),
                          ),
                        ),
                      ),
                    ),
                    SizedBox(height: AppSpacing.sm),
                    Padding(
                      padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
                      child: AppSectionCard(
                        padding: EdgeInsets.zero,
                        child: SizedBox(
                          height: 44,
                          child: Row(
                            children: [
                              Expanded(
                                child: AnimatedContainer(
                                  duration: AppMotion.standard,
                                  curve: Curves.easeOutCubic,
                                  decoration: BoxDecoration(
                                    color: _wifiActionFlash == 0
                                        ? context.palette.primary.withOpacity(0.14)
                                        : Colors.transparent,
                                    borderRadius: BorderRadius.horizontal(
                                      left: Radius.circular(AppRadius.md - 1),
                                    ),
                                  ),
                                  child: Material(
                                    color: Colors.transparent,
                                    child: InkWell(
                                      borderRadius: BorderRadius.horizontal(
                                        left: Radius.circular(AppRadius.md - 1),
                                      ),
                                      onTap: (_wifiConnecting || _recentWifiIps.isEmpty)
                                          ? null
                                          : () {
                                              _flashWifiAction(0);
                                              _useLastWifiIp();
                                            },
                                      child: Row(
                                        mainAxisAlignment: MainAxisAlignment.center,
                                        children: [
                                          Icon(
                                            Icons.history_rounded,
                                            size: 17,
                                            color: (_wifiConnecting || _recentWifiIps.isEmpty)
                                                ? context.palette.onSurfaceVariant.withOpacity(0.45)
                                                : context.palette.primary,
                                          ),
                                          SizedBox(width: AppSpacing.sm - 2),
                                          Flexible(
                                            child: Text(
                                              l10n.tr('wifi_last_ip'),
                                              maxLines: 1,
                                              overflow: TextOverflow.ellipsis,
                                              style: AppTypography.chipBase().copyWith(
                                                color: (_wifiConnecting || _recentWifiIps.isEmpty)
                                                    ? context.palette.onSurfaceVariant.withOpacity(0.45)
                                                    : context.palette.onSurface,
                                              ),
                                            ),
                                          ),
                                        ],
                                      ),
                                    ),
                                  ),
                                ),
                              ),
                              VerticalDivider(width: 1, thickness: 1, color: context.palette.divider),
                              Expanded(
                                child: AnimatedContainer(
                                  duration: AppMotion.standard,
                                  curve: Curves.easeOutCubic,
                                  decoration: BoxDecoration(
                                    color: _wifiActionFlash == 1
                                        ? context.palette.primary.withOpacity(0.14)
                                        : Colors.transparent,
                                    borderRadius: BorderRadius.horizontal(
                                      right: Radius.circular(AppRadius.md - 1),
                                    ),
                                  ),
                                  child: Material(
                                    color: Colors.transparent,
                                    child: InkWell(
                                      borderRadius: BorderRadius.horizontal(
                                        right: Radius.circular(AppRadius.md - 1),
                                      ),
                                      onTap: (_wifiConnecting || _wifiDiscovering)
                                          ? null
                                          : () {
                                              _flashWifiAction(1);
                                              _discoverWifiNodes();
                                            },
                                      child: Row(
                                        mainAxisAlignment: MainAxisAlignment.center,
                                        children: [
                                          _wifiDiscovering
                                              ? SizedBox(
                                                  width: 14,
                                                  height: 14,
                                                  child: CircularProgressIndicator(
                                                    strokeWidth: 2,
                                                    color: context.palette.primary,
                                                  ),
                                                )
                                              : Icon(
                                                  Icons.travel_explore_rounded,
                                                  size: 17,
                                                  color: _wifiConnecting
                                                      ? context.palette.onSurfaceVariant.withOpacity(0.45)
                                                      : context.palette.primary,
                                                ),
                                          SizedBox(width: AppSpacing.sm - 2),
                                          Flexible(
                                            child: Text(
                                              _wifiDiscovering ? l10n.tr('wifi_mdns_scanning') : l10n.tr('wifi_mdns_scan'),
                                              maxLines: 1,
                                              overflow: TextOverflow.ellipsis,
                                              style: AppTypography.chipBase().copyWith(
                                                color: _wifiConnecting
                                                    ? context.palette.onSurfaceVariant.withOpacity(0.45)
                                                    : context.palette.onSurface,
                                              ),
                                            ),
                                          ),
                                        ],
                                      ),
                                    ),
                                  ),
                                ),
                              ),
                            ],
                          ),
                        ),
                      ),
                    ),
                    if (_mdnsNodes.isNotEmpty) ...[
                      SizedBox(height: AppSpacing.sm),
                      Padding(
                        padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
                        child: Align(
                          alignment: Alignment.centerLeft,
                          child: Text(
                            l10n.tr('wifi_mdns_found'),
                            style: _microCaptionStyle(context),
                          ),
                        ),
                      ),
                      SizedBox(height: AppSpacing.sm - 2),
                      Padding(
                        padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
                        child: Column(children: List.generate(_mdnsNodes.length, (i) {
                          final node = _mdnsNodes[i];
                          return Padding(
                            padding: EdgeInsets.only(bottom: i < _mdnsNodes.length - 1 ? AppSpacing.sm : 0),
                            child: _DeviceCard(
                              icon: Icons.router_rounded,
                              title: node.ip,
                              subtitle: '${node.host}:${node.port}',
                              showDelete: false,
                              onTap: _wifiConnecting ? null : () => _useWifiIp(node.ip),
                            ),
                          );
                        })),
                      ),
                    ],
                    if (_recentWifiIps.isNotEmpty) ...[
                      Padding(
                        padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
                        child: Align(
                          alignment: Alignment.centerLeft,
                          child: Text(
                            l10n.tr('wifi_recent_ips'),
                            style: _microCaptionStyle(context),
                          ),
                        ),
                      ),
                      SizedBox(height: AppSpacing.sm - 2),
                      Padding(
                        padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
                        child: Column(children: List.generate(_recentWifiIps.length, (i) {
                          final ip = _recentWifiIps[i];
                          final meta = _wifiMeta[ip];
                          final title = meta != null && meta.displayName.isNotEmpty ? meta.displayName : ip;
                          final subtitle = meta != null && meta.displayName.isNotEmpty ? ip : null;
                          return Padding(
                            padding: EdgeInsets.only(bottom: i < _recentWifiIps.length - 1 ? AppSpacing.sm : 0),
                            child: Dismissible(
                              key: ValueKey('wifi-ip-$ip'),
                              direction: DismissDirection.endToStart,
                              background: Container(
                                alignment: Alignment.centerRight,
                                padding: const EdgeInsets.symmetric(horizontal: AppSpacing.sm + 2),
                                decoration: BoxDecoration(
                                  color: context.palette.error.withOpacity(0.12),
                                  borderRadius: BorderRadius.circular(AppRadius.sm),
                                  border: Border.all(color: context.palette.error.withOpacity(0.45)),
                                ),
                                child: Icon(Icons.delete_outline_rounded, color: context.palette.error),
                              ),
                              confirmDismiss: (_) async {
                                final l = context.l10n;
                                return await showRiftConfirmDialog(
                                  context: context,
                                  title: l.tr('forget_device'),
                                  message: l.tr('forget_device_confirm', {'name': title}),
                                  cancelText: l.tr('cancel'),
                                  confirmText: l.tr('delete'),
                                  danger: true,
                                  icon: Icons.delete_outline_rounded,
                                );
                              },
                              onDismissed: (_) => _removeWifiIp(ip),
                              child: _DeviceCard(
                                icon: Icons.wifi_rounded,
                                title: title,
                                subtitle: subtitle,
                                showDelete: false,
                                onTap: _wifiConnecting ? null : () {
                                  setState(() {
                                    _wifiIpController.text = ip;
                                    _wifiIpTouched = true;
                                  });
                                  _connectWifi();
                                },
                              ),
                            ),
                          );
                        })),
                      ),
                    ],
                  ],
                  if (_transportTab == 0 && _recent.isNotEmpty) ...[
                    SizedBox(height: AppSpacing.xxl),
                    Padding(
                      padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
                      child: Center(child: Text(l10n.tr('recent_devices'), style: _sectionHeaderStyle(context))),
                    ),
                    SizedBox(height: AppSpacing.sm),
                    Padding(
                      padding: EdgeInsets.symmetric(horizontal: AppSpacing.lg),
                      child: Column(
                        children: _recent.asMap().entries.map((e) {
                          final i = e.key;
                          final d = e.value;
                          final isConnecting = _connectingToRemoteId == d.remoteId;
                          final label = d.displayName.isNotEmpty ? d.displayName : (d.nodeId.isNotEmpty ? d.nodeId : d.remoteId);
                          return Padding(
                            padding: EdgeInsets.only(bottom: i < _recent.length - 1 ? AppSpacing.sm : 0),
                            child: Dismissible(
                              key: ValueKey('ble-recent-${d.remoteId}'),
                              direction: DismissDirection.endToStart,
                              background: Container(
                                alignment: Alignment.centerRight,
                                padding: const EdgeInsets.symmetric(horizontal: AppSpacing.sm + 2),
                                decoration: BoxDecoration(
                                  color: context.palette.error.withOpacity(0.12),
                                  borderRadius: BorderRadius.circular(AppRadius.sm),
                                  border: Border.all(color: context.palette.error.withOpacity(0.45)),
                                ),
                                child: Icon(Icons.delete_outline_rounded, color: context.palette.error),
                              ),
                              confirmDismiss: (_) async {
                                final l10n = context.l10n;
                                return await showRiftConfirmDialog(
                                  context: context,
                                  title: l10n.tr('forget_device'),
                                  message: l10n.tr('forget_device_confirm', {'name': d.displayName}),
                                  cancelText: l10n.tr('cancel'),
                                  confirmText: l10n.tr('delete'),
                                  danger: true,
                                  icon: Icons.delete_outline_rounded,
                                );
                              },
                              onDismissed: (_) async {
                                await RecentDevicesService.remove(d.remoteId);
                                _loadRecent();
                              },
                              child: _DeviceCard(
                                icon: Icons.bluetooth,
                                title: label,
                                subtitle: d.displayName != d.nodeId ? d.nodeId : null,
                                isLoading: isConnecting,
                                showDelete: false,
                                onTap: _scanning ? null : () {
                                  setState(() { _connectingToRemoteId = d.remoteId; _connectingToDeviceName = label; _scanning = true; _scanStoppedByUser = false; _results = []; _error = null; });
                                  WidgetsBinding.instance.addPostFrameCallback((_) { if (mounted && _connectingToRemoteId == d.remoteId) _connectToRecent(d, displayLabel: label); });
                                },
                              ),
                            ),
                          );
                        }).toList(),
                      ),
                    ),
                  ],
                  if (_transportTab == 0 && _recent.isEmpty && !_scanning) ...[
                    SizedBox(height: AppSpacing.md),
                    Padding(
                      padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
                      child: Text(l10n.tr('recent_empty'), textAlign: TextAlign.center, style: _captionStyle(context)),
                    ),
                  ],
                  if (_transportTab == 0)
                    Builder(builder: (_) {
                    final filtered = _results.where((r) => !_recent.any((d) => d.remoteId == r.device.remoteId.toString())).toList();
                    if (filtered.isEmpty) return const SizedBox.shrink();
                    return Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        SizedBox(height: AppSpacing.xxl),
                        Padding(
                          padding: EdgeInsets.symmetric(horizontal: AppSpacing.xxl),
                          child: Center(
                            child: Text(
                              '${l10n.tr('found')} ${filtered.length}',
                              style: _sectionHeaderStyle(context),
                            ),
                          ),
                        ),
                        SizedBox(height: AppSpacing.xs),
                        Divider(height: 1, color: context.palette.divider),
                        Padding(
                          padding: EdgeInsets.symmetric(horizontal: AppSpacing.sm),
                          child: Column(children: List.generate(filtered.length, (i) {
                            final r = filtered[i];
                            final name = r.device.advName.isNotEmpty ? r.device.advName
                                : r.device.platformName.isNotEmpty ? r.device.platformName
                                : r.device.remoteId.toString();
                            return Padding(
                              padding: EdgeInsets.only(bottom: i < filtered.length - 1 ? AppSpacing.sm : 0),
                              child: _DeviceCard(
                                icon: Icons.bluetooth,
                                title: name,
                                subtitle: r.device.remoteId.toString(),
                                showDelete: false,
                                onTap: () => _connect(r.device, displayName: name),
                              ),
                            );
                          })),
                        ),
                      ],
                    );
                    }),
                  if (_results.isEmpty) SizedBox(height: AppSpacing.xxl * 2),
                  SizedBox(height: AppSpacing.xxl),
                ],
              ),
            ),
            ),
          ),
        ],
      ),
    );
  }
}


class _DeviceCard extends StatelessWidget {
  final IconData icon;
  final String title;
  final String? subtitle;
  final VoidCallback? onTap;
  final VoidCallback? onDelete;
  final bool isLoading;
  final bool showDelete;

  const _DeviceCard({
    required this.icon,
    required this.title,
    this.subtitle,
    this.onTap,
    this.onDelete,
    this.isLoading = false,
    this.showDelete = true,
  });

  @override
  Widget build(BuildContext context) {
    final pal = context.palette;
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(AppRadius.card),
        child: AppSectionCard(
          padding: const EdgeInsets.symmetric(horizontal: AppSpacing.lg, vertical: AppSpacing.sm + 2),
          margin: EdgeInsets.zero,
          child: Row(children: [
            Container(
              width: 44, height: 44,
              decoration: BoxDecoration(color: pal.primary.withOpacity(0.15), borderRadius: BorderRadius.circular(22)),
              child: isLoading
                  ? Padding(padding: const EdgeInsets.all(10), child: CircularProgressIndicator(strokeWidth: 2, color: pal.primary))
                  : Icon(icon, color: pal.primary, size: 22),
            ),
            const SizedBox(width: AppSpacing.sm + 2),
            Expanded(child: Column(
              crossAxisAlignment: CrossAxisAlignment.start, mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Text(title, style: AppTypography.labelBase().copyWith(color: pal.onSurface, fontWeight: FontWeight.w600), overflow: TextOverflow.ellipsis),
                if (subtitle != null && subtitle!.isNotEmpty) ...[
                  const SizedBox(height: 2),
                  Text(subtitle!, style: AppTypography.chipBase().copyWith(color: pal.onSurfaceVariant, fontFamily: 'monospace'), overflow: TextOverflow.ellipsis),
                ],
              ],
            )),
            if (showDelete && onDelete != null)
              IconButton(
                icon: Icon(Icons.close, size: 20, color: pal.onSurfaceVariant),
                onPressed: onDelete,
                style: IconButton.styleFrom(padding: const EdgeInsets.all(6), minimumSize: const Size(36, 36)),
              )
            else
              Icon(Icons.chevron_right, color: pal.onSurfaceVariant, size: 22),
          ]),
        ),
      ),
    );
  }
}

class _AboutDialog extends StatefulWidget {
  final AppLocalizations l10n;

  const _AboutDialog({required this.l10n});

  @override
  State<_AboutDialog> createState() => _AboutDialogState();
}

class _AboutDialogState extends State<_AboutDialog> {
  String _version = '1.0.0';
  String _buildNumber = '1';

  @override
  void initState() {
    super.initState();
    PackageInfo.fromPlatform().then((info) {
      if (mounted) setState(() {
        _version = info.version;
        _buildNumber = info.buildNumber;
      });
    }).catchError((_) {});
  }

  @override
  Widget build(BuildContext context) {
    final p = context.palette;
    return RiftDialogFrame(
      maxWidth: 360,
      padding: EdgeInsets.fromLTRB(
        AppSpacing.xxl - 2,
        AppSpacing.xl,
        AppSpacing.lg + 2,
        AppSpacing.buttonPrimaryV,
      ),
      child: SingleChildScrollView(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(
              'RiftLink',
              style: AppTypography.screenTitleBase().copyWith(
                fontSize: 22,
                fontWeight: FontWeight.w800,
                color: p.onSurface,
              ),
              textAlign: TextAlign.center,
            ),
            SizedBox(height: AppSpacing.sm + 2),
            Wrap(
              alignment: WrapAlignment.center,
              spacing: 12,
              runSpacing: 4,
              children: [
                Text.rich(
                  TextSpan(
                    style: AppTypography.labelBase().copyWith(color: p.onSurfaceVariant),
                    children: [
                      TextSpan(text: '${widget.l10n.tr('about_version')}: '),
                      TextSpan(
                        text: _version,
                        style: AppTypography.labelBase().copyWith(fontWeight: FontWeight.w700, color: p.primary),
                      ),
                    ],
                  ),
                ),
                Text.rich(
                  TextSpan(
                    style: AppTypography.labelBase().copyWith(color: p.onSurfaceVariant),
                    children: [
                      TextSpan(text: '${widget.l10n.tr('about_build')}: '),
                      TextSpan(
                        text: _buildNumber,
                        style: AppTypography.labelBase().copyWith(fontWeight: FontWeight.w700, color: p.primary),
                      ),
                    ],
                  ),
                ),
              ],
            ),
            SizedBox(height: AppSpacing.lg),
            Container(
              padding: EdgeInsets.all(AppSpacing.md),
              decoration: BoxDecoration(
                color: p.surfaceVariant.withOpacity(0.45),
                borderRadius: BorderRadius.circular(AppRadius.card),
                border: Border.all(color: p.divider.withOpacity(0.5)),
              ),
              child: Text(
                widget.l10n.tr('about_desc'),
                style: AppTypography.labelBase().copyWith(height: AppTypography.bodyHeight, color: p.onSurface),
                textAlign: TextAlign.center,
              ),
            ),
            SizedBox(height: AppSpacing.md),
            Text(
              widget.l10n.tr('about_legal'),
              style: AppTypography.chipBase().copyWith(
                fontSize: 10,
                fontWeight: FontWeight.w400,
                fontStyle: FontStyle.italic,
                color: p.onSurfaceVariant,
                height: 1.3,
              ),
              textAlign: TextAlign.center,
            ),
            SizedBox(height: AppSpacing.sm + 2),
            Align(
              alignment: Alignment.centerRight,
              child: TextButton(
                style: TextButton.styleFrom(foregroundColor: p.primary),
                onPressed: () => Navigator.pop(context),
                child: Text(widget.l10n.tr('ok')),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
