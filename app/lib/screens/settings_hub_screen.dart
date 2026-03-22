import 'dart:async';
import 'dart:convert';
import 'dart:ui' show FontFeature;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../ble/riftlink_ble.dart';
import '../contacts/contacts_service.dart';
import '../l10n/app_localizations.dart';
import '../locale_notifier.dart';
import '../prefs/mesh_prefs.dart';
import '../theme/app_theme.dart';
import '../theme/design_tokens.dart';
import '../theme/theme_notifier.dart';
import '../widgets/app_primitives.dart';
import '../widgets/mesh_background.dart';
import '../widgets/app_snackbar.dart';
import 'ota_screen.dart';

// ─── utilities ─────────────────────────────────────────────────────────────

String _nodeIdForClipboard(String raw) =>
    raw.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();

String _fmtId(String raw) {
  final c = _nodeIdForClipboard(raw);
  if (c.isEmpty) return raw.trim();
  final p = <String>[];
  for (var i = 0; i < c.length; i += 4) {
    p.add(c.substring(i, i + 4 > c.length ? c.length : i + 4));
  }
  return p.join(' ');
}

String _normHex(String raw) =>
    raw.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();

bool _samePeerId(String a, String b) {
  final an = _normHex(a);
  final bn = _normHex(b);
  if (an.length != 16 || bn.length != 16) return false;
  return an == bn;
}

bool _peerHasKey(RiftLinkInfoEvent evt, String peerId) {
  for (var i = 0; i < evt.neighbors.length; i++) {
    if (!_samePeerId(evt.neighbors[i], peerId)) continue;
    if (i < evt.neighborsHasKey.length && evt.neighborsHasKey[i] == true) return true;
    return false;
  }
  return false;
}

// ─── motion constants ──────────────────────────────────────────────────────

const int _kBaseMs = 220;
const int _kStepMs = 40;
const Duration _kDur = Duration(milliseconds: _kBaseMs);
const Curve _kIn = Curves.easeOutCubic;
const Curve _kOut = Curves.easeInCubic;

Widget _stagger(int idx, Widget child) {
  return TweenAnimationBuilder<double>(
    tween: Tween(begin: 0, end: 1),
    duration: Duration(milliseconds: _kBaseMs + _kStepMs * idx),
    curve: _kIn,
    builder: (_, v, __) => Opacity(
      opacity: v,
      child: Transform.translate(offset: Offset(0, 14 * (1 - v)), child: child),
    ),
  );
}

// ═══════════════════════════════════════════════════════════════════════════
//  SettingsHubScreen — единственная точка входа в настройки
// ═══════════════════════════════════════════════════════════════════════════

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
  State<SettingsHubScreen> createState() => _HubState();
}

class _HubState extends State<SettingsHubScreen> with WidgetsBindingObserver {
  late String _nodeId;
  String? _nickname;
  late String _region;
  int? _channel, _sf;
  double? _bw;
  int? _cr, _modemPreset;
  late bool _gpsPresent, _gpsEnabled, _gpsFix, _powersave, _meshAnim;
  int? _offPend, _offCourier, _offDirect, _batMv, _batPct;
  bool _charging = false;
  int? _blePin;
  String? _version;
  String _radioMode = 'ble';
  String? _radioVariant;
  bool _wifiConn = false;
  String? _wifiSsid, _wifiIp;
  int _espCh = 1;
  bool _espAdapt = false;
  StreamSubscription<RiftLinkEvent>? _sub;
  Timer? _infoPollTimer;
  Map<String, String> _nickById = const {};

  void _apply(RiftLinkInfoEvent e) {
    if (!mounted) return;
    setState(() {
      if (e.id.isNotEmpty) _nodeId = e.id;
      _region = e.region;
      if (e.hasChannelField) _channel = e.channel;
      _sf = e.sf; _bw = e.bw; _cr = e.cr; _modemPreset = e.modemPreset;
      _gpsPresent = e.gpsPresent; _gpsEnabled = e.gpsEnabled; _gpsFix = e.gpsFix;
      _powersave = e.powersave;
      if (e.hasOfflinePendingField) _offPend = e.offlinePending;
      if (e.hasOfflineCourierPendingField) _offCourier = e.offlineCourierPending;
      if (e.hasOfflineDirectPendingField) _offDirect = e.offlineDirectPending;
      if (e.batteryMv != null && e.batteryMv! > 0) _batMv = e.batteryMv;
      _batPct = e.batteryPercent; _charging = e.charging;
      _blePin = e.blePin; _version = e.version;
      _radioMode = e.radioMode; _radioVariant = e.radioVariant;
      _wifiConn = e.wifiConnected; _wifiSsid = e.wifiSsid; _wifiIp = e.wifiIp;
      if (e.espNowChannel != null && e.espNowChannel! >= 1 && e.espNowChannel! <= 13) _espCh = e.espNowChannel!;
      _espAdapt = e.espNowAdaptive;
      if (e.hasNicknameField && e.nickname != null && e.nickname!.trim().isNotEmpty) _nickname = e.nickname!.trim();
    });
  }

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    final li = widget.ble.lastInfo;
    _nodeId = (li != null && li.id.isNotEmpty) ? li.id : widget.nodeId;
    _nickname = li?.nickname?.trim().isNotEmpty == true ? li!.nickname!.trim() : widget.nickname;
    _region = li?.region ?? widget.region;
    _channel = li?.channel ?? widget.channel;
    _sf = li?.sf ?? widget.sf; _bw = li?.bw; _cr = li?.cr;
    _modemPreset = li?.modemPreset;
    _gpsPresent = li?.gpsPresent ?? widget.gpsPresent;
    _gpsEnabled = li?.gpsEnabled ?? widget.gpsEnabled;
    _gpsFix = li?.gpsFix ?? widget.gpsFix;
    _powersave = li?.powersave ?? widget.powersave;
    _meshAnim = widget.meshAnimationEnabled;
    _offPend = li?.offlinePending ?? widget.offlinePending;
    _offCourier = li?.offlineCourierPending;
    _offDirect = li?.offlineDirectPending;
    _batMv = li?.batteryMv ?? widget.batteryMv;
    _batPct = li?.batteryPercent; _charging = li?.charging ?? false;
    _blePin = li?.blePin; _version = li?.version;
    _radioMode = li?.radioMode ?? 'ble'; _radioVariant = li?.radioVariant;
    _wifiConn = li?.wifiConnected ?? false; _wifiSsid = li?.wifiSsid; _wifiIp = li?.wifiIp;
    _espCh = (li?.espNowChannel != null && li!.espNowChannel! >= 1 && li.espNowChannel! <= 13) ? li.espNowChannel! : 1;
    _espAdapt = li?.espNowAdaptive ?? false;
    _loadContactNicknames();
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      if (evt is RiftLinkInfoEvent) _apply(evt);
      else if (evt is RiftLinkRegionEvent) setState(() { _region = evt.region; _channel = evt.channel; });
      else if (evt is RiftLinkGpsEvent) setState(() { _gpsPresent = evt.present; _gpsEnabled = evt.enabled; _gpsFix = evt.hasFix; });
    });
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || !widget.ble.isConnected) return;
      final c = widget.ble.lastInfo;
      if (c != null) _apply(c);
      widget.ble.getInfo();
    });
    _infoPollTimer = Timer.periodic(const Duration(seconds: 10), (_) {
      if (!mounted || !widget.ble.isConnected) return;
      widget.ble.getInfo();
    });
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState s) {
    if (s == AppLifecycleState.resumed && mounted && widget.ble.isConnected) {
      final c = widget.ble.lastInfo;
      if (c != null) _apply(c);
      widget.ble.getInfo();
    }
  }

  @override
  void didUpdateWidget(covariant SettingsHubScreen o) {
    super.didUpdateWidget(o);
    if (o.meshAnimationEnabled != widget.meshAnimationEnabled) setState(() => _meshAnim = widget.meshAnimationEnabled);
  }

  @override
  void dispose() {
    _infoPollTimer?.cancel();
    WidgetsBinding.instance.removeObserver(this);
    _sub?.cancel();
    super.dispose();
  }

  Future<void> _loadContactNicknames() async {
    final contacts = await ContactsService.load();
    if (!mounted) return;
    setState(() => _nickById = ContactsService.buildNicknameMap(contacts));
  }

  String _presetTag(AppLocalizations l) => switch (_modemPreset) {
    0 => l.tr('modem_preset_speed'), 1 => l.tr('modem_preset_normal'),
    2 => l.tr('modem_preset_range'), 3 => l.tr('modem_preset_maxrange'),
    4 => l.tr('modem_preset_custom'), _ => '—',
  };

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final pal = context.palette;

    final selfLabel = (_nickname != null && _nickname!.isNotEmpty)
        ? _nickname!
        : ContactsService.displayNodeLabel(_nodeId, _nickById);
    final devSub = selfLabel;
    final netSub = '$_region · ${_presetTag(l)}';
    final connSub = _radioMode == 'wifi'
        ? 'Wi-Fi STA${_wifiConn ? ' · ${_wifiSsid ?? ""}' : ""}'
        : 'BLE${_blePin != null ? " · PIN: $_blePin" : ""}';
    final enSub = '${_powersave ? l.tr("powersave_mode_eco") : l.tr("powersave_mode_normal")}'
        '${_gpsPresent ? " · GPS: ${_gpsEnabled ? "ON" : "OFF"}" : ""}';
    final diagSub = [
      if (_batPct != null) '$_batPct%',
      if (_version != null && _version!.isNotEmpty) 'v$_version',
    ].join(' · ');

    return Scaffold(
      backgroundColor: pal.surface,
      appBar: riftAppBar(context, showBack: true, title: l.tr('settings')),
      body: MeshBackgroundWrapper(
        child: SafeArea(
          child: ListView(
            padding: const EdgeInsets.fromLTRB(AppSpacing.lg, AppSpacing.sm, AppSpacing.lg, AppSpacing.xxl + AppSpacing.sm),
            children: [
              _stagger(0, _HubCard(icon: Icons.devices_rounded, iconColor: pal.primary,
                title: l.tr('settings_device'), subtitle: devSub,
                onTap: () => _push(_DevicePage(ble: widget.ble, nodeId: _nodeId, nickname: _nickname,
                  onNicknameChanged: (n) { widget.onNicknameChanged(n); setState(() => _nickname = n); })))),
              const SizedBox(height: AppSpacing.md),
              _stagger(1, _HubCard(icon: Icons.cell_tower_rounded, iconColor: Colors.orange,
                title: l.tr('settings_network_title'), subtitle: netSub,
                onTap: () => _push(_NetworkModemPage(ble: widget.ble, region: _region, channel: _channel,
                  sf: _sf, bw: _bw, cr: _cr, modemPreset: _modemPreset, radioMode: _radioMode,
                  espNowChannel: _espCh, espNowAdaptive: _espAdapt,
                  onRegionChanged: (r, c) { widget.onRegionChanged(r, c); setState(() { _region = r; _channel = c; }); },
                  onSfChanged: (v) { widget.onSfChanged(v); setState(() => _sf = v); })))),
              const SizedBox(height: AppSpacing.md),
              _stagger(2, _HubCard(icon: Icons.bluetooth_searching_rounded, iconColor: Colors.blueAccent,
                title: l.tr('connection'), subtitle: connSub,
                onTap: () => _push(_ConnectionPage(ble: widget.ble)))),
              const SizedBox(height: AppSpacing.md),
              _stagger(3, _HubCard(icon: Icons.shield_outlined, iconColor: Colors.green,
                title: l.tr('settings_security_title'), subtitle: l.tr('e2e_invite_hint'),
                onTap: () => _push(_SecurityPage(ble: widget.ble)))),
              const SizedBox(height: AppSpacing.md),
              _stagger(4, _HubCard(icon: Icons.battery_saver_rounded, iconColor: Colors.amber,
                title: l.tr('settings_energy_title'), subtitle: enSub,
                onTap: () => _push(_EnergyThemePage(ble: widget.ble, powersave: _powersave,
                  gpsPresent: _gpsPresent, gpsEnabled: _gpsEnabled, gpsFix: _gpsFix,
                  meshAnimationEnabled: _meshAnim,
                  onPowersaveChanged: (v) { widget.onPowersaveChanged(v); setState(() => _powersave = v); },
                  onGpsChanged: (v) { widget.onGpsChanged(v); setState(() => _gpsEnabled = v); },
                  onMeshAnimationChanged: (v) { widget.onMeshAnimationChanged(v); setState(() => _meshAnim = v); })))),
              const SizedBox(height: AppSpacing.md),
              _stagger(5, _HubCard(icon: Icons.analytics_outlined, iconColor: Colors.purple,
                title: l.tr('settings_diagnostics_title'),
                subtitle: diagSub.isNotEmpty ? diagSub : l.tr('settings_diagnostics_hint'),
                onTap: () => _push(_DiagnosticsPage(ble: widget.ble)))),
              const SizedBox(height: AppSpacing.md),
              _stagger(6, _HubCard(
                icon: Icons.help_outline_rounded,
                iconColor: Colors.teal,
                title: l.tr('faq_title'),
                subtitle: l.tr('faq_subtitle'),
                onTap: () => _push(const _FaqPage()),
              )),
            ],
          ),
        ),
      ),
    );
  }

  void _push(Widget page) {
    Navigator.push(context, MaterialPageRoute(builder: (_) => page)).then((_) {
      if (mounted && widget.ble.isConnected) widget.ble.getInfo();
    });
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  _HubCard — карточка хаба
// ═══════════════════════════════════════════════════════════════════════════

class _HubCard extends StatelessWidget {
  final IconData icon;
  final Color iconColor;
  final String title;
  final String subtitle;
  final VoidCallback onTap;

  const _HubCard({required this.icon, required this.iconColor, required this.title, required this.subtitle, required this.onTap});

  @override
  Widget build(BuildContext context) {
    final pal = context.palette;
    return AppSectionCard(
      padding: EdgeInsets.zero,
      child: InkWell(
        borderRadius: BorderRadius.circular(AppRadius.card),
        onTap: () { HapticFeedback.selectionClick(); onTap(); },
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: AppSpacing.lg, vertical: AppSpacing.md + 2),
          child: Row(children: [
            Container(
              width: 44, height: 44,
              decoration: BoxDecoration(color: iconColor.withOpacity(0.12), borderRadius: BorderRadius.circular(AppRadius.md)),
              child: Icon(icon, color: iconColor, size: 24),
            ),
            const SizedBox(width: AppSpacing.lg),
            Expanded(child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text(title, style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: pal.onSurface)),
              const SizedBox(height: 2),
              Text(subtitle, maxLines: 1, overflow: TextOverflow.ellipsis,
                style: TextStyle(fontSize: 12, color: pal.onSurfaceVariant)),
            ])),
            Icon(Icons.chevron_right_rounded, color: pal.onSurfaceVariant),
          ]),
        ),
      ),
    );
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  _AnimatedStatusChip — анимированная обратная связь (сохранено / ошибка)
// ═══════════════════════════════════════════════════════════════════════════

