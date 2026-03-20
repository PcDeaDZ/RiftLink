import 'dart:async';

import 'package:flutter/material.dart';
import 'package:package_info_plus/package_info_plus.dart';
import '../theme/app_theme.dart';
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
import '../widgets/rift_dialogs.dart';
import '../widgets/app_popover_menu.dart';
import 'chat_screen.dart';
import 'debug_screen.dart';

class ScanScreen extends StatefulWidget {
  const ScanScreen({super.key, this.initialMessage});
  final String? initialMessage;
  @override
  State<ScanScreen> createState() => _ScanScreenState();
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

  @override
  void initState() {
    super.initState();
    _loadRecent();
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
    _scanSub = null;
    RiftLinkBle.stopScan();  // fire-and-forget, очистка при закрытии
    _meshAnimController?.dispose();
    super.dispose();
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
    showDialog(
      context: context,
      barrierColor: Colors.black54,
      barrierDismissible: true,
      builder: (ctx) => _AboutDialog(l10n: l10n),
    );
  }

  void _showLangPicker() async {
    await AppLocalizations.switchLocale(() { localeNotifier.value = AppLocalizations.currentLocale; });
    if (mounted) {
      final l10n = context.l10n;
      final langName = AppLocalizations.currentLocale.languageCode == 'ru' ? l10n.tr('lang_ru') : l10n.tr('lang_en');
      showAppSnackBar(context, '${l10n.tr('lang')}: $langName');
    }
  }

  void _showThemePicker() => showThemeModeSheet(context);

  Future<void> _showScanMenu(AppLocalizations l10n) async {
    FocusScope.of(context).unfocus();
    final v = await Navigator.push<String>(
      context,
      AppPopoverMenuRoute<String>(
        child: _ScanMenuPopover(l10n: l10n),
      ),
    );
    if (!mounted || v == null) return;
    switch (v) {
      case 'theme':
        _showThemePicker();
        break;
      case 'lang':
        _showLangPicker();
        break;
      case 'about':
        _showAbout();
        break;
    }
  }

