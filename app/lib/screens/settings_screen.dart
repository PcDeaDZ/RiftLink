import 'dart:async';
import 'dart:convert';
import 'dart:ui' show FontFeature;
import 'dart:math' show min;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../theme/theme_notifier.dart';
import '../widgets/mesh_background.dart';
import '../widgets/app_snackbar.dart';

/// Форматирует hex ID для отображения (группы по 4 символа). Копирование — без пробелов.
String _formatNodeIdDisplay(String raw) {
  final c = raw.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();
  if (c.isEmpty) return raw.trim();
  final parts = <String>[];
  for (var i = 0; i < c.length; i += 4) {
    parts.add(c.substring(i, min(i + 4, c.length)));
  }
  return parts.join(' ');
}

String _nodeIdForClipboard(String raw) {
  return raw.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();
}

/// Сегментированный выбор со скользящей подсветкой (регион, канал, энергосбережение).
class _SegmentedPickBar extends StatelessWidget {
  final List<String> labels;
  final int? selectedIndex;
  final ValueChanged<int> onSelected;
  final bool enabled;
  final IconData? leadingIcon;

  const _SegmentedPickBar({
    required this.labels,
    required this.selectedIndex,
    required this.onSelected,
    this.enabled = true,
    this.leadingIcon,
  });

  static const _anim = Duration(milliseconds: 320);
  static const _curve = Curves.easeOutCubic;

  @override
  Widget build(BuildContext context) {
    final inactive = context.palette.onSurfaceVariant.withOpacity(enabled ? 1.0 : 0.45);
    final n = labels.length;
    return Material(
      color: Colors.transparent,
      child: Container(
        decoration: BoxDecoration(
          color: context.palette.surfaceVariant,
          borderRadius: BorderRadius.circular(14),
          border: Border.all(color: context.palette.divider),
        ),
        padding: const EdgeInsets.all(4),
        child: Row(
          children: [
            if (leadingIcon != null) ...[
              Padding(
                padding: const EdgeInsets.only(left: 8, right: 4),
                child: AnimatedSwitcher(
                  duration: const Duration(milliseconds: 260),
                  switchInCurve: Curves.easeOut,
                  switchOutCurve: Curves.easeIn,
                  transitionBuilder: (child, anim) => ScaleTransition(scale: anim, child: FadeTransition(opacity: anim, child: child)),
                  child: Icon(
                    leadingIcon,
                    key: ValueKey(leadingIcon),
                    size: 20,
                    color: context.palette.primary.withOpacity(enabled ? 1 : 0.4),
                  ),
                ),
              ),
            ],
            Expanded(
              child: n == 0
                  ? const SizedBox.shrink()
                  : LayoutBuilder(
                      builder: (context, constraints) {
                        final segW = constraints.maxWidth / n;
                        final idx = selectedIndex;
                        final showPill = idx != null && idx >= 0 && idx < n;
                        final left = showPill ? segW * idx : 0.0;
                        return ClipRRect(
                          borderRadius: BorderRadius.circular(10),
                          child: Stack(
                            clipBehavior: Clip.none,
                            children: [
                              AnimatedPositioned(
                                duration: _anim,
                                curve: _curve,
                                left: left,
                                width: showPill ? segW : 0,
                                top: 0,
                                bottom: 0,
                                child: AnimatedOpacity(
                                  duration: const Duration(milliseconds: 200),
                                  opacity: showPill ? 1 : 0,
                                  child: DecoratedBox(
                                    decoration: BoxDecoration(
                                      borderRadius: BorderRadius.circular(10),
                                      color: context.palette.primary.withOpacity(enabled ? 0.22 : 0.12),
                                      border: Border.all(
                                        color: context.palette.primary.withOpacity(enabled ? 1 : 0.45),
                                        width: 1.5,
                                      ),
                                      boxShadow: [
                                        BoxShadow(
                                          color: context.palette.primary.withOpacity(0.2),
                                          blurRadius: 12,
                                          offset: const Offset(0, 2),
                                        ),
                                      ],
                                    ),
                                  ),
                                ),
                              ),
                              Row(
                                children: List.generate(n, (i) {
                                  final sel = idx == i;
                                  return Expanded(
                                    child: Material(
                                      color: Colors.transparent,
                                      child: InkWell(
                                        borderRadius: BorderRadius.circular(10),
                                        onTap: enabled
                                            ? () {
                                                HapticFeedback.selectionClick();
                                                onSelected(i);
                                              }
                                            : null,
                                        child: Padding(
                                          padding: const EdgeInsets.symmetric(vertical: 12, horizontal: 2),
                                          child: Center(
                                            child: AnimatedDefaultTextStyle(
                                              duration: _anim,
                                              curve: _curve,
                                              style: TextStyle(
                                                fontSize: 13,
                                                letterSpacing: 0.3,
                                                fontWeight: sel ? FontWeight.w800 : FontWeight.w500,
                                                color: sel ? context.palette.primary : inactive,
                                              ),
                                              child: FittedBox(
                                                fit: BoxFit.scaleDown,
                                                child: Text(labels[i], maxLines: 1),
                                              ),
                                            ),
                                          ),
                                        ),
                                      ),
                                    ),
                                  );
                                }),
                              ),
                            ],
                          ),
                        );
                      },
                    ),
            ),
          ],
        ),
      ),
    );
  }
}