class _AnimatedStatusChip extends StatefulWidget {
  final String? label;
  final bool isError;
  const _AnimatedStatusChip({this.label, this.isError = false});
  @override
  State<_AnimatedStatusChip> createState() => _AnimatedStatusChipState();
}

class _AnimatedStatusChipState extends State<_AnimatedStatusChip> {
  Timer? _t;
  String? _shown;
  bool _err = false;

  @override
  void initState() { super.initState(); _shown = widget.label; _err = widget.isError; _arm(); }

  @override
  void didUpdateWidget(covariant _AnimatedStatusChip o) {
    super.didUpdateWidget(o);
    if (widget.label != o.label || widget.isError != o.isError) {
      _t?.cancel();
      setState(() { _shown = widget.label; _err = widget.isError; });
      _arm();
    }
  }

  void _arm() {
    if (_shown != null && !_err) {
      _t = Timer(const Duration(milliseconds: 2500), () { if (mounted) setState(() => _shown = null); });
    }
  }

  @override
  void dispose() { _t?.cancel(); super.dispose(); }

  @override
  Widget build(BuildContext context) {
    return AnimatedSize(
      duration: _kDur, curve: _kIn, alignment: Alignment.topLeft,
      child: AnimatedSwitcher(
        duration: _kDur, switchInCurve: _kIn, switchOutCurve: _kOut,
        child: _shown != null
            ? Padding(key: ValueKey('$_shown$_err'), padding: const EdgeInsets.only(top: AppSpacing.sm),
                child: AppStateChip(label: _shown!, kind: _err ? AppStateKind.error : AppStateKind.success))
            : const SizedBox.shrink(key: ValueKey('none')),
      ),
    );
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Shared scaffold / card / section header
// ═══════════════════════════════════════════════════════════════════════════

class _SubpageScaffold extends StatelessWidget {
  final String title;
  final List<Widget> children;
  const _SubpageScaffold({required this.title, required this.children});
  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: context.palette.surface,
      appBar: riftAppBar(context, showBack: true, title: title),
      body: MeshBackgroundWrapper(
        child: SafeArea(
          child: ListView(
            padding: const EdgeInsets.fromLTRB(AppSpacing.lg, AppSpacing.sm, AppSpacing.lg, AppSpacing.xxl + AppSpacing.sm),
            children: [
              for (var i = 0; i < children.length; i++) ...[
                if (i > 0) const SizedBox(height: AppSpacing.md),
                _stagger(i, children[i]),
              ],
            ],
          ),
        ),
      ),
    );
  }
}

class _PageCard extends StatelessWidget {
  final String? title;
  final String? subtitle;
  final Widget child;
  const _PageCard({this.title, this.subtitle, required this.child});
  @override
  Widget build(BuildContext context) {
    return AppSectionCard(
      padding: const EdgeInsets.fromLTRB(AppSpacing.lg, 14, AppSpacing.lg, AppSpacing.lg),
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        if (title != null) ...[
          Text(title!, style: TextStyle(fontSize: 17, fontWeight: FontWeight.w600, color: context.palette.onSurface, letterSpacing: 0.2)),
          if (subtitle != null) ...[
            const SizedBox(height: AppSpacing.xs),
            Text(subtitle!, style: TextStyle(fontSize: 12, color: context.palette.onSurfaceVariant, height: 1.3)),
          ],
          const SizedBox(height: AppSpacing.sm),
        ],
        child,
      ]),
    );
  }
}

