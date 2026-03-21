import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../theme/theme_notifier.dart';
import '../widgets/app_primitives.dart';
import '../widgets/app_snackbar.dart';
import 'ota_screen.dart';
import 'settings_screen.dart';

String _nodeIdForClipboard(String raw) =>
    raw.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();

String _formatNodeIdDisplay(String raw) {
  final c = _nodeIdForClipboard(raw);
  if (c.isEmpty) return raw.trim();
  final parts = <String>[];
  for (var i = 0; i < c.length; i += 4) {
    final end = (i + 4 < c.length) ? i + 4 : c.length;
    parts.add(c.substring(i, end));
  }
  return parts.join(' ');
}

class SettingsHubScreen extends StatefulWidget {
  final RiftLinkBle ble;
  final String nodeId;
  final String? nickname;
  final String region;
  final int? channel;
  final int? sf;
  final bool gpsPresent, gpsEnabled, gpsFix, powersave;
  final int? offlinePending, batteryMv;
  final void Function(String) onNicknameChanged;
  final void Function(String, int?) onRegionChanged;
  final void Function(int?) onSfChanged;
  final void Function(bool) onPowersaveChanged;
  final void Function(bool) onGpsChanged;
  final bool meshAnimationEnabled;
  final void Function(bool) onMeshAnimationChanged;

  const SettingsHubScreen({
    super.key,
    required this.ble,
    required this.nodeId,
    this.nickname,
    required this.region,
    this.channel,
    this.sf,
    this.gpsPresent = false,
    this.gpsEnabled = false,
    this.gpsFix = false,
    this.powersave = false,
    this.offlinePending,
    this.batteryMv,
    required this.onNicknameChanged,
    required this.onRegionChanged,
    required this.onSfChanged,
    required this.onPowersaveChanged,
    required this.onGpsChanged,
    this.meshAnimationEnabled = true,
    required this.onMeshAnimationChanged,
  });

  @override
  State<SettingsHubScreen> createState() => _SettingsHubScreenState();
}

class _SettingsHubScreenState extends State<SettingsHubScreen> {
  late String _nickname;
  late String _region;
  late int? _channel;
  late bool _meshAnimationEnabled;

  @override
  void initState() {
    super.initState();
    _nickname = widget.nickname ?? '';
    _region = widget.region;
    _channel = widget.channel;
    _meshAnimationEnabled = widget.meshAnimationEnabled;
  }

  void _snack(String text) => showAppSnackBar(context, text);

  Future<void> _openDeviceProfile() async {
    final updated = await Navigator.push<String>(
      context,
      MaterialPageRoute(
        builder: (_) => _DeviceProfilePage(
          ble: widget.ble,
          nodeId: widget.nodeId,
          nickname: _nickname,
        ),
      ),
    );
    if (updated != null) {
      widget.onNicknameChanged(updated);
      if (mounted) setState(() => _nickname = updated);
    }
  }

  Future<void> _openMeshRegion() async {
    final updated = await Navigator.push<({String region, int? channel})>(
      context,
      MaterialPageRoute(
        builder: (_) => _MeshRegionPage(
          ble: widget.ble,
          region: _region,
          channel: _channel,
        ),
      ),
    );
    if (updated != null) {
      widget.onRegionChanged(updated.region, updated.channel);
      if (mounted) {
        setState(() {
          _region = updated.region;
          _channel = updated.channel;
        });
      }
    }
  }

  Future<void> _openDiagnostics() async {
    await Navigator.push(
      context,
      MaterialPageRoute(
        builder: (_) => _DiagnosticsPage(
          version: widget.ble.lastInfo?.version,
          batteryMv: widget.ble.lastInfo?.batteryMv ?? widget.batteryMv,
          batteryPercent: widget.ble.lastInfo?.batteryPercent,
          offlinePending: widget.ble.lastInfo?.offlinePending ?? widget.offlinePending,
          gpsPresent: widget.ble.lastInfo?.gpsPresent ?? widget.gpsPresent,
          gpsEnabled: widget.ble.lastInfo?.gpsEnabled ?? widget.gpsEnabled,
          gpsFix: widget.ble.lastInfo?.gpsFix ?? widget.gpsFix,
          powersave: widget.ble.lastInfo?.powersave ?? widget.powersave,
        ),
      ),
    );
  }

  Future<void> _openConnection() async {
    await Navigator.push(
      context,
      MaterialPageRoute(builder: (_) => _ConnectionPage(ble: widget.ble)),
    );
  }

