import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../widgets/mesh_background.dart';

class SettingsScreen extends StatefulWidget {
  final RiftLinkBle ble;
  final String nodeId;
  final String? nickname;
  final String region;
  final int? channel;
  final bool gpsPresent, gpsEnabled, gpsFix, powersave;
  final int? offlinePending, batteryMv;
  final VoidCallback onDisconnect;
  final void Function(String) onNicknameChanged;
  final void Function(String, int?) onRegionChanged;
  final void Function(bool) onPowersaveChanged;
  final void Function(bool) onGpsChanged;
  final bool meshAnimationEnabled;
  final void Function(bool) onMeshAnimationChanged;

  const SettingsScreen({super.key, required this.ble, required this.nodeId, this.nickname, required this.region, this.channel,
    this.gpsPresent = false, this.gpsEnabled = false, this.gpsFix = false, this.powersave = false,
    this.offlinePending, this.batteryMv, required this.onDisconnect, required this.onNicknameChanged,
    required this.onRegionChanged, required this.onPowersaveChanged, required this.onGpsChanged,
    this.meshAnimationEnabled = true, required this.onMeshAnimationChanged});

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  late final TextEditingController _nickController;
  late final TextEditingController _wifiSsidController;
  late final TextEditingController _wifiPassController;
  late final TextEditingController _inviteIdController;
  late final TextEditingController _inviteKeyController;

  late String _region;
  late int? _channel;
  late bool _gpsEnabled;
  late bool _powersave;
  late bool _meshAnimationEnabled;

  void _syncFromWidget() {
    _region = widget.region;
    _channel = widget.channel;
    _gpsEnabled = widget.gpsEnabled;
    _powersave = widget.powersave;
    _meshAnimationEnabled = widget.meshAnimationEnabled;
  }

  @override
  void initState() {
    super.initState();
    _syncFromWidget();
    _nickController = TextEditingController(text: widget.nickname ?? '');
    _wifiSsidController = TextEditingController();
    _wifiPassController = TextEditingController();
    _inviteIdController = TextEditingController();
    _inviteKeyController = TextEditingController();
  }

  @override
  void didUpdateWidget(covariant SettingsScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    _syncFromWidget();
  }

  @override
  void dispose() {
    _nickController.dispose(); _wifiSsidController.dispose(); _wifiPassController.dispose();
    _inviteIdController.dispose(); _inviteKeyController.dispose(); super.dispose();
  }