class _SectionAccentHeader extends StatelessWidget {
  final String text;
  final IconData? trailingIcon;
  const _SectionAccentHeader({required this.text, this.trailingIcon});
  @override
  Widget build(BuildContext context) {
    final p = context.palette;
    return Row(children: [
      Container(width: 3, height: 14, decoration: BoxDecoration(color: p.primary, borderRadius: BorderRadius.circular(2))),
      const SizedBox(width: AppSpacing.sm + 2),
      Expanded(child: Text(text, style: TextStyle(fontSize: 14, fontWeight: FontWeight.w600, letterSpacing: 0.2, color: p.onSurface))),
      if (trailingIcon != null) Icon(trailingIcon, size: 18, color: p.onSurfaceVariant),
    ]);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  PAGE 1 — Устройство (Node ID + Nickname)
// ═══════════════════════════════════════════════════════════════════════════

class _DevicePage extends StatefulWidget {
  final RiftLinkBle ble;
  final String nodeId;
  final String? nickname;
  final void Function(String) onNicknameChanged;
  const _DevicePage({required this.ble, required this.nodeId, this.nickname, required this.onNicknameChanged});
  @override
  State<_DevicePage> createState() => _DevicePageState();
}

class _DevicePageState extends State<_DevicePage> {
  late String _nodeId;
  late final TextEditingController _nickCtrl;
  final _nickFocus = FocusNode();
  String? _copySt;
  bool _copyErr = false;
  String? _nickSt;
  bool _nickErr = false;
  StreamSubscription<RiftLinkEvent>? _sub;

  @override
  void initState() {
    super.initState();
    final li = widget.ble.lastInfo;
    _nodeId = (li != null && li.id.isNotEmpty) ? li.id : widget.nodeId;
    final nick = (li?.nickname?.trim().isNotEmpty == true) ? li!.nickname!.trim() : (widget.nickname ?? '');
    _nickCtrl = TextEditingController(text: nick);
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      if (evt is RiftLinkInfoEvent) {
        setState(() { if (evt.id.isNotEmpty) _nodeId = evt.id; });
        if (evt.hasNicknameField && evt.nickname != null && evt.nickname!.trim().isNotEmpty && !_nickFocus.hasFocus) {
          _nickCtrl.text = evt.nickname!.trim();
        }
      }
    });
    WidgetsBinding.instance.addPostFrameCallback((_) { if (mounted && widget.ble.isConnected) widget.ble.getInfo(); });
  }

  @override
  void dispose() { _sub?.cancel(); _nickCtrl.dispose(); _nickFocus.dispose(); super.dispose(); }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final pal = context.palette;
    final connected = widget.ble.isConnected;
    final plain = _nodeIdForClipboard(_nodeId);
    final shown = _fmtId(_nodeId);
    final displayLabel = _nickCtrl.text.trim().isNotEmpty ? _nickCtrl.text.trim() : shown;

    return _SubpageScaffold(title: l.tr('settings_device'), children: [
      _PageCard(title: l.tr('settings_node_id'), subtitle: l.tr('settings_node_id_hint'), child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          SelectableText(
            displayLabel,
            style: TextStyle(
              color: pal.onSurface,
              fontSize: 15,
              height: 1.35,
              fontFamily: displayLabel == shown ? 'monospace' : null,
              letterSpacing: 0.5,
            ),
          ),
          const SizedBox(height: AppSpacing.md),
          AppSecondaryButton(fullWidth: true, onPressed: plain.isEmpty ? null : () async {
            await Clipboard.setData(ClipboardData(text: plain));
            HapticFeedback.lightImpact();
            if (mounted) setState(() { _copySt = l.tr('copied'); _copyErr = false; });
          }, child: Row(mainAxisAlignment: MainAxisAlignment.center, mainAxisSize: MainAxisSize.min, children: [
            const Icon(Icons.copy, size: 18), SizedBox(width: AppSpacing.sm), Text(l.tr('copy')),
          ])),
          _AnimatedStatusChip(label: _copySt, isError: _copyErr),
        ],
      )),
      _PageCard(title: l.tr('nickname'), child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        TextField(controller: _nickCtrl, focusNode: _nickFocus, maxLength: 16,
          style: TextStyle(color: pal.onSurface), decoration: InputDecoration(hintText: l.tr('nickname_hint'), counterText: '')),
        const SizedBox(height: AppSpacing.md),
        AppPrimaryButton(onPressed: connected ? () async {
          final n = _nickCtrl.text.trim();
          if (await widget.ble.setNickname(n)) {
            widget.onNicknameChanged(n);
            HapticFeedback.lightImpact();
            if (mounted) setState(() { _nickSt = l.tr('saved'); _nickErr = false; });
          } else {
            if (mounted) setState(() { _nickSt = l.tr('error'); _nickErr = true; });
          }
        } : null, child: Text(l.tr('save'))),
        _AnimatedStatusChip(label: _nickSt, isError: _nickErr),
      ])),
    ]);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  PAGE 2 — Сеть и модем (Region + Channel + Modem + ESP-NOW)
// ═══════════════════════════════════════════════════════════════════════════

class _NetworkModemPage extends StatefulWidget {
  final RiftLinkBle ble;
  final String region;
  final int? channel, sf;
  final double? bw;
  final int? cr, modemPreset;
  final String radioMode;
  final int espNowChannel;
  final bool espNowAdaptive;
  final void Function(String, int?) onRegionChanged;
  final void Function(int?) onSfChanged;

  const _NetworkModemPage({required this.ble, required this.region, this.channel, this.sf, this.bw, this.cr,
    this.modemPreset, required this.radioMode, required this.espNowChannel, required this.espNowAdaptive,
    required this.onRegionChanged, required this.onSfChanged});

  @override
  State<_NetworkModemPage> createState() => _NetworkModemPageState();
}

class _NetworkModemPageState extends State<_NetworkModemPage> {
  late String _region;
  int? _channel, _sf;
  double? _bw;
  int? _cr, _modemPreset;
  String _radioMode = 'ble';
  int _espCh = 1;
  bool _espAdapt = false;
  bool _modemApplying = false;
  String? _regSt, _modemSt;
  bool _regErr = false, _modemErr = false;
  StreamSubscription<RiftLinkEvent>? _sub;

  @override
  void initState() {
    super.initState();
    final li = widget.ble.lastInfo;
    _region = li?.region ?? widget.region;
    _channel = li?.channel ?? widget.channel;
    _sf = li?.sf ?? widget.sf; _bw = li?.bw ?? widget.bw; _cr = li?.cr ?? widget.cr;
    _modemPreset = li?.modemPreset ?? widget.modemPreset;
    _radioMode = li?.radioMode ?? widget.radioMode;
    _espCh = (li?.espNowChannel != null && li!.espNowChannel! >= 1 && li.espNowChannel! <= 13) ? li.espNowChannel! : widget.espNowChannel;
    _espAdapt = li?.espNowAdaptive ?? widget.espNowAdaptive;
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      if (evt is RiftLinkInfoEvent) {
        setState(() {
          _region = evt.region;
          if (evt.hasChannelField) _channel = evt.channel;
          _sf = evt.sf; _bw = evt.bw; _cr = evt.cr; _modemPreset = evt.modemPreset;
          _radioMode = evt.radioMode;
          if (evt.espNowChannel != null && evt.espNowChannel! >= 1 && evt.espNowChannel! <= 13) _espCh = evt.espNowChannel!;
          _espAdapt = evt.espNowAdaptive;
        });
      } else if (evt is RiftLinkRegionEvent) {
        setState(() { _region = evt.region; _channel = evt.channel; });
      }
    });
    WidgetsBinding.instance.addPostFrameCallback((_) { if (mounted && widget.ble.isConnected) widget.ble.getInfo(); });
  }

  @override
  void dispose() { _sub?.cancel(); super.dispose(); }

  Future<bool> _waitModem({required int preset, int? sf, double? bw, int? cr}) async {
    // Firmware scheduleInfoNotify fires after ~600ms; wait before first poll.
    await Future<void>.delayed(const Duration(milliseconds: 700));
    for (var i = 0; i < 8; i++) {
      await widget.ble.getInfo(force: true);
      await Future<void>.delayed(const Duration(milliseconds: 400));
      final li = widget.ble.lastInfo;
      if (li != null && li.modemPreset == preset) {
        if (preset != 4) return true;
        if (sf != null && bw != null && cr != null && li.sf == sf && (li.bw! - bw).abs() < 0.2 && li.cr == cr) return true;
      }
    }
    return false;
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final pal = context.palette;
    final conn = widget.ble.isConnected;
    final regions = ['EU', 'RU', 'UK', 'US', 'AU'];
    final isEu = _region == 'EU' || _region == 'UK';

    return _SubpageScaffold(title: l.tr('settings_network_title'), children: [
      _PageCard(title: l.tr('region'), subtitle: l.tr('settings_region_hint'), child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Text(l.tr('region_warning'), style: TextStyle(color: pal.onSurfaceVariant, fontSize: 12, height: 1.35)),
          const SizedBox(height: AppSpacing.md + AppSpacing.xs),
          _SegmentedPickBar(leadingIcon: Icons.public_rounded, labels: regions,
            selectedIndex: regions.contains(_region) ? regions.indexOf(_region) : null, enabled: conn,
            onSelected: (i) async {
              final r = regions[i];
              if (await widget.ble.setRegion(r)) {
                widget.onRegionChanged(r, _channel);
                setState(() { _region = r; _regSt = l.tr('saved'); _regErr = false; });
              }
            }),
          _AnimatedStatusChip(label: _regSt, isError: _regErr),
          AnimatedSize(duration: _kDur, curve: _kIn,
            child: isEu ? Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
              const SizedBox(height: AppSpacing.lg + AppSpacing.xs),
              Row(children: [
                Icon(Icons.radio_button_checked, size: 18, color: pal.primary.withOpacity(0.9)),
                const SizedBox(width: AppSpacing.sm),
                Text(l.tr('channel_eu'), style: TextStyle(color: pal.onSurface, fontSize: 14, fontWeight: FontWeight.w600, letterSpacing: 0.2)),
              ]),
              const SizedBox(height: AppSpacing.sm + 2),
              _SegmentedPickBar(leadingIcon: Icons.layers_outlined, labels: const ['0', '1', '2'],
                selectedIndex: _channel != null && _channel! >= 0 && _channel! <= 2 ? _channel : null, enabled: conn,
                onSelected: (i) async {
                  if (await widget.ble.setChannel(i)) { widget.onRegionChanged(_region, i); setState(() => _channel = i); }
                }),
            ]) : const SizedBox.shrink()),
        ],
      )),

      _PageCard(title: l.tr('settings_modem'), subtitle: l.tr('settings_modem_hint'), child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          _ModemSection(modemPreset: _modemPreset, sf: _sf, bw: _bw, cr: _cr,
            enabled: conn && !_modemApplying,
            onPreset: (p) async {
              if (!mounted || _modemApplying) return;
              setState(() => _modemApplying = true);
              var sent = false;
              for (var i = 0; i < 2 && !sent; i++) { sent = await widget.ble.setModemPreset(p); if (!sent) await Future<void>.delayed(const Duration(milliseconds: 120)); }
              if (sent) {
                final ok = await _waitModem(preset: p);
                if (mounted) setState(() { _modemSt = ok ? l.tr('saved') : l.tr('modem_apply_failed'); _modemErr = !ok; });
              } else { if (mounted) setState(() { _modemSt = l.tr('error'); _modemErr = true; }); }
              if (mounted) setState(() => _modemApplying = false);
            },
            onCustom: (sf, bw, cr) async {
              if (!mounted || _modemApplying) return;
              setState(() => _modemApplying = true);
              var sent = false;
              for (var i = 0; i < 2 && !sent; i++) { sent = await widget.ble.setCustomModem(sf, bw, cr); if (!sent) await Future<void>.delayed(const Duration(milliseconds: 120)); }
              if (sent) {
                final ok = await _waitModem(preset: 4, sf: sf, bw: bw, cr: cr);
                if (ok) { setState(() { _sf = sf; _bw = bw; _cr = cr; _modemPreset = 4; }); widget.onSfChanged(sf); }
                if (mounted) setState(() { _modemSt = ok ? l.tr('saved') : l.tr('modem_apply_failed'); _modemErr = !ok; });
              } else { if (mounted) setState(() { _modemSt = l.tr('error'); _modemErr = true; }); }
              if (mounted) setState(() => _modemApplying = false);
            }),
          if (_modemApplying) const Padding(padding: EdgeInsets.only(top: AppSpacing.sm - 2), child: LinearProgressIndicator(minHeight: 2)),
          _AnimatedStatusChip(label: _modemSt, isError: _modemErr),
        ],
      )),

      if (_radioMode == 'wifi')
        _PageCard(title: l.tr('espnow_section'), subtitle: l.tr('espnow_section_hint'), child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            DropdownButtonFormField<int>(value: _espCh, isDense: true,
              decoration: InputDecoration(labelText: l.tr('espnow_channel')),
              items: List.generate(13, (i) => DropdownMenuItem(value: i + 1, child: Text('${i + 1}'))),
              onChanged: conn ? (v) async {
                if (v == null) return;
                if (await widget.ble.setEspNowChannel(v)) { setState(() => _espCh = v); widget.ble.getInfo(); }
              } : null),
            const SizedBox(height: AppSpacing.sm),
            RiftSwitchTile(
              title: Text(l.tr('espnow_adaptive'), style: TextStyle(color: pal.onSurface)),
              value: _espAdapt,
              onChanged: conn ? (v) async {
                if (await widget.ble.setEspNowAdaptive(v)) { setState(() => _espAdapt = v); widget.ble.getInfo(); }
              } : null),
          ],
        )),
    ]);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  PAGE 3 — Соединение (Radio + Wi-Fi + BLE PIN + OTA)