  @override
  Widget build(BuildContext context) {
    final l10n = context.l10n;
    return Scaffold(
      backgroundColor: context.palette.surface,
      appBar: AppBar(
        title: GestureDetector(
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
            icon: const Icon(Icons.more_vert),
            tooltip: l10n.tr('menu_tools'),
            onPressed: () => _showScanMenu(l10n),
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
                  const SizedBox(height: 20),
                  Icon(Icons.bluetooth_searching, size: 56, color: context.palette.primary),
                  const SizedBox(height: 12),
                  Text(l10n.tr('find_device'), style: TextStyle(fontSize: 20, fontWeight: FontWeight.w600, color: context.palette.onSurface), textAlign: TextAlign.center),
                  const SizedBox(height: 12),
                  if (_error != null)
                    Container(
                      margin: const EdgeInsets.symmetric(horizontal: 24),
                      padding: const EdgeInsets.all(12),
                      decoration: BoxDecoration(color: context.palette.error.withOpacity(0.2), borderRadius: BorderRadius.circular(8), border: Border.all(color: context.palette.error)),
                      child: Column(
                        mainAxisSize: MainAxisSize.min,
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Row(
                            children: [
                              Icon(Icons.error_outline, color: context.palette.error, size: 20),
                              const SizedBox(width: 8),
                              Expanded(child: Text(_error!, style: TextStyle(color: context.palette.error, fontSize: 13))),
                            ],
                          ),
                          if (_error!.toLowerCase().contains('permission') || _error!.toLowerCase().contains('denied')) ...[
                            const SizedBox(height: 8),
                            TextButton.icon(
                              onPressed: () => openAppSettings(),
                              icon: Icon(Icons.settings, size: 18, color: context.palette.primary),
                              label: Text(l10n.tr('open_settings'), style: TextStyle(color: context.palette.primary)),
                            ),
                          ],
                        ],
                      ),
                    ),
                  if (_error != null) const SizedBox(height: 12),
                  const SizedBox(height: 12),
                  Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 24),
                    child: SizedBox(
                      width: double.infinity,
                      height: 48,
                      child: ElevatedButton.icon(
                        onPressed: _connectingToRemoteId != null ? null : () {
                          if (_scanning) {
                            _scanStoppedByUser = true;
                            _scanCompleter?.complete();
                            return;
                          }
                          _startScan();
                        },
                        icon: SizedBox(
                          width: 20,
                          height: 20,
                          child: (_scanning || _connectingToRemoteId != null) && _connectingToRemoteId == null
                              ? const CircularProgressIndicator(strokeWidth: 2, color: Colors.white)
                              : _connectingToRemoteId != null
                                  ? const Icon(Icons.bluetooth_searching, color: Colors.white, size: 20)
                                  : const Icon(Icons.search, color: Colors.white, size: 20),
                        ),
                        label: Builder(
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
                            return SizedBox(width: double.infinity, child: Center(child: Text(labelText, style: TextStyle(color: Colors.white, fontSize: 15), textAlign: TextAlign.center)));
                          },
                        ),
                        style: ElevatedButton.styleFrom(
                          backgroundColor: context.palette.primary,
                          disabledBackgroundColor: context.palette.primary.withOpacity(0.5),
                          padding: const EdgeInsets.symmetric(horizontal: 28, vertical: 14),
                          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
                        ),
                      ),
                    ),
                  ),
                  if (_recent.isNotEmpty) ...[
                    const SizedBox(height: 24),
                    Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 24),
                      child: Center(child: Text(l10n.tr('recent_devices'), style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: context.palette.onSurface))),
                    ),
                    const SizedBox(height: 8),
                    Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 16),
                      child: Column(
                        children: _recent.asMap().entries.map((e) {
                          final i = e.key;
                          final d = e.value;
                          final isConnecting = _connectingToRemoteId == d.remoteId;
                          return Padding(
                            padding: EdgeInsets.only(bottom: i < _recent.length - 1 ? 12 : 0),
                            child: Material(
                              color: Colors.transparent,
                              child: InkWell(
                                onTap: _scanning ? null : () {
                                  final label = d.displayName.isNotEmpty ? d.displayName : (d.nodeId.isNotEmpty ? d.nodeId : d.remoteId);
                                  setState(() {
                                    _connectingToRemoteId = d.remoteId;
                                    _connectingToDeviceName = label;
                                    _scanning = true;
                                    _scanStoppedByUser = false;
                                    _results = [];
                                    _error = null;
                                  });
                                  WidgetsBinding.instance.addPostFrameCallback((_) {
                                    if (mounted && _connectingToRemoteId == d.remoteId) {
                                      _connectToRecent(d, displayLabel: label);
                                    }
                                  });
                                },
                                borderRadius: BorderRadius.circular(14),
                                child: Container(
                                  width: double.infinity,
                                  padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
                                decoration: BoxDecoration(
                                  color: context.palette.surfaceVariant.withOpacity(0.7),
                                  borderRadius: BorderRadius.circular(14),
                                  border: Border.all(color: context.palette.divider, width: 1),
                                ),
                                child: Row(
                                  children: [
                                    Container(
                                      width: 52,
                                      height: 52,
                                      decoration: BoxDecoration(color: context.palette.primary.withOpacity(0.2), borderRadius: BorderRadius.circular(26)),
                                      child: isConnecting
                                          ? Padding(padding: const EdgeInsets.all(14), child: CircularProgressIndicator(strokeWidth: 2, color: context.palette.primary))
                                          : Icon(Icons.bluetooth, color: context.palette.primary, size: 28),
                                    ),
                                    const SizedBox(width: 14),
                                    Expanded(
                                      child: Column(
                                        mainAxisAlignment: MainAxisAlignment.center,
                                        crossAxisAlignment: CrossAxisAlignment.start,
                                        children: d.displayName != d.nodeId
                                            ? [
                                                Text(d.displayName, style: TextStyle(fontWeight: FontWeight.w600, color: context.palette.onSurface, fontSize: 16), overflow: TextOverflow.ellipsis),
                                                const SizedBox(height: 2),
                                                Text(d.nodeId, style: TextStyle(fontFamily: 'monospace', fontSize: 13, color: context.palette.onSurfaceVariant), overflow: TextOverflow.ellipsis),
                                              ]
                                            : [
                                                Text(d.nodeId, style: TextStyle(fontWeight: FontWeight.w600, color: context.palette.onSurface, fontSize: 16, fontFamily: 'monospace'), overflow: TextOverflow.ellipsis),
                                              ],
                                      ),
                                    ),
                                    if (!_scanning)
                                      IconButton(
                                        icon: Icon(Icons.close, size: 22, color: context.palette.onSurfaceVariant),
                                        onPressed: () => _confirmForgetDevice(d),
                                        tooltip: l10n.tr('forget_device'),
                                        style: IconButton.styleFrom(padding: const EdgeInsets.all(6), minimumSize: const Size(40, 40)),
                                      ),
                                  ],
                                ),
                              ),
                            ),
                          ),
                          );
                        }).toList(),
                      ),
                    ),
                  ],
                  if (_recent.isEmpty && !_scanning) ...[
                    const SizedBox(height: 12),
                    Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 24),
                      child: Text(l10n.tr('recent_empty'), textAlign: TextAlign.center, style: TextStyle(fontSize: 13, color: context.palette.onSurfaceVariant)),
                    ),
                  ],
                  Builder(builder: (_) {
                    final filtered = _results.where((r) => !_recent.any((d) => d.remoteId == r.device.remoteId.toString())).toList();
                    if (filtered.isEmpty) return const SizedBox.shrink();
                    return Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        const SizedBox(height: 24),
                        Padding(
                          padding: const EdgeInsets.symmetric(horizontal: 24),
                          child: Center(child: Text('${l10n.tr('found')} ${filtered.length}', style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: context.palette.onSurface))),
                        ),
                        const SizedBox(height: 4),
                        Divider(height: 1, color: context.palette.divider),
                        ListView.separated(
                          shrinkWrap: true,
                          physics: const NeverScrollableScrollPhysics(),
                          itemCount: filtered.length,
                          separatorBuilder: (_, __) => Divider(height: 1, indent: 72, color: context.palette.divider),
                          itemBuilder: (_, i) {
                            final r = filtered[i];
                            final name = r.device.advName.isNotEmpty ? r.device.advName
                                : r.device.platformName.isNotEmpty ? r.device.platformName
                                : r.device.remoteId.toString();
                            return ListTile(
                              leading: Container(
                                width: 40,
                                height: 40,
                                decoration: BoxDecoration(color: context.palette.primary.withOpacity(0.2), borderRadius: BorderRadius.circular(20)),
                                child: Icon(Icons.bluetooth, color: context.palette.primary, size: 22),
                              ),
                              title: Text(name, style: TextStyle(fontWeight: FontWeight.w500, color: context.palette.onSurface)),
                              subtitle: Text(r.device.remoteId.toString(), style: TextStyle(fontFamily: 'monospace', fontSize: 12, color: context.palette.onSurfaceVariant)),
                              trailing: Icon(Icons.chevron_right, color: context.palette.onSurfaceVariant),
                              onTap: () => _connect(r.device, displayName: name),
                            );
                          },
                        ),
                      ],
                    );
                  }),
                  if (_results.isEmpty) const SizedBox(height: 48),
                  const SizedBox(height: 24),
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

