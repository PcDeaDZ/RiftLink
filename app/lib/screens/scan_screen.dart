import 'dart:async';
import 'package:flutter/material.dart';
import '../theme/app_theme.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../locale_notifier.dart';
import 'chat_screen.dart';
import 'debug_screen.dart';

class ScanScreen extends StatefulWidget {
  const ScanScreen({super.key});
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

class _ScanScreenState extends State<ScanScreen> {
  final RiftLinkBle _ble = RiftLinkBle();
  bool _scanning = false;
  int _titleTapCount = 0;
  bool _showAllDevices = false;
  List<ScanResult> _results = [];
  String? _error;
  StreamSubscription? _scanSub;

  Future<void> _startScan() async {
    setState(() { _scanning = true; _results = []; });
    try {
      if (await Permission.bluetoothScan.isDenied) await Permission.bluetoothScan.request();
      if (await Permission.bluetoothConnect.isDenied) await Permission.bluetoothConnect.request();
      if (await Permission.locationWhenInUse.isDenied) await Permission.locationWhenInUse.request();
      if (!await FlutterBluePlus.isOn) await FlutterBluePlus.turnOn();
      await RiftLinkBle.stopScan();
      _scanSub = FlutterBluePlus.scanResults.listen((results) {
        if (!mounted) return;
        final found = _showAllDevices ? results : results.where(RiftLinkBle.isRiftLink).toList();
        for (final r in found) {
          if (!_results.any((x) => x.device.remoteId == r.device.remoteId)) {
            setState(() { _results = [..._results, r]; if (_error != null) _error = null; });
          }
        }
      });
      await RiftLinkBle.startScan(timeout: const Duration(seconds: 12));
    } catch (e) {
      if (mounted) setState(() => _error = _formatBleError(context, e));
    } finally {
      if (mounted) {
        await Future.delayed(const Duration(seconds: 12));
        await _scanSub?.cancel();
        _scanSub = null;
        await RiftLinkBle.stopScan();
        setState(() => _scanning = false);
      }
    }
  }

  Future<void> _connect(BluetoothDevice device) async {
    setState(() => _error = null);
    try {
      final ok = await _ble.connect(device);
      if (!mounted) return;
      if (ok) {
        Navigator.of(context).pushReplacement(MaterialPageRoute(builder: (_) => ChatScreen(ble: _ble)));
      } else {
        setState(() => _error = context.l10n.tr('ble_no_service'));
      }
    } catch (e) {
      if (mounted) setState(() => _error = _formatBleError(context, e));
    }
  }

  void _showAbout() {
    final l10n = context.l10n;
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text(l10n.tr('about'), style: const TextStyle(color: AppColors.onSurface)),
        content: SingleChildScrollView(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(l10n.tr('about_legal'), style: const TextStyle(fontStyle: FontStyle.italic, color: AppColors.onSurface)),
              const SizedBox(height: 16),
              Text(l10n.tr('about_desc'), style: const TextStyle(fontSize: 13, color: AppColors.onSurface)),
            ],
          ),
        ),
        actions: [TextButton(onPressed: () => Navigator.pop(ctx), child: Text(l10n.tr('ok')))],
      ),
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
          IconButton(onPressed: _showAbout, icon: const Icon(Icons.info_outline), tooltip: l10n.tr('about')),
        ],
      ),
      body: Material(
        color: AppColors.surface,
        child: SafeArea(
          child: Column(
          children: [
            const SizedBox(height: 40),
            Icon(Icons.bluetooth_searching, size: 72, color: AppColors.primary),
            const SizedBox(height: 20),
            Text(l10n.tr('find_device'), style: const TextStyle(fontSize: 22, fontWeight: FontWeight.w600, color: AppColors.onSurface)),
            const SizedBox(height: 8),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 32),
              child: Text(l10n.tr('turn_on_heltec'), textAlign: TextAlign.center, style: const TextStyle(fontSize: 14, color: AppColors.onSurfaceVariant)),
            ),
            const SizedBox(height: 24),
            SizedBox(
              height: _error != null ? 56 : 0,
              child: _error != null
                  ? Container(
                      margin: const EdgeInsets.symmetric(horizontal: 24),
                      padding: const EdgeInsets.all(12),
                      decoration: BoxDecoration(color: AppColors.error.withOpacity(0.2), borderRadius: BorderRadius.circular(8), border: Border.all(color: AppColors.error)),
                      child: Row(
                        children: [
                          const Icon(Icons.error_outline, color: AppColors.error, size: 20),
                          const SizedBox(width: 8),
                          Expanded(child: Text(_error!, style: const TextStyle(color: AppColors.error, fontSize: 13))),
                        ],
                      ),
                    )
                  : const SizedBox.shrink(),
            ),
            Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                IgnorePointer(
                  ignoring: _scanning,
                  child: Opacity(
                    opacity: _scanning ? 0.5 : 1,
                    child: Checkbox(value: _showAllDevices, onChanged: (v) => setState(() => _showAllDevices = v ?? false), activeColor: AppColors.primary),
                  ),
                ),
                GestureDetector(
                  onTap: _scanning ? null : () => setState(() => _showAllDevices = !_showAllDevices),
                  child: Opacity(
                    opacity: _scanning ? 0.5 : 1,
                    child: Text(l10n.tr('show_all_ble'), style: const TextStyle(color: AppColors.onSurface)),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 24),
              child: SizedBox(
                width: double.infinity,
                height: 48,
                child: ElevatedButton.icon(
                  onPressed: _scanning ? null : _startScan,
                  icon: SizedBox(
                    width: 20,
                    height: 20,
                    child: _scanning
                        ? const CircularProgressIndicator(strokeWidth: 2, color: Colors.white)
                        : const Icon(Icons.search, color: Colors.white, size: 20),
                  ),
                  label: Text(_scanning ? l10n.tr('scanning') : l10n.tr('find_device'), style: const TextStyle(color: Colors.white)),
                  style: ElevatedButton.styleFrom(
                    backgroundColor: AppColors.primary,
                    disabledBackgroundColor: AppColors.primary.withOpacity(0.5),
                    padding: const EdgeInsets.symmetric(horizontal: 28, vertical: 14),
                    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
                  ),
                ),
              ),
            ),
            if (_results.isNotEmpty) ...[
              const SizedBox(height: 24),
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 24),
                child: Align(
                  alignment: Alignment.centerLeft,
                  child: Text('${l10n.tr('found')} ${_results.length}', style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: AppColors.onSurface)),
                ),
              ),
              const SizedBox(height: 4),
              const Divider(height: 1, color: AppColors.divider),
              Expanded(
                child: ListView.separated(
                  itemCount: _results.length,
                  separatorBuilder: (_, __) => const Divider(height: 1, indent: 72, color: AppColors.divider),
                  itemBuilder: (_, i) {
                    final r = _results[i];
                    final name = r.device.advName.isNotEmpty ? r.device.advName
                        : r.device.platformName.isNotEmpty ? r.device.platformName
                        : r.device.remoteId.toString();
                    return ListTile(
                      leading: Container(
                        width: 40, height: 40,
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
              ),
            ],
            if (_results.isEmpty) const Spacer(),
          ],
        ),
        ),
      ),
    );
  }
}