// ═══════════════════════════════════════════════════════════════════════════

class _ConnectionPage extends StatefulWidget {
  final RiftLinkBle ble;
  const _ConnectionPage({required this.ble});
  @override
  State<_ConnectionPage> createState() => _ConnectionPageState();
}

class _ConnectionPageState extends State<_ConnectionPage> {
  int? _blePin;
  String _radioMode = 'ble';
  String? _radioVariant;
  bool _wifiConn = false;
  String? _wifiSsid, _wifiIp;
  bool _radioApplying = false;
  late final TextEditingController _ssidCtrl, _passCtrl;
  String? _pinSt, _radioSt;
  bool _pinErr = false, _radioErr = false;
  StreamSubscription<RiftLinkEvent>? _sub;

  @override
  void initState() {
    super.initState();
    _ssidCtrl = TextEditingController();
    _passCtrl = TextEditingController();
    final li = widget.ble.lastInfo;
    _blePin = li?.blePin; _radioMode = li?.radioMode ?? 'ble'; _radioVariant = li?.radioVariant;
    _wifiConn = li?.wifiConnected ?? false; _wifiSsid = li?.wifiSsid; _wifiIp = li?.wifiIp;
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      if (evt is RiftLinkInfoEvent) {
        setState(() {
          _blePin = evt.blePin; _radioMode = evt.radioMode; _radioVariant = evt.radioVariant;
          _wifiConn = evt.wifiConnected; _wifiSsid = evt.wifiSsid; _wifiIp = evt.wifiIp;
        });
      }
    });
    WidgetsBinding.instance.addPostFrameCallback((_) { if (mounted && widget.ble.isConnected) widget.ble.getInfo(); });
  }

  @override
  void dispose() { _sub?.cancel(); _ssidCtrl.dispose(); _passCtrl.dispose(); super.dispose(); }

  Future<void> _toBle() async {
    final l = context.l10n;
    if (!widget.ble.isConnected || _radioApplying) return;
    setState(() => _radioApplying = true);
    final ok = await widget.ble.switchToBle();
    if (ok) {
      await widget.ble.getInfo(force: true);
      await Future<void>.delayed(const Duration(milliseconds: 350));
      final li = widget.ble.lastInfo;
      if (mounted) {
        setState(() {
          _radioSt = (li != null && li.radioMode == 'ble') ? l.tr('radio_mode_switched', {'mode': l.tr('radio_mode_ble')}) : l.tr('radio_mode_failed');
          _radioErr = !(li != null && li.radioMode == 'ble');
        });
      }
    } else { if (mounted) setState(() { _radioSt = l.tr('radio_mode_failed'); _radioErr = true; }); }
    if (mounted) setState(() => _radioApplying = false);
  }

  Future<void> _toWifi() async {
    final l = context.l10n;
    if (!widget.ble.isConnected || _radioApplying) return;
    final ssid = _ssidCtrl.text.trim();
    if (ssid.isEmpty) { showAppSnackBar(context, l.tr('radio_mode_need_ssid')); return; }
    setState(() => _radioApplying = true);
    final ok = await widget.ble.switchToWifiSta(ssid: ssid, pass: _passCtrl.text);
    if (ok) {
      if (!widget.ble.isConnected) {
        if (mounted) { showAppSnackBar(context, l.tr('radio_mode_switching_reconnect')); setState(() => _radioApplying = false); }
        return;
      }
      await widget.ble.getInfo(force: true);
      await Future<void>.delayed(const Duration(milliseconds: 350));
      final li = widget.ble.lastInfo;
      if (mounted) {
        final success = li != null && li.radioMode == 'wifi' && li.radioVariant == 'sta';
        setState(() {
          _radioSt = success ? l.tr('radio_mode_switched', {'mode': l.tr('radio_mode_wifi_sta')}) : l.tr('radio_mode_failed');
          _radioErr = !success;
        });
      }
    } else { if (mounted) setState(() { _radioSt = l.tr('radio_mode_failed'); _radioErr = true; }); }
    if (mounted) setState(() => _radioApplying = false);
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final pal = context.palette;
    final conn = widget.ble.isConnected;
    final usingWifi = widget.ble.isWifiMode;

    return _SubpageScaffold(title: l.tr('connection'), children: [
      // BLE PIN
      _PageCard(title: l.tr('ble_pin'), subtitle: l.tr('settings_connection_hint'), child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Container(
            padding: const EdgeInsets.symmetric(horizontal: AppSpacing.md, vertical: AppSpacing.sm + 2),
            decoration: BoxDecoration(color: pal.surfaceVariant, borderRadius: BorderRadius.circular(AppRadius.md), border: Border.all(color: pal.divider)),
            child: Text(_blePin != null ? _blePin.toString() : '—', style: TextStyle(fontFamily: 'monospace', color: pal.onSurface, fontWeight: FontWeight.w600, fontFeatures: const [FontFeature.tabularFigures()])),
          ),
          const SizedBox(height: AppSpacing.sm + 2),
          FilledButton.tonalIcon(
            onPressed: conn ? () async {
              final ok = await widget.ble.regeneratePin();
              if (ok) await widget.ble.getInfo();
              HapticFeedback.lightImpact();
              if (mounted) setState(() { _pinSt = ok ? l.tr('saved') : l.tr('error'); _pinErr = !ok; });
            } : null,
            icon: const Icon(Icons.lock_reset_rounded, size: 18), label: Text(l.tr('regen_pin'))),
          _AnimatedStatusChip(label: _pinSt, isError: _pinErr),
        ],
      )),

      // Radio mode
      _PageCard(title: l.tr('radio_mode_title'), subtitle: l.tr('radio_mode_hint'), child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(children: [
            Icon(_radioMode == 'wifi' ? Icons.wifi_rounded : Icons.bluetooth_rounded, size: 18, color: pal.primary),
            const SizedBox(width: AppSpacing.sm),
            Text('${l.tr('radio_mode_current')}: ${_radioMode == 'wifi' ? l.tr('radio_mode_wifi_sta') : l.tr('radio_mode_ble')}',
              style: TextStyle(color: pal.onSurface, fontWeight: FontWeight.w600)),
          ]),
          const SizedBox(height: AppSpacing.md),
          _SegmentedPickBar(leadingIcon: Icons.swap_horiz_rounded,
            labels: [l.tr('radio_mode_ble'), l.tr('radio_mode_wifi_sta')],
            selectedIndex: _radioMode == 'wifi' ? 1 : 0, enabled: conn && !_radioApplying,
            onSelected: (i) async { if (i == 0) await _toBle(); else await _toWifi(); }),
          _AnimatedStatusChip(label: _radioSt, isError: _radioErr),
          AnimatedSize(duration: _kDur, curve: _kIn,
            child: _radioMode == 'wifi' ? Padding(padding: const EdgeInsets.only(top: AppSpacing.sm + 2), child: Container(
              padding: const EdgeInsets.all(AppSpacing.sm + 2),
              decoration: BoxDecoration(color: pal.surfaceVariant, borderRadius: BorderRadius.circular(AppRadius.md), border: Border.all(color: pal.divider)),
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text(_wifiConn ? l.tr('radio_mode_wifi_connected') : l.tr('radio_mode_wifi_not_connected'),
                  style: TextStyle(color: _wifiConn ? pal.success : pal.onSurfaceVariant, fontWeight: FontWeight.w600)),
                if (_wifiSsid != null && _wifiSsid!.isNotEmpty) ...[const SizedBox(height: AppSpacing.xs), Text('SSID: $_wifiSsid', style: TextStyle(color: pal.onSurfaceVariant, fontSize: 12))],
                if (_wifiIp != null && _wifiIp!.isNotEmpty) ...[const SizedBox(height: AppSpacing.xs / 2), Text('IP: $_wifiIp', style: TextStyle(color: pal.onSurfaceVariant, fontSize: 12))],
              ]),
            )) : const SizedBox.shrink()),
          const SizedBox(height: AppSpacing.md),
          TextField(controller: _ssidCtrl, style: TextStyle(color: pal.onSurface), decoration: InputDecoration(labelText: l.tr('wifi_ssid'))),
          const SizedBox(height: AppSpacing.sm + 2),
          TextField(controller: _passCtrl, obscureText: true, style: TextStyle(color: pal.onSurface), decoration: InputDecoration(labelText: l.tr('wifi_password'))),
          const SizedBox(height: AppSpacing.md),
          FilledButton.icon(onPressed: conn && !_radioApplying ? _toWifi : null,
            icon: const Icon(Icons.wifi_find_rounded), label: Text(l.tr('radio_mode_connect_sta'))),
          if (_radioApplying) const Padding(padding: EdgeInsets.only(top: AppSpacing.sm + 2), child: LinearProgressIndicator(minHeight: 2)),
        ],
      )),

      // OTA
      _PageCard(title: l.tr('firmware_update_title'), subtitle: l.tr('firmware_update_hint'), child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (!usingWifi) ...[
            FilledButton.icon(onPressed: conn ? () => showOtaDialog(context, widget.ble) : null,
              icon: const Icon(Icons.bluetooth_searching_rounded), label: Text(l.tr('firmware_update_ble'))),
            const SizedBox(height: AppSpacing.sm + 2),
            Text(l.tr('firmware_update_where_hint'), style: TextStyle(fontSize: 12, color: pal.onSurfaceVariant)),
            const SizedBox(height: AppSpacing.sm - 2),
            Text(l.tr('firmware_update_path'), style: TextStyle(fontFamily: 'monospace', color: pal.onSurfaceVariant.withOpacity(0.95), fontSize: 12)),
          ] else ...[
            Text(l.tr('firmware_update_wifi'), style: TextStyle(fontSize: 13, fontWeight: FontWeight.w600, color: pal.onSurface)),
            const SizedBox(height: AppSpacing.sm),
            Text(l.tr('firmware_update_where_hint'), style: TextStyle(fontSize: 12, color: pal.onSurfaceVariant)),
          ],
          if (!usingWifi && _radioMode != 'wifi') ...[
            const SizedBox(height: AppSpacing.md),
            Row(children: [
              Icon(Icons.info_outline_rounded, size: 16, color: pal.onSurfaceVariant), const SizedBox(width: AppSpacing.sm - 2),
              Expanded(child: Text(l.tr('firmware_update_wifi_requires_wifi_mode'), style: TextStyle(fontSize: 12, color: pal.onSurfaceVariant))),
            ]),
          ],
          if (usingWifi || _radioMode == 'wifi') ...[
            const SizedBox(height: AppSpacing.md),
            Row(children: [
              Icon(Icons.check_circle_outline_rounded, size: 16, color: pal.success), const SizedBox(width: AppSpacing.sm - 2),
              Expanded(child: Text(l.tr('firmware_update_wifi_mode_ready'), style: TextStyle(fontSize: 12, color: pal.onSurfaceVariant))),
            ]),
          ],
          const SizedBox(height: AppSpacing.md),
          AppSecondaryButton(fullWidth: true, onPressed: conn && !_radioApplying && _radioMode != 'ble' ? _toBle : null,
            child: Row(mainAxisAlignment: MainAxisAlignment.center, mainAxisSize: MainAxisSize.min, children: [
              const Icon(Icons.bluetooth_rounded), SizedBox(width: AppSpacing.sm), Text(l.tr('radio_mode_back_to_ble')),
            ])),
        ],
      )),
    ]);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  PAGE 4 — Безопасность (E2E Invite)