/// Сетка выбора SF (карточки с мягким свечением у выбранного).
class _SfPickGrid extends StatelessWidget {
  final int? selectedSf;
  final ValueChanged<int> onSelectSf;
  final bool enabled;

  const _SfPickGrid({
    required this.selectedSf,
    required this.onSelectSf,
    this.enabled = true,
  });

  static const _sfs = [7, 8, 9, 10, 11, 12];

  @override
  Widget build(BuildContext context) {
    final inactive = context.palette.onSurfaceVariant.withOpacity(enabled ? 1.0 : 0.45);
    return LayoutBuilder(
      builder: (context, c) {
        final w = c.maxWidth;
        final gap = 8.0;
        final n = _sfs.length;
        final cell = (w - gap * (n - 1)) / n;
        final minCell = 44.0;
        final useScroll = cell < minCell;
        if (useScroll) {
          return SizedBox(
            height: 52,
            child: ListView.separated(
              scrollDirection: Axis.horizontal,
              physics: const BouncingScrollPhysics(),
              itemCount: n,
              separatorBuilder: (_, __) => SizedBox(width: gap),
              itemBuilder: (context, i) {
                final sf = _sfs[i];
                return _SfCell(
                  sf: sf,
                  selected: selectedSf == sf,
                  enabled: enabled,
                  inactiveColor: inactive,
                  minWidth: minCell,
                  onTap: enabled ? () => onSelectSf(sf) : null,
                );
              },
            ),
          );
        }
        return Row(
          children: [
            for (var i = 0; i < n; i++) ...[
              if (i > 0) SizedBox(width: gap),
              Expanded(
                child: _SfCell(
                  sf: _sfs[i],
                  selected: selectedSf == _sfs[i],
                  enabled: enabled,
                  inactiveColor: inactive,
                  minWidth: 0,
                  onTap: enabled ? () => onSelectSf(_sfs[i]) : null,
                ),
              ),
            ],
          ],
        );
      },
    );
  }
}

class _SfCell extends StatelessWidget {
  final int sf;
  final bool selected;
  final bool enabled;
  final Color inactiveColor;
  final double minWidth;
  final VoidCallback? onTap;