  void _snack(String t) => ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(t)));

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final regions = ['EU', 'RU', 'UK', 'US', 'AU'];
    final isEu = _region == 'EU' || _region == 'UK';

    return Scaffold(
      backgroundColor: AppColors.surface,
      appBar: AppBar(title: Text(l.tr('settings')), leading: IconButton(icon: const Icon(Icons.arrow_back), onPressed: () => Navigator.pop(context))),
      body: MeshBackgroundWrapper(
        child: Material(
          color: Colors.transparent,
          child: ListView(padding: const EdgeInsets.all(16), children: [
        _section(l.tr('connection'), [
          ListTile(
            leading: const Icon(Icons.link_off, color: AppColors.error),
            title: Text(l.tr('disconnect'), style: const TextStyle(color: AppColors.onSurface, fontWeight: FontWeight.w600)),
            onTap: widget.onDisconnect,
          ),
        ]),
        _section(l.tr('nickname'), [
          Padding(padding: const EdgeInsets.all(16), child: Row(children: [
            Expanded(child: TextField(controller: _nickController, maxLength: 16, style: const TextStyle(color: AppColors.onSurface), decoration: InputDecoration(hintText: l.tr('nickname_hint')))),
            const SizedBox(width: 8),
            ElevatedButton(
              style: ElevatedButton.styleFrom(backgroundColor: AppColors.primary, foregroundColor: Colors.white),
              onPressed: widget.ble.isConnected ? () async {
                final n = _nickController.text.trim();
                if (await widget.ble.setNickname(n)) { widget.onNicknameChanged(n); if (mounted) _snack(l.tr('saved')); }
              } : null,
              child: Text(l.tr('ok')),
            ),
          ])),
        ]),
        _section(l.tr('region'), [
          Padding(padding: const EdgeInsets.all(16), child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Wrap(spacing: 8, runSpacing: 8, children: regions.map((r) => FilterChip(
              label: Text(r, style: const TextStyle(color: AppColors.onSurface)),
              selected: _region == r,
              selectedColor: AppColors.primary.withOpacity(0.2),
              backgroundColor: AppColors.surface,
              checkmarkColor: AppColors.primary,
              side: BorderSide(color: _region == r ? AppColors.primary : const Color(0xFFBDBDBD)),
              onSelected: (_) async { if (await widget.ble.setRegion(r)) { widget.onRegionChanged(r, _channel); setState(() => _region = r); } },
            )).toList()),
            if (isEu) ...[
              const SizedBox(height: 12),
              Text(l.tr('channel_eu'), style: const TextStyle(color: AppColors.onSurface, fontSize: 13, fontWeight: FontWeight.w500)),
              const SizedBox(height: 6),
              Row(children: [0, 1, 2].map((ch) => Padding(padding: const EdgeInsets.only(right: 8), child: FilterChip(
                label: Text('$ch', style: const TextStyle(color: AppColors.onSurface)),
                selected: _channel == ch,
                selectedColor: AppColors.primary.withOpacity(0.2),
                backgroundColor: AppColors.surface,
                checkmarkColor: AppColors.primary,
                side: BorderSide(color: _channel == ch ? AppColors.primary : const Color(0xFFBDBDBD)),
                onSelected: (_) async { if (await widget.ble.setChannel(ch)) { widget.onRegionChanged(_region, ch); setState(() => _channel = ch); } },
              ))).toList()),
            ],
          ])),
        ]),
        _section('WiFi', [
          Padding(padding: const EdgeInsets.all(16), child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
            TextField(controller: _wifiSsidController, style: const TextStyle(color: AppColors.onSurface), decoration: const InputDecoration(labelText: 'SSID')),
            const SizedBox(height: 8),
            TextField(controller: _wifiPassController, obscureText: true, style: const TextStyle(color: AppColors.onSurface), decoration: const InputDecoration(labelText: 'Password')),
            const SizedBox(height: 12),
            ElevatedButton(
              style: ElevatedButton.styleFrom(backgroundColor: AppColors.primary, foregroundColor: Colors.white),
              onPressed: widget.ble.isConnected ? () async {
                final ssid = _wifiSsidController.text.trim();
                if (ssid.isEmpty) return;
                await widget.ble.setWifi(ssid: ssid, pass: _wifiPassController.text);
                if (mounted) _snack(l.tr('wifi_connect'));
              } : null,
              child: Text(l.tr('connect')),
            ),
          ])),
        ]),
        if (widget.gpsPresent) _section('GPS', [
          SwitchListTile(
            title: Text(l.tr('gps_enable'), style: const TextStyle(color: AppColors.onSurface)),
            subtitle: Text(widget.gpsFix ? l.tr('gps_fix_yes') : l.tr('gps_fix_no'), style: const TextStyle(color: AppColors.onSurfaceVariant)),
            value: _gpsEnabled,
            activeColor: AppColors.primary,
            onChanged: widget.ble.isConnected ? (v) async { if (await widget.ble.setGps(v)) { widget.onGpsChanged(v); setState(() => _gpsEnabled = v); } } : null,
          ),
        ]),
        _section(l.tr('powersave'), [
          SwitchListTile(
            title: const Text('Powersave', style: TextStyle(color: AppColors.onSurface)),
            value: _powersave,
            activeColor: AppColors.primary,
            onChanged: widget.ble.isConnected ? (v) async { if (await widget.ble.setPowersave(v)) { widget.onPowersaveChanged(v); setState(() => _powersave = v); } } : null,
          ),
        ]),
        _section(l.tr('e2e_invite'), [
          ListTile(
            leading: const Icon(Icons.vpn_key, color: AppColors.primary),
            title: Text(l.tr('create_invite'), style: const TextStyle(color: AppColors.onSurface)),
            onTap: widget.ble.isConnected ? () => widget.ble.createInvite() : null,
          ),
          Padding(padding: const EdgeInsets.all(16), child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
            TextField(controller: _inviteIdController, style: const TextStyle(color: AppColors.onSurface), decoration: InputDecoration(labelText: l.tr('inviter_id')), maxLength: 16),
            const SizedBox(height: 8),
            TextField(controller: _inviteKeyController, style: const TextStyle(color: AppColors.onSurface), decoration: const InputDecoration(labelText: 'PubKey base64'), maxLines: 2),
            const SizedBox(height: 12),
            ElevatedButton(
              style: ElevatedButton.styleFrom(backgroundColor: AppColors.primary, foregroundColor: Colors.white),
              onPressed: widget.ble.isConnected ? () async {
                final id = _inviteIdController.text.trim().replaceAll(RegExp(r'[^0-9A-Fa-f]'), '');
                final key = _inviteKeyController.text.trim();
                if (id.length < 8 || key.isEmpty) return;
                if (await widget.ble.acceptInvite(id: id.substring(0, id.length > 16 ? 16 : id.length), pubKey: key)) { if (mounted) _snack(l.tr('invite_accepted')); }
              } : null,
              child: Text(l.tr('accept_invite')),
            ),
          ])),
        ]),
        _section(l.tr('other'), [
          SwitchListTile(
            title: Text(l.tr('mesh_animation'), style: const TextStyle(color: AppColors.onSurface)),
            value: _meshAnimationEnabled,
            activeColor: AppColors.primary,
            onChanged: (v) { widget.onMeshAnimationChanged(v); setState(() => _meshAnimationEnabled = v); },
          ),
          if (widget.offlinePending != null && widget.offlinePending! > 0) ListTile(leading: const Icon(Icons.schedule, color: AppColors.onSurfaceVariant), title: Text('${l.tr('offline_pending')}: ${widget.offlinePending}', style: const TextStyle(color: AppColors.onSurface))),
          if (widget.batteryMv != null && widget.batteryMv! > 0) ListTile(leading: const Icon(Icons.battery_charging_full, color: AppColors.onSurfaceVariant), title: Text('${(widget.batteryMv! / 1000).toStringAsFixed(2)} V', style: const TextStyle(color: AppColors.onSurface))),
          ListTile(leading: const Icon(Icons.health_and_safety, color: AppColors.primary), title: Text(l.tr('selftest'), style: const TextStyle(color: AppColors.onSurface)), onTap: widget.ble.isConnected ? () => widget.ble.selftest() : null),
        ]),
        const SizedBox(height: 32),
      ]),
        ),
        ),
    );
  }

  Widget _section(String title, List<Widget> children) {
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      const SizedBox(height: 20),
      Text(title, style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: AppColors.onSurface)),
      const SizedBox(height: 8),
      Container(
        decoration: BoxDecoration(color: AppColors.surface, borderRadius: BorderRadius.circular(12), border: Border.all(color: AppColors.divider)),
        child: Column(children: children),
      ),
    ]);
  }
}