// ═══════════════════════════════════════════════════════════════════════════

class _SecurityPage extends StatefulWidget {
  final RiftLinkBle ble;
  const _SecurityPage({required this.ble});
  @override
  State<_SecurityPage> createState() => _SecurityPageState();
}

class _SecurityPageState extends State<_SecurityPage> {
  late final TextEditingController _idCtrl, _keyCtrl, _ckCtrl, _tokCtrl;
  String? _invSt;
  bool _invErr = false;
  String? _pendPeer;
  Timer? _expiryTimer;
  StreamSubscription<RiftLinkEvent>? _sub;

  @override
  void initState() {
    super.initState();
    _idCtrl = TextEditingController(); _keyCtrl = TextEditingController();
    _ckCtrl = TextEditingController(); _tokCtrl = TextEditingController();
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      if (evt is RiftLinkInfoEvent) {
        final pp = _pendPeer;
        if (pp != null && _peerHasKey(evt, pp) && mounted) {
          setState(() { _invErr = false; _invSt = context.l10n.tr('invite_status_key_ready'); _pendPeer = null; });
        }
      } else if (evt is RiftLinkInviteEvent) {
        final ttl = evt.inviteTtlMs != null ? (evt.inviteTtlMs! / 1000).round() : null;
        _armExpiry(evt.inviteTtlMs);
        setState(() { _invErr = false; _invSt = ttl != null ? context.l10n.tr('invite_status_created_ttl', {'sec': '$ttl'}) : context.l10n.tr('invite_created'); });
      } else if (evt is RiftLinkErrorEvent && evt.code.startsWith('invite_')) {
        String mapped = evt.msg;
        if (evt.code == 'invite_peer_key_mismatch') mapped = context.l10n.tr('invite_status_mismatch');
        else if (evt.code == 'invite_token_bad_length' || evt.code == 'invite_token_bad_format') mapped = context.l10n.tr('invite_status_token_bad');
        setState(() { _invErr = true; _invSt = mapped; });
      }
    });
    WidgetsBinding.instance.addPostFrameCallback((_) { if (mounted && widget.ble.isConnected) widget.ble.getInfo(); });
  }

  @override
  void dispose() { _sub?.cancel(); _expiryTimer?.cancel(); _idCtrl.dispose(); _keyCtrl.dispose(); _ckCtrl.dispose(); _tokCtrl.dispose(); super.dispose(); }

  void _armExpiry(int? ms) {
    _expiryTimer?.cancel();
    if (ms == null || ms <= 0) return;
    _expiryTimer = Timer(Duration(milliseconds: ms), () {
      if (!mounted) return;
      if (_pendPeer == null && !_invErr && _invSt == context.l10n.tr('invite_status_key_ready')) return;
      setState(() { _invErr = true; _invSt = context.l10n.tr('invite_status_expired'); });
    });
  }

  String? _valToken(String raw) {
    if (raw.isEmpty) return null;
    if (raw.length != 16 || !RegExp(r'^[0-9A-Fa-f]{16}$').hasMatch(raw)) return context.l10n.tr('invite_status_token_bad');
    return null;
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final pal = context.palette;
    final conn = widget.ble.isConnected;

    return _SubpageScaffold(title: l.tr('settings_security_title'), children: [
      _PageCard(title: l.tr('e2e_invite'), subtitle: l.tr('e2e_invite_hint'), child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          OutlinedButton.icon(
            style: OutlinedButton.styleFrom(foregroundColor: pal.primary, side: BorderSide(color: pal.primary, width: 1.2),
              padding: const EdgeInsets.symmetric(vertical: AppSpacing.buttonPrimaryV, horizontal: AppSpacing.lg),
              shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(AppRadius.md))),
            onPressed: conn ? () {
              HapticFeedback.lightImpact();
              setState(() { _invErr = false; _invSt = l.tr('invite_status_creating'); });
              widget.ble.createInvite();
            } : null,
            icon: const Icon(Icons.add_moderator_outlined, size: 22),
            label: Text(l.tr('create_invite'), style: const TextStyle(fontWeight: FontWeight.w600))),
          SizedBox(height: AppSpacing.xl + AppSpacing.xs),
          _SectionAccentHeader(text: l.tr('invite_accept_section')),
          const SizedBox(height: AppSpacing.md),
          Container(
            padding: const EdgeInsets.all(AppSpacing.md + AppSpacing.xs),
            decoration: BoxDecoration(color: pal.surfaceVariant.withOpacity(0.65), borderRadius: BorderRadius.circular(AppRadius.md), border: Border.all(color: pal.divider)),
            child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
              TextField(controller: _idCtrl, style: TextStyle(color: pal.onSurface, fontFamily: 'monospace', fontSize: 13),
                decoration: InputDecoration(isDense: true, labelText: l.tr('inviter_id'),
                  suffixIcon: IconButton(icon: Icon(Icons.content_paste_rounded, color: pal.primary), tooltip: l.tr('paste'),
                    onPressed: () async {
                      final data = await Clipboard.getData(Clipboard.kTextPlain);
                      final text = data?.text?.trim();
                      if (text == null || text.isEmpty) return;
                      try {
                        final m = jsonDecode(text) as Map<String, dynamic>?;
                        if (m != null) {
                          _idCtrl.text = ((m['id'] as String?) ?? '').replaceAll(RegExp(r'[^0-9A-Fa-f]'), '');
                          _keyCtrl.text = (m['pubKey'] as String?) ?? '';
                          _ckCtrl.text = (m['channelKey'] as String?) ?? '';
                          _tokCtrl.text = ((m['inviteToken'] as String?) ?? '').replaceAll(RegExp(r'[^0-9A-Fa-f]'), '');
                          final ttl = (m['inviteTtlMs'] as num?)?.toInt();
                          if (ttl != null && mounted) showAppSnackBar(context, l.tr('invite_ttl', {'sec': '${(ttl / 1000).round()}'}));
                          if (mounted) setState(() {});
                        }
                      } catch (_) {}
                    })),
                maxLength: 16),
              const SizedBox(height: AppSpacing.md),
              TextField(controller: _keyCtrl, style: TextStyle(color: pal.onSurface, fontSize: 13, height: 1.35),
                decoration: InputDecoration(isDense: true, labelText: l.tr('invite_pubkey'), alignLabelWithHint: true), maxLines: 3, minLines: 2),
              const SizedBox(height: AppSpacing.md),
              TextField(controller: _ckCtrl, style: TextStyle(color: pal.onSurface, fontSize: 13, height: 1.35),
                decoration: InputDecoration(isDense: true, labelText: l.tr('invite_channel_key'), alignLabelWithHint: true), maxLines: 2, minLines: 1),
              const SizedBox(height: AppSpacing.md),
              TextField(controller: _tokCtrl, style: TextStyle(color: pal.onSurface, fontFamily: 'monospace', fontSize: 13),
                decoration: InputDecoration(isDense: true, labelText: l.tr('invite_token_optional')), maxLength: 16),
            ]),
          ),
          const SizedBox(height: AppSpacing.md + AppSpacing.xs),
          FilledButton.icon(
            style: FilledButton.styleFrom(padding: const EdgeInsets.symmetric(vertical: AppSpacing.buttonPrimaryV, horizontal: AppSpacing.buttonPrimaryH - 2),
              shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(AppRadius.md))),
            onPressed: conn ? () async {
              HapticFeedback.lightImpact();
              final id = _idCtrl.text.trim().replaceAll(RegExp(r'[^0-9A-Fa-f]'), '');
              final key = _keyCtrl.text.trim();
              final ck = _ckCtrl.text.trim();
              final token = _tokCtrl.text.trim().replaceAll(RegExp(r'[^0-9A-Fa-f]'), '');
              if (id.length != 16 || key.isEmpty) return;
              final tokErr = _valToken(token);
              if (tokErr != null) { setState(() { _invErr = true; _invSt = tokErr; }); showAppSnackBar(context, tokErr); return; }
              setState(() { _invErr = false; _invSt = l.tr('invite_status_handshake_pending'); });
              if (await widget.ble.acceptInvite(
                id: id, pubKey: key,
                channelKey: ck.isEmpty ? null : ck, inviteToken: token.isEmpty ? null : token)) {
                if (mounted) {
                  setState(() { _invErr = false; _invSt = l.tr('invite_status_accepted_wait_key'); _pendPeer = id; });
                  showAppSnackBar(context, l.tr('invite_accepted'));
                }
              }
            } : null,
            icon: const Icon(Icons.check_circle_outline_rounded, size: 22),
            label: Text(l.tr('accept_invite'), style: const TextStyle(fontWeight: FontWeight.w600))),
          if (_invSt != null && _invSt!.isNotEmpty) ...[
            const SizedBox(height: AppSpacing.sm + 2),
            AppStateChip(label: _invSt!, kind: _invErr ? AppStateKind.error : AppStateKind.success),
          ],
        ],
      )),
    ]);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  PAGE 5 — Энергия и тема (Powersave + GPS + Theme + MeshAnim)