/// Меню «три точки» в том же стиле, что и в [ChatScreen].
class _ScanMenuPopover extends StatelessWidget {
  final AppLocalizations l10n;

  const _ScanMenuPopover({required this.l10n});

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.transparent,
      child: Container(
        constraints: const BoxConstraints(minWidth: 200, maxWidth: 280),
        decoration: BoxDecoration(
          color: context.palette.card,
          borderRadius: BorderRadius.circular(12),
          border: Border.all(color: context.palette.divider, width: 0.5),
          boxShadow: [
            BoxShadow(color: Colors.black.withOpacity(0.3), blurRadius: 16, offset: const Offset(0, 4)),
          ],
        ),
        child: ClipRRect(
          borderRadius: BorderRadius.circular(12),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              _item(context, Icons.dark_mode_outlined, l10n.tr('theme'), 'theme'),
              _item(context, Icons.language, l10n.tr('lang'), 'lang'),
              _item(context, Icons.info_outline_rounded, l10n.tr('about'), 'about'),
            ],
          ),
        ),
      ),
    );
  }

  Widget _item(BuildContext context, IconData icon, String label, String id) {
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: () => Navigator.pop(context, id),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
          child: Row(
            children: [
              Icon(icon, size: 22, color: context.palette.onSurface),
              const SizedBox(width: 14),
              Text(label, style: TextStyle(fontSize: 15, color: context.palette.onSurface)),
            ],
          ),
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
      padding: const EdgeInsets.fromLTRB(22, 20, 18, 14),
      child: SingleChildScrollView(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(
              'RiftLink',
              style: TextStyle(fontSize: 22, fontWeight: FontWeight.w800, color: p.onSurface),
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 10),
            Wrap(
              alignment: WrapAlignment.center,
              spacing: 12,
              runSpacing: 4,
              children: [
                Text.rich(
                  TextSpan(
                    style: TextStyle(fontSize: 13, color: p.onSurfaceVariant),
                    children: [
                      TextSpan(text: '${widget.l10n.tr('about_version')}: '),
                      TextSpan(text: _version, style: TextStyle(fontWeight: FontWeight.w700, color: p.primary)),
                    ],
                  ),
                ),
                Text.rich(
                  TextSpan(
                    style: TextStyle(fontSize: 13, color: p.onSurfaceVariant),
                    children: [
                      TextSpan(text: '${widget.l10n.tr('about_build')}: '),
                      TextSpan(text: _buildNumber, style: TextStyle(fontWeight: FontWeight.w700, color: p.primary)),
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: 16),
            Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: p.surfaceVariant.withOpacity(0.45),
                borderRadius: BorderRadius.circular(12),
                border: Border.all(color: p.divider.withOpacity(0.5)),
              ),
              child: Text(
                widget.l10n.tr('about_desc'),
                style: TextStyle(fontSize: 13, height: 1.35, color: p.onSurface),
                textAlign: TextAlign.center,
              ),
            ),
            const SizedBox(height: 12),
            Text(
              widget.l10n.tr('about_legal'),
              style: TextStyle(fontSize: 10, fontStyle: FontStyle.italic, color: p.onSurfaceVariant, height: 1.3),
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 14),
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
