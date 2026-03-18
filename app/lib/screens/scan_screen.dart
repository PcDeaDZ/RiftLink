import 'dart:async';
import 'package:flutter/material.dart';
import 'package:package_info_plus/package_info_plus.dart';
import '../theme/app_theme.dart';
import '../prefs/mesh_prefs.dart';
import '../widgets/mesh_background.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import '../ble/riftlink_ble.dart';
import '../app_navigator.dart';
import '../l10n/app_localizations.dart';
import '../locale_notifier.dart';
import '../recent_devices/recent_devices_service.dart';
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
  final RiftLinkBle _ble = RiftLinkBle();
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

  @override
  void initState() {
    super.initState();
    _loadRecent();
    _loadMeshPrefs();
    if (widget.initialMessage != null) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(content: Text(widget.initialMessage!), backgroundColor: AppColors.error),
          );
        }
      });
    }
  }

  @override
  void dispose() {
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
    setState(() { _scanning = true; _results = []; _connectingToRemoteId = connectToRemoteId; _connectingToDeviceName = connectToDeviceName; _error = null; });
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
          if (connectToRemoteId != null && r.device.remoteId.toString() == connectToRemoteId) {
            _scanSub?.cancel();
            _scanSub = null;
            RiftLinkBle.stopScan();
            _connect(r.device);
            return;
          }
          if (connectToRemoteId == null && !_results.any((x) => x.device.remoteId == r.device.remoteId)) {
            setState(() { _results = [..._results, r]; if (_error != null) _error = null; });
          }
        }
      });
      const scanDuration = Duration(seconds: 12);
      await RiftLinkBle.startScan(timeout: scanDuration);
      await Future<void>.delayed(scanDuration);
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
          if (_error == null && _results.isEmpty && connectToRemoteId == null) {
            _error = context.l10n.tr('scan_no_devices');
          }
        });
      }
    }
  }

  Future<void> _connect(BluetoothDevice device) async {
    final name = device.advName.isNotEmpty ? device.advName
        : device.platformName.isNotEmpty ? device.platformName
        : device.remoteId.toString().length > 12 ? device.remoteId.toString().substring(device.remoteId.toString().length - 12) : device.remoteId.toString();
    final displayName = name.isNotEmpty ? name : context.l10n.tr('device');
    setState(() {
      _error = null;
      _connectingToRemoteId = device.remoteId.toString();
      _connectingToDeviceName = displayName;
    });
    try {
      final ok = await _ble.connect(device);
      if (!mounted) return;
      if (ok) {
        Navigator.of(context).pushReplacement(MaterialPageRoute(builder: (_) => ChatScreen(ble: _ble)));
      } else {
        setState(() { _error = context.l10n.tr('ble_no_service'); _connectingToRemoteId = null; _connectingToDeviceName = null; });
      }
    } catch (e) {
      if (mounted) setState(() { _error = _formatBleError(context, e); _connectingToRemoteId = null; _connectingToDeviceName = null; });
    }
  }

  void _connectToRecent(RecentDevice dev) {
    final name = dev.displayName.isNotEmpty ? dev.displayName : dev.nodeId.isNotEmpty ? dev.nodeId : dev.remoteId;
    _startScan(connectToRemoteId: dev.remoteId, connectToDeviceName: name);
  }

  Future<void> _confirmForgetDevice(RecentDevice d) async {
    final l10n = context.l10n;
    final confirm = await showAppDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text(l10n.tr('forget_device'), style: const TextStyle(color: AppColors.onSurface)),
        content: Text(l10n.tr('forget_device_confirm', {'name': d.displayName}), style: const TextStyle(color: AppColors.onSurface)),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: Text(l10n.tr('cancel'))),
          TextButton(onPressed: () => Navigator.pop(ctx, true), child: Text(l10n.tr('delete'), style: const TextStyle(color: AppColors.error))),
        ],
      ),
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
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('${l10n.tr('lang')}: $langName')));
    }
  }

  @override
  Widget build(BuildContext context) {
    final l10n = context.l10n;
    return Scaffold(
      backgroundColor: AppColors.surface,
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
          IconButton(onPressed: _showLangPicker, icon: const Icon(Icons.language), tooltip: l10n.tr('lang')),
          IconButton(
            onPressed: () => showAppDialog(
              context: context,
              builder: (ctx) => _AboutDialog(l10n: AppLocalizations(AppLocalizations.currentLocale)),
            ),
            icon: const Icon(Icons.info_outline),
            tooltip: l10n.tr('about'),
            splashRadius: 24,
          ),
        ],
      ),
      body: Stack(
        fit: StackFit.expand,
        children: [
          Positioned.fill(
            child: IgnorePointer(
              child: ColoredBox(
                color: AppColors.surface,
                child: ListenableBuilder(
                  listenable: _meshAnimationEnabled && _meshAnimController != null ? _meshAnimController! : const AlwaysStoppedAnimation(0),
                  builder: (_, __) => CustomPaint(
                    painter: MeshBackgroundPainter(progress: _meshAnimController?.value ?? 0, animated: _meshAnimationEnabled),
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
                  children: [
                  const SizedBox(height: 20),
                  Icon(Icons.bluetooth_searching, size: 56, color: AppColors.primary),
                  const SizedBox(height: 12),
                  Text(l10n.tr('find_device'), style: const TextStyle(fontSize: 20, fontWeight: FontWeight.w600, color: AppColors.onSurface)),
                  const SizedBox(height: 12),
                  if (_error != null)
                    Container(
                      margin: const EdgeInsets.symmetric(horizontal: 24),
                      padding: const EdgeInsets.all(12),
                      decoration: BoxDecoration(color: AppColors.error.withOpacity(0.2), borderRadius: BorderRadius.circular(8), border: Border.all(color: AppColors.error)),
                      child: Column(
                        mainAxisSize: MainAxisSize.min,
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Row(
                            children: [
                              const Icon(Icons.error_outline, color: AppColors.error, size: 20),
                              const SizedBox(width: 8),
                              Expanded(child: Text(_error!, style: const TextStyle(color: AppColors.error, fontSize: 13))),
                            ],
                          ),
                          if (_error!.toLowerCase().contains('permission') || _error!.toLowerCase().contains('denied')) ...[
                            const SizedBox(height: 8),
                            TextButton.icon(
                              onPressed: () => openAppSettings(),
                              icon: const Icon(Icons.settings, size: 18, color: AppColors.primary),
                              label: Text(l10n.tr('open_settings'), style: const TextStyle(color: AppColors.primary)),
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
                        onPressed: (_scanning || _connectingToRemoteId != null) ? null : () => _startScan(),
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
                              if (name.isEmpty && _connectingToRemoteId != null) {
                                final rid = _connectingToRemoteId!;
                                name = rid.length > 12 ? rid.substring(rid.length - 12) : rid;
                              }
                              if (name.isEmpty) name = l10n.tr('device');
                              labelText = l10n.tr('connecting_to', {'name': name});
                            } else if (_scanning) {
                              labelText = l10n.tr('scanning');
                            }
                            return Text(labelText, style: const TextStyle(color: Colors.white, fontSize: 15));
                          },
                        ),
                        style: ElevatedButton.styleFrom(
                          backgroundColor: AppColors.primary,
                          disabledBackgroundColor: AppColors.primary.withOpacity(0.5),
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
                      child: Align(
                        alignment: Alignment.centerLeft,
                        child: Text(l10n.tr('recent_devices'), style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: AppColors.onSurface)),
                      ),
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
                                onTap: _scanning ? null : () => _connectToRecent(d),
                                borderRadius: BorderRadius.circular(14),
                                child: Container(
                                  width: double.infinity,
                                  padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
                                decoration: BoxDecoration(
                                  color: AppColors.surfaceVariant.withOpacity(0.7),
                                  borderRadius: BorderRadius.circular(14),
                                  border: Border.all(color: AppColors.divider, width: 1),
                                ),
                                child: Row(
                                  children: [
                                    Container(
                                      width: 52,
                                      height: 52,
                                      decoration: BoxDecoration(color: AppColors.primary.withOpacity(0.2), borderRadius: BorderRadius.circular(26)),
                                      child: isConnecting
                                          ? const Padding(padding: EdgeInsets.all(14), child: CircularProgressIndicator(strokeWidth: 2, color: AppColors.primary))
                                          : const Icon(Icons.bluetooth, color: AppColors.primary, size: 28),
                                    ),
                                    const SizedBox(width: 14),
                                    Expanded(
                                      child: Column(
                                        mainAxisAlignment: MainAxisAlignment.center,
                                        crossAxisAlignment: CrossAxisAlignment.start,
                                        children: d.displayName != d.nodeId
                                            ? [
                                                Text(d.displayName, style: const TextStyle(fontWeight: FontWeight.w600, color: AppColors.onSurface, fontSize: 16), overflow: TextOverflow.ellipsis),
                                                const SizedBox(height: 2),
                                                Text(d.nodeId, style: const TextStyle(fontFamily: 'monospace', fontSize: 13, color: AppColors.onSurfaceVariant), overflow: TextOverflow.ellipsis),
                                              ]
                                            : [
                                                Text(d.nodeId, style: const TextStyle(fontWeight: FontWeight.w600, color: AppColors.onSurface, fontSize: 16, fontFamily: 'monospace'), overflow: TextOverflow.ellipsis),
                                              ],
                                      ),
                                    ),
                                    if (!_scanning)
                                      IconButton(
                                        icon: const Icon(Icons.close, size: 22, color: AppColors.onSurfaceVariant),
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
                      child: Text(l10n.tr('recent_empty'), textAlign: TextAlign.center, style: const TextStyle(fontSize: 13, color: AppColors.onSurfaceVariant)),
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
                          child: Align(
                            alignment: Alignment.centerLeft,
                            child: Text('${l10n.tr('found')} ${filtered.length}', style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: AppColors.onSurface)),
                          ),
                        ),
                        const SizedBox(height: 4),
                        const Divider(height: 1, color: AppColors.divider),
                        ListView.separated(
                          shrinkWrap: true,
                          physics: const NeverScrollableScrollPhysics(),
                          itemCount: filtered.length,
                          separatorBuilder: (_, __) => const Divider(height: 1, indent: 72, color: AppColors.divider),
                          itemBuilder: (_, i) {
                            final r = filtered[i];
                            final name = r.device.advName.isNotEmpty ? r.device.advName
                                : r.device.platformName.isNotEmpty ? r.device.platformName
                                : r.device.remoteId.toString();
                            return ListTile(
                              leading: Container(
                                width: 40,
                                height: 40,
                                decoration: BoxDecoration(color: AppColors.primary.withOpacity(0.2), borderRadius: BorderRadius.circular(20)),
                                child: const Icon(Icons.bluetooth, color: AppColors.primary, size: 22),
                              ),
                              title: Text(name, style: const TextStyle(fontWeight: FontWeight.w500, color: AppColors.onSurface)),
                              subtitle: Text(r.device.remoteId.toString(), style: const TextStyle(fontFamily: 'monospace', fontSize: 12, color: AppColors.onSurfaceVariant)),
                              trailing: const Icon(Icons.chevron_right, color: AppColors.onSurfaceVariant),
                              onTap: () => _connect(r.device),
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
    return Dialog(
      backgroundColor: AppColors.card,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(20)),
      insetPadding: const EdgeInsets.symmetric(horizontal: 24),
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 400),
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: SingleChildScrollView(
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
              Text('RiftLink', style: const TextStyle(fontSize: 24, fontWeight: FontWeight.bold, color: AppColors.onSurface)),
              const SizedBox(height: 8),
              Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Text('${widget.l10n.tr('about_version')}: ', style: const TextStyle(fontSize: 14, color: AppColors.onSurfaceVariant)),
                  Text(_version, style: const TextStyle(fontSize: 14, fontWeight: FontWeight.w600, color: AppColors.primary)),
                  const SizedBox(width: 16),
                  Text('${widget.l10n.tr('about_build')}: ', style: const TextStyle(fontSize: 14, color: AppColors.onSurfaceVariant)),
                  Text(_buildNumber, style: const TextStyle(fontSize: 14, fontWeight: FontWeight.w600, color: AppColors.primary)),
                ],
              ),
              const SizedBox(height: 20),
              Container(
                padding: const EdgeInsets.all(16),
                decoration: BoxDecoration(
                  color: AppColors.surfaceVariant.withOpacity(0.5),
                  borderRadius: BorderRadius.circular(12),
                  border: Border.all(color: AppColors.divider, width: 0.5),
                ),
                child: Text(
                  widget.l10n.tr('about_desc'),
                  style: const TextStyle(fontSize: 14, height: 1.4, color: AppColors.onSurface),
                  textAlign: TextAlign.center,
                ),
              ),
              const SizedBox(height: 16),
              Text(
                widget.l10n.tr('about_legal'),
                style: const TextStyle(fontSize: 11, fontStyle: FontStyle.italic, color: AppColors.onSurfaceVariant),
                textAlign: TextAlign.center,
              ),
              const SizedBox(height: 24),
              SizedBox(
                width: double.infinity,
                child: ElevatedButton(
                  onPressed: () => Navigator.pop(context),
                  style: ElevatedButton.styleFrom(
                    backgroundColor: AppColors.primary,
                    foregroundColor: Colors.white,
                    padding: const EdgeInsets.symmetric(vertical: 14),
                    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
                  ),
                  child: Text(widget.l10n.tr('ok')),
                ),
              ),
            ],
          ),
        ),
      ),
      ),
    );
  }
}