// ═══════════════════════════════════════════════════════════════════════════

class _EnergyThemePage extends StatefulWidget {
  final RiftLinkBle ble;
  final bool powersave, gpsPresent, gpsEnabled, gpsFix, meshAnimationEnabled;
  final void Function(bool) onPowersaveChanged, onGpsChanged, onMeshAnimationChanged;

  const _EnergyThemePage({required this.ble, required this.powersave,
    required this.gpsPresent, required this.gpsEnabled, required this.gpsFix,
    required this.meshAnimationEnabled,
    required this.onPowersaveChanged, required this.onGpsChanged, required this.onMeshAnimationChanged});

  @override
  State<_EnergyThemePage> createState() => _EnergyThemePageState();
}

class _EnergyThemePageState extends State<_EnergyThemePage> {
  late bool _ps, _gpsPresent, _gpsOn, _gpsFix, _meshAnim;
  StreamSubscription<RiftLinkEvent>? _sub;

  @override
  void initState() {
    super.initState();
    final li = widget.ble.lastInfo;
    _ps = li?.powersave ?? widget.powersave;
    _gpsPresent = li?.gpsPresent ?? widget.gpsPresent;
    _gpsOn = li?.gpsEnabled ?? widget.gpsEnabled;
    _gpsFix = li?.gpsFix ?? widget.gpsFix;
    _meshAnim = widget.meshAnimationEnabled;
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      if (evt is RiftLinkInfoEvent) setState(() { _ps = evt.powersave; _gpsPresent = evt.gpsPresent; _gpsOn = evt.gpsEnabled; _gpsFix = evt.gpsFix; });
      else if (evt is RiftLinkGpsEvent) setState(() { _gpsPresent = evt.present; _gpsOn = evt.enabled; _gpsFix = evt.hasFix; });
    });
    WidgetsBinding.instance.addPostFrameCallback((_) { if (mounted && widget.ble.isConnected) widget.ble.getInfo(); });
  }

  @override
  void dispose() { _sub?.cancel(); super.dispose(); }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final pal = context.palette;
    final conn = widget.ble.isConnected;

    return _SubpageScaffold(title: l.tr('settings_energy_title'), children: [
      _PageCard(title: l.tr('settings_energy_title'), subtitle: l.tr('settings_energy_hint'), child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          _SectionAccentHeader(text: l.tr('settings_energy_node'), trailingIcon: Icons.memory_rounded),
          const SizedBox(height: AppSpacing.sm + 2),
          _SegmentedPickBar(leadingIcon: Icons.battery_saver_outlined,
            labels: [l.tr('powersave_mode_normal'), l.tr('powersave_mode_eco')],
            selectedIndex: _ps ? 1 : 0, enabled: conn,
            onSelected: (i) async {
              final eco = i == 1;
              if (eco == _ps) return;
              if (await widget.ble.setPowersave(eco)) { widget.onPowersaveChanged(eco); setState(() => _ps = eco); }
            }),
          const SizedBox(height: AppSpacing.xl),
          _SectionAccentHeader(text: l.tr('settings_energy_app'), trailingIcon: Icons.smartphone_rounded),
          const SizedBox(height: AppSpacing.sm - 2),
          ListTile(
            contentPadding: EdgeInsets.zero,
            leading: Icon(Icons.dark_mode_outlined, color: pal.primary),
            title: Text(l.tr('theme'), style: TextStyle(color: pal.onSurface, fontWeight: FontWeight.w500)),
            subtitle: Text(l.tr('theme_hint'), style: TextStyle(color: pal.onSurfaceVariant, fontSize: 12, height: 1.35)),
            trailing: Icon(Icons.chevron_right, color: pal.onSurfaceVariant),
            onTap: () { HapticFeedback.selectionClick(); showThemeModeSheet(context); }),
          const SizedBox(height: AppSpacing.xs),
          RiftSwitchTile(
            title: Text(l.tr('mesh_animation'), style: TextStyle(color: pal.onSurface, fontWeight: FontWeight.w500)),
            subtitle: Text(l.tr('mesh_animation_hint'), style: TextStyle(color: pal.onSurfaceVariant, fontSize: 12, height: 1.35)),
            leading: Icon(Icons.animation_rounded, color: pal.primary),
            value: _meshAnim,
            onChanged: (v) { HapticFeedback.selectionClick(); widget.onMeshAnimationChanged(v); setState(() => _meshAnim = v); }),
          const SizedBox(height: AppSpacing.lg),
          _SectionAccentHeader(text: l.tr('lang'), trailingIcon: Icons.language),
          const SizedBox(height: AppSpacing.sm + 2),
          _SegmentedPickBar(
            leadingIcon: Icons.language,
            labels: [l.tr('lang_ru'), l.tr('lang_en')],
            selectedIndex: AppLocalizations.currentLocale.languageCode == 'ru' ? 0 : 1,
            onSelected: (i) async {
              final target = i == 0 ? 'ru' : 'en';
              if (AppLocalizations.currentLocale.languageCode != target) {
                await AppLocalizations.switchLocale(() {
                  localeNotifier.value = AppLocalizations.currentLocale;
                });
                if (mounted) setState(() {});
              }
            },
          ),
        ],
      )),

      if (_gpsPresent)
        _PageCard(title: l.tr('gps_section'), child: RiftSwitchTile(
          title: Text(l.tr('gps_enable'), style: TextStyle(color: pal.onSurface)),
          subtitle: Text(_gpsFix ? l.tr('gps_fix_yes') : l.tr('gps_fix_no'), style: TextStyle(color: pal.onSurfaceVariant, fontSize: 13)),
          value: _gpsOn,
          onChanged: conn ? (v) async {
            if (await widget.ble.setGps(v)) { widget.onGpsChanged(v); setState(() => _gpsOn = v); }
          } : null)),
    ]);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  PAGE 6 — Диагностика (Info + Voice + Shutdown)