  const _SfCell({
    required this.sf,
    required this.selected,
    required this.enabled,
    required this.inactiveColor,
    required this.minWidth,
    this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    return AnimatedScale(
      scale: selected ? 1.06 : 1.0,
      duration: const Duration(milliseconds: 280),
      curve: Curves.easeOutCubic,
      alignment: Alignment.center,
      child: Material(
        color: Colors.transparent,
        borderRadius: BorderRadius.circular(12),
        child: InkWell(
          borderRadius: BorderRadius.circular(12),
          onTap: onTap == null
              ? null
              : () {
                  HapticFeedback.selectionClick();
                  onTap!();
                },
          child: AnimatedContainer(
            duration: const Duration(milliseconds: 260),
            curve: Curves.easeOutCubic,
            constraints: BoxConstraints(minWidth: minWidth, minHeight: 48),
            padding: const EdgeInsets.symmetric(vertical: 8, horizontal: 4),
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(12),
              color: context.palette.surfaceVariant,
              border: Border.all(
                color: selected ? context.palette.primary : context.palette.divider,
                width: selected ? 2 : 1,
              ),
              boxShadow: selected && enabled
                  ? [
                      BoxShadow(
                        color: context.palette.primary.withOpacity(0.28),
                        blurRadius: 14,
                        offset: const Offset(0, 3),
                      ),
                    ]
                  : null,
            ),
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              mainAxisSize: MainAxisSize.min,
              children: [
                AnimatedDefaultTextStyle(
                  duration: const Duration(milliseconds: 260),
                  style: TextStyle(
                    fontSize: 10,
                    height: 1,
                    fontWeight: FontWeight.w500,
                    color: inactiveColor,
                    letterSpacing: 0.5,
                  ),
                  child: const Text('SF'),
                ),
                const SizedBox(height: 2),
                AnimatedDefaultTextStyle(
                  duration: const Duration(milliseconds: 260),
                  style: TextStyle(
                    fontSize: 16,
                    height: 1,
                    fontWeight: selected ? FontWeight.w800 : FontWeight.w600,
                    color: selected ? context.palette.primary : inactiveColor,
                    fontFeatures: const [FontFeature.tabularFigures()],
                  ),
                  child: Text('$sf'),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class SettingsScreen extends StatefulWidget {
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

  const SettingsScreen({
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
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  late final TextEditingController _nickController;
  late final TextEditingController _wifiSsidController;
  late final TextEditingController _wifiPassController;
  late final TextEditingController _inviteIdController;
  late final TextEditingController _inviteKeyController;
  late final TextEditingController _inviteChannelKeyController;

  late String _region;
  late int? _channel;
  int? _sf;
  late bool _gpsPresent;
  late bool _gpsEnabled;
  late bool _gpsFix;
  late bool _powersave;
  late bool _meshAnimationEnabled;
  int? _offlinePending;

  /// Актуальный ID (обновляется по evt info, пока открыты настройки).
  late String _nodeIdLive;
  StreamSubscription<RiftLinkEvent>? _bleSub;

  void _syncFromWidget() {
    _region = widget.region;
    _channel = widget.channel;
    _sf = widget.sf;
    _gpsPresent = widget.gpsPresent;
    _gpsEnabled = widget.gpsEnabled;
    _gpsFix = widget.gpsFix;
    _powersave = widget.powersave;
    _meshAnimationEnabled = widget.meshAnimationEnabled;
    _offlinePending = widget.offlinePending;
  }

  @override
  void initState() {
    super.initState();
    _nodeIdLive = widget.nodeId;
    _syncFromWidget();
    _nickController = TextEditingController(text: widget.nickname ?? '');
    _wifiSsidController = TextEditingController();
    _wifiPassController = TextEditingController();
    _inviteIdController = TextEditingController();
    _inviteKeyController = TextEditingController();
    _inviteChannelKeyController = TextEditingController();

    _bleSub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      if (evt is RiftLinkInfoEvent) {
        setState(() {
          if (evt.id.isNotEmpty) _nodeIdLive = evt.id;
          _region = evt.region;
          _channel = evt.channel;
          _sf = evt.sf;
          _gpsPresent = evt.gpsPresent;
          _gpsEnabled = evt.gpsEnabled;
          _gpsFix = evt.gpsFix;
          _powersave = evt.powersave;
          _offlinePending = evt.offlinePending;
        });
        final nick = evt.nickname?.trim();
        if (nick != null && nick.isNotEmpty && _nickController.text.trim().isEmpty) {
          _nickController.text = nick;
        }
      } else if (evt is RiftLinkRegionEvent) {
        setState(() {
          _region = evt.region;
          _channel = evt.channel;
        });
      } else if (evt is RiftLinkGpsEvent) {
        setState(() {
          _gpsPresent = evt.present;
          _gpsEnabled = evt.enabled;
          _gpsFix = evt.hasFix;
        });
      }
    });
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (widget.ble.isConnected) widget.ble.getInfo();
    });
  }

  @override
  void didUpdateWidget(covariant SettingsScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    _syncFromWidget();
    if (oldWidget.nodeId != widget.nodeId && widget.nodeId.isNotEmpty) {
      _nodeIdLive = widget.nodeId;
    }
  }

  @override
  void dispose() {
    _bleSub?.cancel();
    _nickController.dispose();
    _wifiSsidController.dispose();
    _wifiPassController.dispose();
    _inviteIdController.dispose();
    _inviteKeyController.dispose();
    _inviteChannelKeyController.dispose();
    super.dispose();
  }

  void _snack(String t) => showAppSnackBar(context, t);

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final regions = ['EU', 'RU', 'UK', 'US', 'AU'];
    final isEu = _region == 'EU' || _region == 'UK';
    final connected = widget.ble.isConnected;
    final idPlain = _nodeIdForClipboard(_nodeIdLive);
    final idShown = _formatNodeIdDisplay(_nodeIdLive);

    return Scaffold(
      backgroundColor: context.palette.surface,
      appBar: AppBar(
        title: Text(l.tr('settings')),
        leading: IconButton(
          icon: const Icon(Icons.arrow_back),
          onPressed: () => Navigator.pop(context),
        ),
      ),
      body: MeshBackgroundWrapper(
        child: SafeArea(
          child: ListView(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
            children: [
              _SettingsCard(
                title: l.tr('settings_device'),
                subtitle: l.tr('settings_node_id_hint'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Text(
                      l.tr('settings_node_id'),
                      style: TextStyle(color: context.palette.onSurfaceVariant, fontSize: 13),
                    ),
                    const SizedBox(height: 8),
                    SelectableText(
                      idShown,
                      style: TextStyle(
                        color: context.palette.onSurface,
                        fontSize: 15,
                        height: 1.35,
                        fontFamily: 'monospace',
                        letterSpacing: 0.5,
                      ),
                    ),
                    const SizedBox(height: 12),
                    OutlinedButton.icon(
                      onPressed: idPlain.isEmpty
                          ? null
                          : () async {
                              await Clipboard.setData(ClipboardData(text: idPlain));
                              if (mounted) _snack(l.tr('copied'));
                            },
                      icon: const Icon(Icons.copy, size: 18),
                      label: Text(l.tr('copy')),
                    ),
                  ],
                ),
              ),
              const SizedBox(height: 12),
              _SettingsCard(
                title: l.tr('nickname'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    TextField(
                      controller: _nickController,
                      maxLength: 16,
                      style: TextStyle(color: context.palette.onSurface),
                      decoration: InputDecoration(
                        hintText: l.tr('nickname_hint'),
                        counterText: '',
                      ),
                    ),
                    const SizedBox(height: 12),
                    FilledButton(
                      onPressed: connected
                          ? () async {
                              final n = _nickController.text.trim();
                              if (await widget.ble.setNickname(n)) {
                                widget.onNicknameChanged(n);
                                if (mounted) _snack(l.tr('saved'));
                              }
                            }
                          : null,
                      child: Text(l.tr('save')),
                    ),
                  ],
                ),
              ),
              const SizedBox(height: 12),
              _SettingsCard(
                title: l.tr('region'),
                subtitle: l.tr('settings_region_hint'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Text(
                      l.tr('region_warning'),
                      style: TextStyle(color: context.palette.onSurfaceVariant, fontSize: 12, height: 1.35),
                    ),
                    const SizedBox(height: 14),
                    _SegmentedPickBar(
                      leadingIcon: Icons.public_rounded,
                      labels: regions,
                      selectedIndex: regions.contains(_region) ? regions.indexOf(_region) : null,
                      enabled: connected,
                      onSelected: (i) async {
                        final r = regions[i];
                        if (await widget.ble.setRegion(r)) {
                          widget.onRegionChanged(r, _channel);
                          setState(() => _region = r);
                        }
                      },
                    ),
                    if (isEu) ...[
                      const SizedBox(height: 18),
                      Row(
                        children: [
                          Icon(Icons.radio_button_checked, size: 18, color: context.palette.primary.withOpacity(0.9)),
                          const SizedBox(width: 8),
                          Text(
                            l.tr('channel_eu'),
                            style: TextStyle(
                              color: context.palette.onSurface,
                              fontSize: 14,
                              fontWeight: FontWeight.w600,
                              letterSpacing: 0.2,
                            ),
                          ),
                        ],
                      ),
                      const SizedBox(height: 10),
                      _SegmentedPickBar(
                        leadingIcon: Icons.layers_outlined,
                        labels: const ['0', '1', '2'],
                        selectedIndex: _channel != null && _channel! >= 0 && _channel! <= 2 ? _channel : null,
                        enabled: connected,
                        onSelected: (i) async {
                          if (await widget.ble.setChannel(i)) {
                            widget.onRegionChanged(_region, i);
                            setState(() => _channel = i);
                          }
                        },
                      ),
                    ],
                  ],
                ),
              ),
              const SizedBox(height: 12),
              _SettingsCard(
                title: l.tr('settings_sf'),
                subtitle: l.tr('settings_sf_hint'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Row(
                      children: [
                        Icon(Icons.tune_rounded, size: 20, color: context.palette.primary.withOpacity(0.95)),
                        const SizedBox(width: 8),
                        Expanded(
                          child: Text(
                            l.tr('settings_sf_legend'),
                            style: TextStyle(
                              fontSize: 11.5,
                              height: 1.35,
                              color: context.palette.onSurfaceVariant.withOpacity(0.95),
                            ),
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 12),
                    _SfPickGrid(
                      selectedSf: _sf,
                      enabled: connected,
                      onSelectSf: (sf) async {
                        if (await widget.ble.setSpreadingFactor(sf)) {
                          widget.onSfChanged(sf);
                          setState(() => _sf = sf);
                          widget.ble.getInfo();
                        }
                      },
                    ),
                  ],
                ),
              ),
              const SizedBox(height: 12),
              _SettingsCard(
                title: l.tr('wifi_ota_section'),
                subtitle: l.tr('wifi_ota_section_hint'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Row(
                      children: [
                        Container(
                          width: 3,
                          height: 14,
                          decoration: BoxDecoration(
                            color: context.palette.primary,
                            borderRadius: BorderRadius.circular(2),
                          ),
                        ),
                        const SizedBox(width: 10),
                        Text(
                          l.tr('wifi_station_block'),
                          style: TextStyle(
                            fontSize: 13,
                            fontWeight: FontWeight.w600,
                            color: context.palette.onSurface,
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 12),
                    TextField(
                      controller: _wifiSsidController,
                      style: TextStyle(color: context.palette.onSurface),
                      decoration: InputDecoration(labelText: l.tr('wifi_ssid')),
                    ),
                    const SizedBox(height: 12),
                    TextField(
                      controller: _wifiPassController,
                      obscureText: true,
                      style: TextStyle(color: context.palette.onSurface),
                      decoration: InputDecoration(labelText: l.tr('wifi_password')),
                    ),
                    const SizedBox(height: 16),
                    FilledButton(
                      onPressed: connected
                          ? () async {
                              final ssid = _wifiSsidController.text.trim();
                              if (ssid.isEmpty) return;
                              await widget.ble.setWifi(ssid: ssid, pass: _wifiPassController.text);
                              if (mounted) _snack(l.tr('wifi_connect'));
                            }
                          : null,
                      child: Text(l.tr('connect')),
                    ),
                    const SizedBox(height: 22),
                    Divider(height: 1, color: context.palette.divider),
                    const SizedBox(height: 18),
                    Row(
                      children: [
                        Container(
                          width: 3,
                          height: 14,
                          decoration: BoxDecoration(
                            color: context.palette.primary,
                            borderRadius: BorderRadius.circular(2),
                          ),
                        ),
                        const SizedBox(width: 10),
                        Expanded(
                          child: Text(
                            l.tr('wifi_ota_upload_block'),
                            style: TextStyle(
                              fontSize: 13,
                              fontWeight: FontWeight.w600,
                              color: context.palette.onSurface,
                            ),
                          ),
                        ),
                        Icon(Icons.system_update_alt, size: 18, color: context.palette.primary.withOpacity(0.9)),
                      ],
                    ),
                    const SizedBox(height: 8),
                    Text(
                      l.tr('ota_start_hint'),
                      style: TextStyle(
                        fontSize: 12,
                        height: 1.35,
                        color: context.palette.onSurfaceVariant.withOpacity(0.95),
                      ),
                    ),
                    const SizedBox(height: 14),
                    FilledButton.tonal(
                      style: FilledButton.styleFrom(
                        foregroundColor: context.palette.onSurface,
                        backgroundColor: context.palette.primary.withOpacity(0.18),
                      ),
                      onPressed: connected
                          ? () async {
                              HapticFeedback.lightImpact();
                              final ok = await widget.ble.sendOta();
                              if (mounted) _snack(ok ? l.tr('ota_started') : l.tr('ota_failed'));
                            }
                          : null,
                      child: Row(
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          Icon(Icons.system_update_alt, size: 20, color: context.palette.primary),
                          const SizedBox(width: 8),
                          Text(l.tr('ota_title'), style: const TextStyle(fontWeight: FontWeight.w600)),
                        ],
                      ),
                    ),
                  ],
                ),
              ),
              if (_gpsPresent) ...[
                const SizedBox(height: 12),
                _SettingsCard(
                  title: l.tr('gps_section'),
                  child: SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    title: Text(l.tr('gps_enable'), style: TextStyle(color: context.palette.onSurface)),
                    subtitle: Text(
                      _gpsFix ? l.tr('gps_fix_yes') : l.tr('gps_fix_no'),
                      style: TextStyle(color: context.palette.onSurfaceVariant, fontSize: 13),
                    ),
                    value: _gpsEnabled,
                    activeThumbColor: context.palette.primary,
                    activeTrackColor: context.palette.primary.withOpacity(0.45),
                    onChanged: connected
                        ? (v) async {
                            if (await widget.ble.setGps(v)) {
                              widget.onGpsChanged(v);
                              setState(() => _gpsEnabled = v);
                            }
                          }
                        : null,
                  ),
                ),
              ],
              const SizedBox(height: 12),
              _SettingsCard(
                title: l.tr('settings_energy_title'),
                subtitle: l.tr('settings_energy_hint'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    const SizedBox(height: 4),
                    Row(
                      children: [
                        Container(
                          width: 3,
                          height: 14,
                          decoration: BoxDecoration(
                            color: context.palette.primary,
                            borderRadius: BorderRadius.circular(2),
                          ),
                        ),
                        const SizedBox(width: 10),
                        Expanded(
                          child: Text(
                            l.tr('settings_energy_node'),
                            style: TextStyle(
                              fontSize: 13,
                              fontWeight: FontWeight.w600,
                              color: context.palette.onSurface,
                            ),
                          ),
                        ),
                        Icon(Icons.memory_rounded, size: 18, color: context.palette.onSurfaceVariant),
                      ],
                    ),
                    const SizedBox(height: 10),
                    _SegmentedPickBar(
                      leadingIcon: Icons.battery_saver_outlined,
                      labels: [l.tr('powersave_mode_normal'), l.tr('powersave_mode_eco')],
                      selectedIndex: _powersave ? 1 : 0,
                      enabled: connected,
                      onSelected: (i) async {
                        final eco = i == 1;
                        if (eco == _powersave) return;
                        if (await widget.ble.setPowersave(eco)) {
                          widget.onPowersaveChanged(eco);
                          setState(() => _powersave = eco);
                        }
                      },
                    ),
                    const SizedBox(height: 20),
                    Row(
                      children: [
                        Container(
                          width: 3,
                          height: 14,
                          decoration: BoxDecoration(
                            color: context.palette.primary,
                            borderRadius: BorderRadius.circular(2),
                          ),
                        ),
                        const SizedBox(width: 10),
                        Expanded(
                          child: Text(
                            l.tr('settings_energy_app'),
                            style: TextStyle(
                              fontSize: 13,
                              fontWeight: FontWeight.w600,
                              color: context.palette.onSurface,
                            ),
                          ),
                        ),
                        Icon(Icons.smartphone_rounded, size: 18, color: context.palette.onSurfaceVariant),
                      ],
                    ),
                    const SizedBox(height: 6),
                    ListTile(
                      contentPadding: EdgeInsets.zero,
                      leading: Icon(Icons.dark_mode_outlined, color: context.palette.primary),
                      title: Text(l.tr('theme'), style: TextStyle(color: context.palette.onSurface, fontWeight: FontWeight.w500)),
                      subtitle: Text(
                        l.tr('theme_hint'),
                        style: TextStyle(color: context.palette.onSurfaceVariant, fontSize: 12, height: 1.35),
                      ),
                      trailing: Icon(Icons.chevron_right, color: context.palette.onSurfaceVariant),
                      onTap: () {
                        HapticFeedback.selectionClick();
                        showThemeModeSheet(context);
                      },
                    ),
                    const SizedBox(height: 4),
                    SwitchListTile(
                      contentPadding: EdgeInsets.zero,
                      title: Text(
                        l.tr('mesh_animation'),
                        style: TextStyle(color: context.palette.onSurface, fontWeight: FontWeight.w500),
                      ),
                      subtitle: Text(
                        l.tr('mesh_animation_hint'),
                        style: TextStyle(color: context.palette.onSurfaceVariant, fontSize: 12, height: 1.35),
                      ),
                      secondary: Icon(Icons.animation_rounded, color: context.palette.primary),
                      value: _meshAnimationEnabled,
                      activeThumbColor: context.palette.primary,
                      activeTrackColor: context.palette.primary.withOpacity(0.45),
                      onChanged: (v) {
                        HapticFeedback.selectionClick();
                        widget.onMeshAnimationChanged(v);
                        setState(() => _meshAnimationEnabled = v);
                      },
                    ),
                  ],
                ),
              ),
              const SizedBox(height: 12),
              _SettingsCard(
                title: l.tr('e2e_invite'),
                subtitle: l.tr('e2e_invite_hint'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    const SizedBox(height: 4),
                    OutlinedButton.icon(
                      style: OutlinedButton.styleFrom(
                        foregroundColor: context.palette.primary,
                        side: BorderSide(color: context.palette.primary, width: 1.2),
                        padding: const EdgeInsets.symmetric(vertical: 14, horizontal: 16),
                        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
                      ),
                      onPressed: connected
                          ? () {
                              HapticFeedback.lightImpact();
                              widget.ble.createInvite();
                            }
                          : null,
                      icon: const Icon(Icons.add_moderator_outlined, size: 22),
                      label: Text(l.tr('create_invite'), style: const TextStyle(fontWeight: FontWeight.w600)),
                    ),
                    const SizedBox(height: 22),
                    Row(
                      children: [
                        Container(
                          width: 3,
                          height: 16,
                          decoration: BoxDecoration(
                            color: context.palette.primary,
                            borderRadius: BorderRadius.circular(2),
                          ),
                        ),
                        const SizedBox(width: 10),
                        Text(
                          l.tr('invite_accept_section'),
                          style: TextStyle(
                            fontSize: 14,
                            fontWeight: FontWeight.w600,
                            color: context.palette.onSurface,
                            letterSpacing: 0.2,
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 12),
                    Container(
                      padding: const EdgeInsets.all(14),
                      decoration: BoxDecoration(
                        color: context.palette.surfaceVariant.withOpacity(0.65),
                        borderRadius: BorderRadius.circular(12),
                        border: Border.all(color: context.palette.divider),
                      ),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        children: [
                          TextField(
                            controller: _inviteIdController,
                            style: TextStyle(color: context.palette.onSurface, fontFamily: 'monospace', fontSize: 13),
                            decoration: InputDecoration(
                              isDense: true,
                              labelText: l.tr('inviter_id'),
                              suffixIcon: IconButton(
                                icon: Icon(Icons.content_paste_rounded, color: context.palette.primary),
                                tooltip: l.tr('paste'),
                                onPressed: () async {
                                  final data = await Clipboard.getData(Clipboard.kTextPlain);
                                  final text = data?.text?.trim();
                                  if (text == null || text.isEmpty) return;
                                  try {
                                    final m = jsonDecode(text) as Map<String, dynamic>?;
                                    if (m != null) {
                                      final id = (m['id'] as String?) ?? '';
                                      final pk = (m['pubKey'] as String?) ?? '';
                                      final ck = (m['channelKey'] as String?) ?? '';
                                      _inviteIdController.text = id.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '');
                                      _inviteKeyController.text = pk;
                                      _inviteChannelKeyController.text = ck;
                                      if (mounted) setState(() {});
                                    }
                                  } catch (_) {}
                                },
                              ),
                            ),
                            maxLength: 16,
                          ),
                          const SizedBox(height: 12),
                          TextField(
                            controller: _inviteKeyController,
                            style: TextStyle(color: context.palette.onSurface, fontSize: 13, height: 1.35),
                            decoration: InputDecoration(
                              isDense: true,
                              labelText: l.tr('invite_pubkey'),
                              alignLabelWithHint: true,
                            ),
                            maxLines: 3,
                            minLines: 2,
                          ),
                          const SizedBox(height: 12),
                          TextField(
                            controller: _inviteChannelKeyController,
                            style: TextStyle(color: context.palette.onSurface, fontSize: 13, height: 1.35),
                            decoration: InputDecoration(
                              isDense: true,
                              labelText: l.tr('invite_channel_key'),
                              alignLabelWithHint: true,
                            ),
                            maxLines: 2,
                            minLines: 1,
                          ),
                        ],
                      ),
                    ),
                    const SizedBox(height: 14),
                    FilledButton.icon(
                      style: FilledButton.styleFrom(
                        padding: const EdgeInsets.symmetric(vertical: 14, horizontal: 18),
                        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
                      ),
                      onPressed: connected
                          ? () async {
                              HapticFeedback.lightImpact();
                              final id = _inviteIdController.text.trim().replaceAll(RegExp(r'[^0-9A-Fa-f]'), '');
                              final key = _inviteKeyController.text.trim();
                              final ck = _inviteChannelKeyController.text.trim();
                              if (id.length < 8 || key.isEmpty) return;
                              if (await widget.ble.acceptInvite(
                                id: id.substring(0, id.length > 16 ? 16 : id.length),
                                pubKey: key,
                                channelKey: ck.isEmpty ? null : ck,
                              )) {
                                if (mounted) _snack(l.tr('invite_accepted'));
                              }
                            }
                          : null,
                      icon: const Icon(Icons.check_circle_outline_rounded, size: 22),
                      label: Text(l.tr('accept_invite'), style: const TextStyle(fontWeight: FontWeight.w600)),
                    ),
                  ],
                ),
              ),
              if ((_offlinePending != null && _offlinePending! > 0) ||
                  (widget.batteryMv != null && widget.batteryMv! > 0)) ...[
                const SizedBox(height: 12),
                _SettingsCard(
                  title: l.tr('other'),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      if (_offlinePending != null && _offlinePending! > 0)
                        ListTile(
                          contentPadding: EdgeInsets.zero,
                          leading: Icon(Icons.schedule, color: context.palette.onSurfaceVariant),
                          title: Text(
                            '${l.tr('offline_pending')}: $_offlinePending',
                            style: TextStyle(color: context.palette.onSurface),
                          ),
                        ),
                      if (widget.batteryMv != null && widget.batteryMv! > 0)
                        ListTile(
                          contentPadding: EdgeInsets.zero,
                          leading: Icon(Icons.battery_charging_full, color: context.palette.onSurfaceVariant),
                          title: Text(
                            '${(widget.batteryMv! / 1000).toStringAsFixed(2)} V',
                            style: TextStyle(color: context.palette.onSurface),
                          ),
                        ),
                    ],
                  ),
                ),
              ],
            ],
          ),
        ),
      ),
    );
  }
}

class _SettingsCard extends StatelessWidget {
  final String title;
  final String? subtitle;
  final Widget child;

  const _SettingsCard({
    required this.title,
    this.subtitle,
    required this.child,
  });

  @override
  Widget build(BuildContext context) {
    return Card(
      color: context.palette.card,
      elevation: 0,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
        side: BorderSide(color: context.palette.divider, width: 1),
      ),
      margin: EdgeInsets.zero,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 14, 16, 16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(
              title,
              style: TextStyle(
                fontSize: 17,
                fontWeight: FontWeight.w600,
                color: context.palette.onSurface,
                letterSpacing: 0.2,
              ),
            ),
            if (subtitle != null) ...[
              const SizedBox(height: 4),
              Text(
                subtitle!,
                style: TextStyle(fontSize: 12, color: context.palette.onSurfaceVariant, height: 1.3),
              ),
            ],
            const SizedBox(height: 8),
            child,
          ],
        ),
      ),
    );
  }
}