  Future<void> _openSecurity() async {
    await Navigator.push(
      context,
      MaterialPageRoute(builder: (_) => _SecurityPage(ble: widget.ble)),
    );
  }

  Future<void> _openLegacySettings() async {
    await Navigator.push(
      context,
      MaterialPageRoute(
        builder: (_) => SettingsScreen(
          ble: widget.ble,
          nodeId: widget.nodeId,
          nickname: _nickname,
          region: _region,
          channel: _channel,
          sf: widget.sf,
          gpsPresent: widget.gpsPresent,
          gpsEnabled: widget.gpsEnabled,
          gpsFix: widget.gpsFix,
          powersave: widget.powersave,
          offlinePending: widget.offlinePending,
          batteryMv: widget.batteryMv,
          onNicknameChanged: widget.onNicknameChanged,
          onRegionChanged: widget.onRegionChanged,
          onSfChanged: widget.onSfChanged,
          onPowersaveChanged: widget.onPowersaveChanged,
          onGpsChanged: widget.onGpsChanged,
          meshAnimationEnabled: _meshAnimationEnabled,
          onMeshAnimationChanged: widget.onMeshAnimationChanged,
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    return Scaffold(
      backgroundColor: context.palette.surface,
      appBar: AppBar(
        title: Text(l.tr('settings')),
        leading: IconButton(
          icon: const Icon(Icons.arrow_back_rounded),
          onPressed: () => Navigator.pop(context),
        ),
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
        children: [
          AppSectionCard(
            child: Column(
              children: [
                ListTile(
                  contentPadding: EdgeInsets.zero,
                  leading: Icon(Icons.devices_outlined, color: context.palette.primary),
                  title: Text('Device profile', style: TextStyle(color: context.palette.onSurface)),
                  subtitle: Text(
                    _nickname.trim().isEmpty ? _formatNodeIdDisplay(widget.nodeId) : _nickname.trim(),
                    style: TextStyle(color: context.palette.onSurfaceVariant),
                  ),
                  trailing: Icon(Icons.chevron_right_rounded, color: context.palette.onSurfaceVariant),
                  onTap: _openDeviceProfile,
                ),
                Divider(height: 1, color: context.palette.divider),
                ListTile(
                  contentPadding: EdgeInsets.zero,
                  leading: Icon(Icons.route_outlined, color: context.palette.primary),
                  title: Text('Mesh network', style: TextStyle(color: context.palette.onSurface)),
                  subtitle: Text(
                    _channel != null ? 'Region: $_region, channel: $_channel' : 'Region: $_region',
                    style: TextStyle(color: context.palette.onSurfaceVariant),
                  ),
                  trailing: Icon(Icons.chevron_right_rounded, color: context.palette.onSurfaceVariant),
                  onTap: _openMeshRegion,
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),
          AppSectionCard(
            child: Column(
              children: [
                ListTile(
                  contentPadding: EdgeInsets.zero,
                  leading: Icon(Icons.dark_mode_outlined, color: context.palette.primary),
                  title: Text(l.tr('theme'), style: TextStyle(color: context.palette.onSurface)),
                  subtitle: Text(l.tr('theme_hint'), style: TextStyle(color: context.palette.onSurfaceVariant)),
                  trailing: Icon(Icons.chevron_right_rounded, color: context.palette.onSurfaceVariant),
                  onTap: () => showThemeModeSheet(context),
                ),
                Divider(height: 1, color: context.palette.divider),
                SwitchListTile(
                  contentPadding: EdgeInsets.zero,
                  secondary: Icon(Icons.animation_rounded, color: context.palette.primary),
                  title: Text(l.tr('mesh_animation'), style: TextStyle(color: context.palette.onSurface)),
                  subtitle: Text(l.tr('mesh_animation_hint'), style: TextStyle(color: context.palette.onSurfaceVariant)),
                  value: _meshAnimationEnabled,
                  onChanged: (v) {
                    widget.onMeshAnimationChanged(v);
                    setState(() => _meshAnimationEnabled = v);
                    _snack(l.tr('saved'));
                  },
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),
          AppSectionCard(
            child: Column(
              children: [
                ListTile(
                  contentPadding: EdgeInsets.zero,
                  leading: Icon(Icons.bluetooth_searching_rounded, color: context.palette.primary),
                  title: Text('Connection', style: TextStyle(color: context.palette.onSurface)),
                  subtitle: Text(
                    widget.ble.isWifiMode ? 'Radio mode: Wi-Fi' : 'Radio mode: BLE',
                    style: TextStyle(color: context.palette.onSurfaceVariant),
                  ),
                  trailing: Icon(Icons.chevron_right_rounded, color: context.palette.onSurfaceVariant),
                  onTap: _openConnection,
                ),
                Divider(height: 1, color: context.palette.divider),
                ListTile(
                  contentPadding: EdgeInsets.zero,
                  leading: Icon(Icons.lock_outline_rounded, color: context.palette.primary),
                  title: Text('Security', style: TextStyle(color: context.palette.onSurface)),
                  subtitle: Text('E2E invite, accept invite and pairing controls', style: TextStyle(color: context.palette.onSurfaceVariant)),
                  trailing: Icon(Icons.chevron_right_rounded, color: context.palette.onSurfaceVariant),
                  onTap: _openSecurity,
                ),
                Divider(height: 1, color: context.palette.divider),
                ListTile(
                  contentPadding: EdgeInsets.zero,
                  leading: Icon(Icons.system_update_alt_rounded, color: context.palette.primary),
                  title: Text(l.tr('firmware_update_title'), style: TextStyle(color: context.palette.onSurface)),
                  subtitle: Text('Open OTA update screen', style: TextStyle(color: context.palette.onSurfaceVariant)),
                  trailing: Icon(Icons.chevron_right_rounded, color: context.palette.onSurfaceVariant),
                  onTap: () => Navigator.push(
                    context,
                    MaterialPageRoute(builder: (_) => OtaScreen(ble: widget.ble)),
                  ),
                ),
                Divider(height: 1, color: context.palette.divider),
                ListTile(
                  contentPadding: EdgeInsets.zero,
                  leading: Icon(Icons.analytics_outlined, color: context.palette.primary),
                  title: Text('Diagnostics', style: TextStyle(color: context.palette.onSurface)),
                  subtitle: Text('Battery, queues, GPS and version', style: TextStyle(color: context.palette.onSurfaceVariant)),
                  trailing: Icon(Icons.chevron_right_rounded, color: context.palette.onSurfaceVariant),
                  onTap: _openDiagnostics,
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),
          AppSectionCard(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('Advanced settings', style: TextStyle(color: context.palette.onSurface, fontWeight: FontWeight.w600)),
                const SizedBox(height: 6),
                Text(
                  'All detailed controls (modem, invite, radio mode, ESP-NOW, low-level options).',
                  style: TextStyle(color: context.palette.onSurfaceVariant),
                ),
                const SizedBox(height: 12),
                AppSecondaryButton(
                  fullWidth: true,
                  onPressed: _openLegacySettings,
                  child: const Text('Open full settings'),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _ConnectionPage extends StatefulWidget {
  const _ConnectionPage({required this.ble});

  final RiftLinkBle ble;

  @override
  State<_ConnectionPage> createState() => _ConnectionPageState();
}

class _ConnectionPageState extends State<_ConnectionPage> {
  late final TextEditingController _ssidController = TextEditingController();
  late final TextEditingController _passController = TextEditingController();

  bool _busy = false;
  int? _blePin;
  String _radioMode = 'ble';
  String? _wifiSsid;
  String? _wifiIp;

  @override
  void initState() {
    super.initState();
    _refreshFromInfo();
  }

  @override
  void dispose() {
    _ssidController.dispose();
    _passController.dispose();
    super.dispose();
  }

  void _refreshFromInfo() {
    final info = widget.ble.lastInfo;
    if (info == null) return;
    _blePin = info.blePin;
    _radioMode = info.radioMode;
    _wifiSsid = info.wifiSsid;
    _wifiIp = info.wifiIp;
  }

  void _snack(String t) => showAppSnackBar(context, t);

  Future<void> _withBusy(Future<void> Function() action) async {
    if (_busy) return;
    setState(() => _busy = true);
    try {
      await action();
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _refreshInfo() async {
    await widget.ble.getInfo(force: true);
    if (!mounted) return;
    setState(_refreshFromInfo);
  }

  @override
  Widget build(BuildContext context) {
    final p = context.palette;
    return Scaffold(
      backgroundColor: p.surface,
      appBar: AppBar(title: const Text('Connection')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
        children: [
          AppSectionCard(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('Current mode', style: TextStyle(color: p.onSurfaceVariant)),
                const SizedBox(height: 8),
                AppStateChip(
                  label: _radioMode == 'wifi' ? 'Wi-Fi' : 'BLE',
                  kind: AppStateKind.info,
                ),
                const SizedBox(height: 12),
                if (_wifiSsid != null && _wifiSsid!.isNotEmpty)
                  Text('SSID: $_wifiSsid', style: TextStyle(color: p.onSurfaceVariant)),
                if (_wifiIp != null && _wifiIp!.isNotEmpty)
                  Text('IP: $_wifiIp', style: TextStyle(color: p.onSurfaceVariant)),
                const SizedBox(height: 12),
                AppSecondaryButton(
                  onPressed: _busy
                      ? null
                      : () => _withBusy(() async {
                            final ok = await widget.ble.switchToBle();
                            await _refreshInfo();
                            _snack(ok ? 'Switched to BLE' : 'Switch failed');
                          }),
                  child: const Text('Switch to BLE'),
                ),
                const SizedBox(height: 8),
                TextField(
                  controller: _ssidController,
                  decoration: const InputDecoration(labelText: 'Wi-Fi SSID'),
                ),
                const SizedBox(height: 8),
                TextField(
                  controller: _passController,
                  obscureText: true,
                  decoration: const InputDecoration(labelText: 'Wi-Fi password'),
                ),
                const SizedBox(height: 12),
                AppPrimaryButton(
                  onPressed: _busy
                      ? null
                      : () => _withBusy(() async {
                            final ssid = _ssidController.text.trim();
                            final pass = _passController.text;
                            if (ssid.isEmpty) {
                              _snack('SSID is required');
                              return;
                            }
                            final ok = await widget.ble.switchToWifiSta(ssid: ssid, pass: pass);
                            await _refreshInfo();
                            _snack(ok ? 'Switch command sent' : 'Switch failed');
                          }),
                  child: const Text('Switch to Wi-Fi STA'),
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),
          AppSectionCard(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('BLE PIN', style: TextStyle(color: p.onSurfaceVariant)),
                const SizedBox(height: 8),
                SelectableText(
                  _blePin?.toString() ?? '—',
                  style: TextStyle(
                    color: p.onSurface,
                    fontFamily: 'monospace',
                    fontSize: 16,
                    fontWeight: FontWeight.w700,
                  ),
                ),
                const SizedBox(height: 12),
                AppSecondaryButton(
                  onPressed: _busy
                      ? null
                      : () => _withBusy(() async {
                            final ok = await widget.ble.regeneratePin();
                            await _refreshInfo();
                            _snack(ok ? 'PIN regenerated' : 'Failed to regenerate PIN');
                          }),
                  child: const Text('Regenerate PIN'),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _SecurityPage extends StatefulWidget {
  const _SecurityPage({required this.ble});

  final RiftLinkBle ble;

  @override
  State<_SecurityPage> createState() => _SecurityPageState();
}

class _SecurityPageState extends State<_SecurityPage> {
  late final TextEditingController _inviteIdController = TextEditingController();
  late final TextEditingController _inviteKeyController = TextEditingController();
  late final TextEditingController _inviteChannelKeyController =
      TextEditingController();
  late final TextEditingController _inviteTokenController =
      TextEditingController();
  bool _busy = false;

  @override
  void dispose() {
    _inviteIdController.dispose();
    _inviteKeyController.dispose();
    _inviteChannelKeyController.dispose();
    _inviteTokenController.dispose();
    super.dispose();
  }

  void _snack(String t) => showAppSnackBar(context, t);

  Future<void> _withBusy(Future<void> Function() action) async {
    if (_busy) return;
    setState(() => _busy = true);
    try {
      await action();
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final p = context.palette;
    return Scaffold(
      backgroundColor: p.surface,
      appBar: AppBar(title: const Text('Security')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
        children: [
          AppSectionCard(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('E2E invite', style: TextStyle(color: p.onSurface)),
                const SizedBox(height: 8),
                Text(
                  'Create new invite from device and accept invite data from trusted peers.',
                  style: TextStyle(color: p.onSurfaceVariant),
                ),
                const SizedBox(height: 12),
                AppPrimaryButton(
                  onPressed: _busy
                      ? null
                      : () => _withBusy(() async {
                            final ok = await widget.ble.createInvite();
                            _snack(ok ? 'Invite requested' : 'Failed to create invite');
                          }),
                  child: const Text('Create invite'),
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),
          AppSectionCard(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('Accept invite', style: TextStyle(color: p.onSurface)),
                const SizedBox(height: 8),
                TextField(
                  controller: _inviteIdController,
                  maxLength: 16,
                  decoration: const InputDecoration(
                    labelText: 'Inviter ID (hex)',
                    counterText: '',
                  ),
                ),
                const SizedBox(height: 8),
                TextField(
                  controller: _inviteKeyController,
                  minLines: 2,
                  maxLines: 3,
                  decoration: const InputDecoration(labelText: 'Public key'),
                ),
                const SizedBox(height: 8),
                TextField(
                  controller: _inviteChannelKeyController,
                  minLines: 1,
                  maxLines: 2,
                  decoration: const InputDecoration(
                    labelText: 'Channel key (optional)',
                  ),
                ),
                const SizedBox(height: 8),
                TextField(
                  controller: _inviteTokenController,
                  maxLength: 16,
                  decoration: const InputDecoration(
                    labelText: 'Invite token (optional)',
                    counterText: '',
                  ),
                ),
                const SizedBox(height: 12),
                AppPrimaryButton(
                  onPressed: _busy
                      ? null
                      : () => _withBusy(() async {
                            final id = _inviteIdController.text
                                .trim()
                                .replaceAll(RegExp(r'[^0-9A-Fa-f]'), '');
                            final pubKey = _inviteKeyController.text.trim();
                            final channelKey = _inviteChannelKeyController.text.trim();
                            final token = _inviteTokenController.text
                                .trim()
                                .replaceAll(RegExp(r'[^0-9A-Fa-f]'), '');
                            if (id.length < 8 || pubKey.isEmpty) {
                              _snack('Invite ID and public key are required');
                              return;
                            }
                            final ok = await widget.ble.acceptInvite(
                              id: id.substring(0, id.length > 16 ? 16 : id.length),
                              pubKey: pubKey,
                              channelKey:
                                  channelKey.isEmpty ? null : channelKey,
                              inviteToken: token.isEmpty ? null : token,
                            );
                            _snack(ok ? 'Invite accepted' : 'Invite accept failed');
                          }),
                  child: const Text('Accept invite'),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _DeviceProfilePage extends StatefulWidget {
  const _DeviceProfilePage({
    required this.ble,
    required this.nodeId,
    required this.nickname,
  });

  final RiftLinkBle ble;
  final String nodeId;
  final String nickname;

  @override
  State<_DeviceProfilePage> createState() => _DeviceProfilePageState();
}

class _DeviceProfilePageState extends State<_DeviceProfilePage> {
  late final TextEditingController _controller =
      TextEditingController(text: widget.nickname);

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  void _snack(String t) => showAppSnackBar(context, t);

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    return Scaffold(
      backgroundColor: context.palette.surface,
      appBar: AppBar(title: const Text('Device profile')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
        children: [
          AppSectionCard(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(l.tr('settings_node_id'), style: TextStyle(color: context.palette.onSurfaceVariant)),
                const SizedBox(height: 8),
                SelectableText(
                  _formatNodeIdDisplay(widget.nodeId),
                  style: TextStyle(color: context.palette.onSurface, fontFamily: 'monospace', fontSize: 15),
                ),
                const SizedBox(height: 12),
                AppSecondaryButton(
                  onPressed: () async {
                    await Clipboard.setData(
                      ClipboardData(text: _nodeIdForClipboard(widget.nodeId)),
                    );
                    if (mounted) _snack(l.tr('copied'));
                  },
                  child: Text(l.tr('copy')),
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),
          AppSectionCard(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(l.tr('nickname'), style: TextStyle(color: context.palette.onSurface)),
                const SizedBox(height: 8),
                TextField(
                  controller: _controller,
                  maxLength: 16,
                  decoration: InputDecoration(hintText: l.tr('nickname_hint'), counterText: ''),
                ),
                const SizedBox(height: 12),
                AppPrimaryButton(
                  onPressed: () async {
                    final n = _controller.text.trim();
                    final ok = await widget.ble.setNickname(n);
                    if (!mounted) return;
                    if (!ok) {
                      _snack(l.tr('error'));
                      return;
                    }
                    _snack(l.tr('saved'));
                    Navigator.pop(context, n);
                  },
                  child: Text(l.tr('save')),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _MeshRegionPage extends StatefulWidget {
  const _MeshRegionPage({
    required this.ble,
    required this.region,
    required this.channel,
  });

  final RiftLinkBle ble;
  final String region;
  final int? channel;

  @override
  State<_MeshRegionPage> createState() => _MeshRegionPageState();
}

class _MeshRegionPageState extends State<_MeshRegionPage> {
  late String _region = widget.region;
  late int? _channel = widget.channel;
  static const _regions = ['EU', 'RU', 'UK', 'US', 'AU'];

  void _snack(String t) => showAppSnackBar(context, t);

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final isEu = _region == 'EU' || _region == 'UK';
    return Scaffold(
      backgroundColor: context.palette.surface,
      appBar: AppBar(title: const Text('Mesh network')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
        children: [
          AppSectionCard(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(l.tr('region'), style: TextStyle(color: context.palette.onSurface)),
                const SizedBox(height: 8),
                Wrap(
                  spacing: 8,
                  runSpacing: 8,
                  children: _regions.map((r) {
                    return ChoiceChip(
                      label: Text(r),
                      selected: _region == r,
                      onSelected: (_) async {
                        final ok = await widget.ble.setRegion(r);
                        if (!mounted) return;
                        if (!ok) {
                          _snack(l.tr('error'));
                          return;
                        }
                        setState(() => _region = r);
                        _snack(l.tr('saved'));
                      },
                    );
                  }).toList(),
                ),
                if (isEu) ...[
                  const SizedBox(height: 12),
                  Text('Channel', style: TextStyle(color: context.palette.onSurface)),
                  const SizedBox(height: 8),
                  Wrap(
                    spacing: 8,
                    children: List.generate(3, (i) {
                      return ChoiceChip(
                        label: Text('$i'),
                        selected: _channel == i,
                        onSelected: (_) async {
                          final ok = await widget.ble.setChannel(i);
                          if (!mounted) return;
                          if (!ok) {
                            _snack(l.tr('error'));
                            return;
                          }
                          setState(() => _channel = i);
                          _snack(l.tr('saved'));
                        },
                      );
                    }),
                  ),
                ],
                const SizedBox(height: 16),
                AppPrimaryButton(
                  onPressed: () => Navigator.pop(
                    context,
                    (region: _region, channel: isEu ? _channel : null),
                  ),
                  child: Text(l.tr('ok')),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _DiagnosticsPage extends StatelessWidget {
  const _DiagnosticsPage({
    required this.version,
    required this.batteryMv,
    required this.batteryPercent,
    required this.offlinePending,
    required this.gpsPresent,
    required this.gpsEnabled,
    required this.gpsFix,
    required this.powersave,
  });

  final String? version;
  final int? batteryMv;
  final int? batteryPercent;
  final int? offlinePending;
  final bool gpsPresent;
  final bool gpsEnabled;
  final bool gpsFix;
  final bool powersave;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: context.palette.surface,
      appBar: AppBar(title: const Text('Diagnostics')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
        children: [
          AppSectionCard(
            child: Column(
              children: [
                ListTile(
                  contentPadding: EdgeInsets.zero,
                  title: const Text('Firmware version'),
                  subtitle: Text(version?.isNotEmpty == true ? version! : '—'),
                ),
                Divider(height: 1, color: context.palette.divider),
                ListTile(
                  contentPadding: EdgeInsets.zero,
                  title: const Text('Battery'),
                  subtitle: Text(
                    batteryMv != null && batteryMv! > 0
                        ? (batteryPercent != null
                            ? '$batteryPercent% (${(batteryMv! / 1000).toStringAsFixed(2)} V)'
                            : '${(batteryMv! / 1000).toStringAsFixed(2)} V')
                        : '—',
                  ),
                ),
                Divider(height: 1, color: context.palette.divider),
                ListTile(
                  contentPadding: EdgeInsets.zero,
                  title: const Text('Offline queue'),
                  subtitle: Text('${offlinePending ?? 0} pending'),
                ),
                Divider(height: 1, color: context.palette.divider),
                ListTile(
                  contentPadding: EdgeInsets.zero,
                  title: const Text('GPS'),
                  subtitle: Text(gpsPresent ? 'enabled: $gpsEnabled, fix: $gpsFix' : 'not present'),
                ),
                Divider(height: 1, color: context.palette.divider),
                ListTile(
                  contentPadding: EdgeInsets.zero,
                  title: const Text('Power save'),
                  subtitle: Text(powersave ? 'on' : 'off'),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