// ═══════════════════════════════════════════════════════════════════════════

class _DiagnosticsPage extends StatefulWidget {
  final RiftLinkBle ble;
  const _DiagnosticsPage({required this.ble});
  @override
  State<_DiagnosticsPage> createState() => _DiagnosticsPageState();
}

class _DiagnosticsPageState extends State<_DiagnosticsPage> {
  int? _offPend, _offCourier, _offDirect, _batMv, _batPct;
  bool _charging = false;
  bool _selftestRunning = false;
  bool? _radioOk, _displayOk, _antennaOk;
  int? _heapFree;
  String? _version;
  late final TextEditingController _avgCtrl, _hardCtrl, _minCtrl;
  bool _saving = false;
  String? _voiceSt;
  bool _voiceErr = false;
  StreamSubscription<RiftLinkEvent>? _sub;

  @override
  void initState() {
    super.initState();
    _avgCtrl = TextEditingController(); _hardCtrl = TextEditingController(); _minCtrl = TextEditingController();
    final li = widget.ble.lastInfo;
    _offPend = li?.offlinePending; _offCourier = li?.offlineCourierPending; _offDirect = li?.offlineDirectPending;
    _batMv = li?.batteryMv; _batPct = li?.batteryPercent; _charging = li?.charging ?? false;
    _version = li?.version;
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      if (evt is RiftLinkInfoEvent) {
        setState(() {
          if (evt.hasOfflinePendingField) _offPend = evt.offlinePending;
          if (evt.hasOfflineCourierPendingField) _offCourier = evt.offlineCourierPending;
          if (evt.hasOfflineDirectPendingField) _offDirect = evt.offlineDirectPending;
          if (evt.batteryMv != null && evt.batteryMv! > 0) _batMv = evt.batteryMv;
          _batPct = evt.batteryPercent; _charging = evt.charging; _version = evt.version;
        });
      } else if (evt is RiftLinkSelftestEvent) {
        setState(() {
          _selftestRunning = false;
          _radioOk = evt.radioOk;
          _displayOk = evt.displayOk;
          _antennaOk = evt.antennaOk;
          _heapFree = evt.heapFree;
          if (evt.batteryMv > 0) _batMv = evt.batteryMv;
          _batPct = evt.batteryPercent;
          _charging = evt.charging;
        });
      }
    });
    _loadVoicePrefs();
    WidgetsBinding.instance.addPostFrameCallback((_) { if (mounted && widget.ble.isConnected) widget.ble.getInfo(); });
  }

  @override
  void dispose() { _sub?.cancel(); _avgCtrl.dispose(); _hardCtrl.dispose(); _minCtrl.dispose(); super.dispose(); }

  Future<void> _loadVoicePrefs() async {
    final avg = await MeshPrefs.getVoiceAcceptMaxAvgLossPercent();
    final hard = await MeshPrefs.getVoiceAcceptMaxHardLossPercent();
    final min = await MeshPrefs.getVoiceAcceptMinSessions();
    if (!mounted) return;
    setState(() { _avgCtrl.text = '$avg'; _hardCtrl.text = '$hard'; _minCtrl.text = '$min'; });
  }

  Future<void> _saveVoicePrefs() async {
    final l = context.l10n;
    final avg = int.tryParse(_avgCtrl.text.trim());
    final hard = int.tryParse(_hardCtrl.text.trim());
    final min = int.tryParse(_minCtrl.text.trim());
    if (avg == null || hard == null || min == null || avg < 0 || avg > 100 || hard < 0 || hard > 100 || min < 1 || min > 500) {
      showAppSnackBar(context, l.tr('voice_acceptance_bad_values'));
      return;
    }
    setState(() => _saving = true);
    await MeshPrefs.setVoiceAcceptMaxAvgLossPercent(avg);
    await MeshPrefs.setVoiceAcceptMaxHardLossPercent(hard);
    await MeshPrefs.setVoiceAcceptMinSessions(min);
    if (!mounted) return;
    setState(() { _saving = false; _voiceSt = l.tr('saved'); _voiceErr = false; });
  }

  Widget _selftestRow(String label, bool ok) {
    final pal = context.palette;
    return ListTile(
      contentPadding: EdgeInsets.zero, dense: true,
      leading: Icon(ok ? Icons.check_circle_outline : Icons.error_outline, color: ok ? pal.success : pal.error, size: 20),
      title: Text(label, style: TextStyle(color: pal.onSurface, fontSize: 13)),
    );
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final pal = context.palette;

    return _SubpageScaffold(title: l.tr('settings_diagnostics_title'), children: [
      _PageCard(title: l.tr('settings_diagnostics_title'), subtitle: l.tr('settings_diagnostics_hint'), child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (_version != null && _version!.isNotEmpty)
            ListTile(contentPadding: EdgeInsets.zero,
              leading: Icon(Icons.info_outline, color: pal.onSurfaceVariant),
              title: Text('${l.tr('settings_firmware_version')}: $_version', style: TextStyle(color: pal.onSurface))),
          if (_batMv != null && _batMv! > 0)
            ListTile(contentPadding: EdgeInsets.zero,
              leading: Icon(_charging ? Icons.battery_charging_full : Icons.battery_std,
                color: _batPct != null && _batPct! <= 15 ? pal.error : pal.onSurfaceVariant),
              title: Text(_batPct != null
                ? '$_batPct% (${(_batMv! / 1000).toStringAsFixed(2)} V)${_charging ? ' ⚡' : ''}'
                : '${(_batMv! / 1000).toStringAsFixed(2)} V${_charging ? ' ⚡' : ''}',
                style: TextStyle(color: pal.onSurface))),
          if (_offPend != null && _offPend! > 0)
            ListTile(contentPadding: EdgeInsets.zero, leading: Icon(Icons.schedule, color: pal.onSurfaceVariant),
              title: Text('${l.tr('offline_pending')}: $_offPend', style: TextStyle(color: pal.onSurface))),
          if (_offCourier != null && _offCourier! > 0)
            ListTile(contentPadding: EdgeInsets.zero, leading: Icon(Icons.local_shipping_outlined, color: pal.onSurfaceVariant),
              title: Text(l.tr('scf_courier_status_count', {'n': '$_offCourier'}), style: TextStyle(color: pal.onSurface))),
          if (_offDirect != null && _offDirect! > 0)
            ListTile(contentPadding: EdgeInsets.zero, leading: Icon(Icons.mark_email_unread_outlined, color: pal.onSurfaceVariant),
              title: Text(l.tr('offline_direct_status_count', {'n': '$_offDirect'}), style: TextStyle(color: pal.onSurface))),
        ],
      )),

      _PageCard(title: l.tr('selftest'), child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          FilledButton.tonalIcon(
            onPressed: _selftestRunning ? null : () {
              setState(() => _selftestRunning = true);
              widget.ble.selftest();
            },
            icon: _selftestRunning
              ? SizedBox(width: 14, height: 14, child: CircularProgressIndicator(strokeWidth: 2, color: pal.primary))
              : const Icon(Icons.health_and_safety_outlined, size: 18),
            label: Text(l.tr('selftest_run')),
          ),
          if (_radioOk != null) ...[
            const SizedBox(height: AppSpacing.sm),
            _selftestRow(l.tr('selftest_radio'), _radioOk!),
            _selftestRow(l.tr('selftest_antenna'), _antennaOk ?? true),
            _selftestRow(l.tr('selftest_display'), _displayOk!),
            if (_heapFree != null)
              ListTile(contentPadding: EdgeInsets.zero,
                leading: Icon(Icons.memory, color: pal.onSurfaceVariant),
                title: Text(l.tr('selftest_heap', {'kb': (_heapFree! / 1024).toStringAsFixed(1)}), style: TextStyle(color: pal.onSurface))),
          ],
        ],
      )),

      _PageCard(title: l.tr('voice_acceptance_title'), subtitle: l.tr('voice_acceptance_hint'), child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          TextField(controller: _avgCtrl, keyboardType: TextInputType.number,
            inputFormatters: [FilteringTextInputFormatter.digitsOnly],
            decoration: InputDecoration(isDense: true, labelText: l.tr('voice_acceptance_avg_loss'))),
          const SizedBox(height: AppSpacing.sm),
          TextField(controller: _hardCtrl, keyboardType: TextInputType.number,
            inputFormatters: [FilteringTextInputFormatter.digitsOnly],
            decoration: InputDecoration(isDense: true, labelText: l.tr('voice_acceptance_hard_loss'))),
          const SizedBox(height: AppSpacing.sm),
          TextField(controller: _minCtrl, keyboardType: TextInputType.number,
            inputFormatters: [FilteringTextInputFormatter.digitsOnly],
            decoration: InputDecoration(isDense: true, labelText: l.tr('voice_acceptance_min_sessions'))),
          const SizedBox(height: AppSpacing.sm + 2),
          FilledButton.tonalIcon(
            onPressed: _saving ? null : _saveVoicePrefs,
            icon: _saving
              ? SizedBox(width: 14, height: 14, child: CircularProgressIndicator(strokeWidth: 2, color: pal.primary))
              : const Icon(Icons.tune_rounded, size: 18),
            label: Text(l.tr('voice_acceptance_apply'))),
          _AnimatedStatusChip(label: _voiceSt, isError: _voiceErr),
        ],
      )),

      _PageCard(child: ListTile(
        contentPadding: EdgeInsets.zero,
        leading: Icon(Icons.power_settings_new, color: pal.error),
        title: Text(l.tr('shutdown_device'), style: TextStyle(color: pal.onSurface)),
        onTap: () async {
          final ok = await showDialog<bool>(context: context, builder: (ctx) => AlertDialog(
            title: Text(l.tr('shutdown_device')),
            content: Text(l.tr('shutdown_confirm')),
            actions: [
              TextButton(onPressed: () => Navigator.pop(ctx, false), child: Text(l.tr('cancel'))),
              TextButton(onPressed: () => Navigator.pop(ctx, true), child: Text(l.tr('shutdown_device'))),
            ],
          ));
          if (ok == true) widget.ble.shutdown();
        },
      )),
    ]);
  }
}

class _FaqPage extends StatelessWidget {
  const _FaqPage();

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final pal = context.palette;
    final items = <(String, String)>[
      (l.tr('faq_q_mesh'), l.tr('faq_a_mesh')),
      (l.tr('faq_q_sf'), l.tr('faq_a_sf')),
      (l.tr('faq_q_invite'), l.tr('faq_a_invite')),
      (l.tr('faq_q_ota'), l.tr('faq_a_ota')),
      (l.tr('faq_q_critical'), l.tr('faq_a_critical')),
      (l.tr('faq_q_sos'), l.tr('faq_a_sos')),
      (l.tr('faq_q_timecapsule'), l.tr('faq_a_timecapsule')),
      (l.tr('faq_q_ping'), l.tr('faq_a_ping')),
      (l.tr('faq_q_ble_wifi'), l.tr('faq_a_ble_wifi')),
      (l.tr('faq_q_e2e'), l.tr('faq_a_e2e')),
      (l.tr('faq_q_powersave'), l.tr('faq_a_powersave')),
      (l.tr('faq_q_gps'), l.tr('faq_a_gps')),
      (l.tr('faq_q_groups'), l.tr('faq_a_groups')),
      (l.tr('faq_q_offline'), l.tr('faq_a_offline')),
      (l.tr('faq_q_nickname'), l.tr('faq_a_nickname')),
      (l.tr('faq_q_lora_channel'), l.tr('faq_a_lora_channel')),
      (l.tr('faq_q_voice'), l.tr('faq_a_voice')),
      (l.tr('faq_q_selftest'), l.tr('faq_a_selftest')),
    ];
    return _SubpageScaffold(
      title: l.tr('faq_title'),
      children: items.map((qa) => _PageCard(
        child: ExpansionTile(
          tilePadding: EdgeInsets.zero,
          childrenPadding: const EdgeInsets.only(bottom: AppSpacing.sm),
          title: Text(qa.$1, style: TextStyle(color: pal.onSurface, fontWeight: FontWeight.w600, fontSize: 14)),
          children: [Text(qa.$2, style: TextStyle(color: pal.onSurfaceVariant, fontSize: 13, height: 1.4))],
        ),
      )).toList(),
    );
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Shared widgets — скопированы из settings_screen.dart для автономности
// ═══════════════════════════════════════════════════════════════════════════

// _SegmentedPickBar is now RiftSegmentedBar from app_primitives.dart
typedef _SegmentedPickBar = RiftSegmentedBar;

class _ModemSection extends StatefulWidget {
  final int? modemPreset, sf, cr;
  final double? bw;
  final bool enabled;
  final Future<void> Function(int preset) onPreset;
  final Future<void> Function(int sf, double bw, int cr) onCustom;
  const _ModemSection({this.modemPreset, this.sf, this.bw, this.cr, this.enabled = true, required this.onPreset, required this.onCustom});
  @override
  State<_ModemSection> createState() => _ModemSectionState();
}

class _ModemSectionState extends State<_ModemSection> {
  static const _descRu = ['SF7·BW250·CR5\nГород, скорость, малая дальность', 'SF7·BW125·CR5\nБаланс скорости и дальности', 'SF10·BW125·CR5\nХорошая дальность', 'SF12·BW125·CR8\nМакс. дальность, медленно', 'Ручная настройка SF / BW / CR'];
  static const _descEn = ['SF7·BW250·CR5\nCity use, high speed, short range', 'SF7·BW125·CR5\nBalanced speed and range', 'SF10·BW125·CR5\nGood long-range profile', 'SF12·BW125·CR8\nMaximum range, slower throughput', 'Manual SF / BW / CR configuration'];
  static const _bwOpts = [62.5, 125.0, 250.0, 500.0];

  late int _sel, _cSf, _cCr;
  late double _cBw;

  @override
  void initState() { super.initState(); _sync(); }

  @override
  void didUpdateWidget(_ModemSection o) {
    super.didUpdateWidget(o);
    if (o.modemPreset != widget.modemPreset || o.sf != widget.sf || o.bw != widget.bw || o.cr != widget.cr) _sync();
  }

  void _sync() {
    _sel = (widget.modemPreset != null && widget.modemPreset! >= 0 && widget.modemPreset! <= 4) ? widget.modemPreset! : 1;
    _cSf = widget.sf ?? 7; _cBw = widget.bw ?? 125.0; _cCr = widget.cr ?? 5;
  }

  @override
  Widget build(BuildContext context) {
    final pal = context.palette;
    final l = context.l10n;
    final en = widget.enabled;
    final isCustom = _sel == 4;
    final isRu = AppLocalizations.currentLocale.languageCode == 'ru';
    final desc = isRu ? _descRu : _descEn;
    final descStyle = AppTypography.chipBase().copyWith(color: pal.onSurfaceVariant, fontStyle: FontStyle.italic);
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      Wrap(spacing: AppSpacing.sm - 2, runSpacing: AppSpacing.sm - 2, children: [for (var i = 0; i < 5; i++) _chip(context, i, en)]),
      const SizedBox(height: AppSpacing.sm),
      AnimatedSize(duration: _kDur, curve: _kIn, child: Padding(padding: const EdgeInsets.symmetric(vertical: AppSpacing.xs),
        child: Text(desc[_sel], style: TextStyle(fontSize: 12, height: 1.4, color: pal.onSurfaceVariant.withOpacity(0.85))))),
      if (isCustom) ...[
        const SizedBox(height: AppSpacing.sm + 2),
        Text(l.tr('modem_sf_label'), style: TextStyle(fontSize: 12, fontWeight: FontWeight.w600, color: pal.onSurfaceVariant)),
        const SizedBox(height: AppSpacing.xs),
        _SegmentedPickBar(
          labels: const ['7', '8', '9', '10', '11', '12'],
          selectedIndex: _cSf >= 7 && _cSf <= 12 ? _cSf - 7 : null,
          enabled: en,
          onSelected: (i) => setState(() => _cSf = i + 7),
        ),
        const SizedBox(height: AppSpacing.xs),
        Text(l.tr('modem_sf_desc'), style: descStyle),
        const SizedBox(height: AppSpacing.sm + 2),
        Text(l.tr('modem_bw_label'), style: TextStyle(fontSize: 12, fontWeight: FontWeight.w600, color: pal.onSurfaceVariant)),
        const SizedBox(height: AppSpacing.xs),
        _SegmentedPickBar(
          labels: const ['62.5', '125', '250', '500'],
          selectedIndex: _bwOpts.indexOf(_cBw) >= 0 ? _bwOpts.indexOf(_cBw) : null,
          enabled: en,
          onSelected: (i) => setState(() => _cBw = _bwOpts[i]),
        ),
        const SizedBox(height: AppSpacing.xs),
        Text(l.tr('modem_bw_desc'), style: descStyle),
        const SizedBox(height: AppSpacing.sm + 2),
        Text(l.tr('modem_cr_label'), style: TextStyle(fontSize: 12, fontWeight: FontWeight.w600, color: pal.onSurfaceVariant)),
        const SizedBox(height: AppSpacing.xs),
        _SegmentedPickBar(
          labels: const ['4/5', '4/6', '4/7', '4/8'],
          selectedIndex: _cCr >= 5 && _cCr <= 8 ? _cCr - 5 : null,
          enabled: en,
          onSelected: (i) => setState(() => _cCr = i + 5),
        ),
        const SizedBox(height: AppSpacing.xs),
        Text(l.tr('modem_cr_desc'), style: descStyle),
        const SizedBox(height: AppSpacing.md),
        FilledButton.tonalIcon(onPressed: en ? () => widget.onCustom(_cSf, _cBw, _cCr) : null,
          icon: const Icon(Icons.tune_rounded, size: 18), label: Text(l.tr('modem_apply_custom'))),
      ],
    ]);
  }

  Widget _chip(BuildContext context, int idx, bool en) {
    final pal = context.palette;
    final l = context.l10n;
    final sel = _sel == idx;
    final label = switch (idx) { 0 => l.tr('modem_preset_speed'), 1 => l.tr('modem_preset_normal'), 2 => l.tr('modem_preset_range'), 3 => l.tr('modem_preset_maxrange'), _ => l.tr('modem_preset_custom') };
    return GestureDetector(
      onTap: en ? () { HapticFeedback.selectionClick(); setState(() => _sel = idx); if (idx < 4) widget.onPreset(idx); } : null,
      child: AnimatedContainer(duration: _kDur, constraints: const BoxConstraints(minWidth: 84),
        padding: const EdgeInsets.symmetric(horizontal: AppSpacing.md, vertical: AppSpacing.sm),
        decoration: BoxDecoration(borderRadius: BorderRadius.circular(AppRadius.md),
          color: sel ? pal.primary.withOpacity(0.15) : pal.surfaceVariant,
          border: Border.all(color: sel ? pal.primary : pal.divider, width: sel ? 1.6 : 1)),
        child: Text(label, textAlign: TextAlign.center,
          style: TextStyle(fontSize: 12.5, fontWeight: sel ? FontWeight.w700 : FontWeight.w500, color: sel ? pal.primary : pal.onSurfaceVariant.withOpacity(en ? 1.0 : 0.5)))));
  }
}
