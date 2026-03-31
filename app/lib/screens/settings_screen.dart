import 'dart:async';
import 'dart:convert';
import 'dart:ui' show FontFeature;
import 'dart:math' show min;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../ble/device_sync_reason.dart';
import '../ble/riftlink_ble.dart';
import '../contacts/contacts_service.dart';
import '../l10n/app_localizations.dart';
import '../prefs/mesh_prefs.dart';
import '../prefs/wifi_sta_prefs.dart';
import '../theme/app_theme.dart';
import '../theme/design_tokens.dart';
import '../theme/theme_notifier.dart';
import '../widgets/app_primitives.dart';
import '../widgets/mesh_background.dart';
import '../widgets/app_snackbar.dart';
import '../app_navigator.dart';
import 'ota_screen.dart';
import 'scan_screen.dart';

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
          borderRadius: BorderRadius.circular(AppRadius.lg),
          border: Border.all(color: context.palette.divider),
        ),
        padding: const EdgeInsets.all(AppSpacing.xs),
        child: Row(
          children: [
            if (leadingIcon != null) ...[
              Padding(
                padding: const EdgeInsets.only(left: AppSpacing.sm, right: AppSpacing.xs),
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
                          borderRadius: BorderRadius.circular(AppRadius.md),
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
                                      borderRadius: BorderRadius.circular(AppRadius.md),
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
                                        borderRadius: BorderRadius.circular(AppRadius.md),
                                        onTap: enabled
                                            ? () {
                                                HapticFeedback.selectionClick();
                                                onSelected(i);
                                              }
                                            : null,
                                        child: Padding(
                                          padding: const EdgeInsets.symmetric(vertical: AppSpacing.md, horizontal: 2),
                                          child: Center(
                                            child: AnimatedDefaultTextStyle(
                                              duration: _anim,
                                              curve: _curve,
                                              style: AppTypography.labelBase().copyWith(
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
        final gap = AppSpacing.sm;
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
        borderRadius: BorderRadius.circular(AppRadius.md),
        child: InkWell(
          borderRadius: BorderRadius.circular(AppRadius.md),
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
            padding: const EdgeInsets.symmetric(vertical: AppSpacing.sm, horizontal: AppSpacing.xs),
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(AppRadius.md),
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
                  style: AppTypography.captionDenseBase().copyWith(
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
                  style: AppTypography.bodyLargeBase().copyWith(
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

class _ModemSection extends StatefulWidget {
  final int? modemPreset;
  final int? sf;
  final double? bw;
  final int? cr;
  final bool enabled;
  final Future<void> Function(int preset) onPreset;
  final Future<void> Function(int sf, double bw, int cr) onCustom;

  const _ModemSection({
    this.modemPreset,
    this.sf,
    this.bw,
    this.cr,
    this.enabled = true,
    required this.onPreset,
    required this.onCustom,
  });

  @override
  State<_ModemSection> createState() => _ModemSectionState();
}

class _ModemSectionState extends State<_ModemSection> {
  static const _presetDescRu = [
    'SF7 · BW250 · CR5\nГород, скорость, малая дальность',
    'SF7 · BW125 · CR5\nБаланс скорости и дальности',
    'SF10 · BW125 · CR5\nХорошая дальность',
    'SF12 · BW125 · CR8\nМакс. дальность, медленно',
    'Ручная настройка SF / BW / CR',
  ];
  static const _presetDescEn = [
    'SF7 · BW250 · CR5\nCity use, high speed, short range',
    'SF7 · BW125 · CR5\nBalanced speed and range',
    'SF10 · BW125 · CR5\nGood long-range profile',
    'SF12 · BW125 · CR8\nMaximum range, slower throughput',
    'Manual SF / BW / CR configuration',
  ];
  static const _presetSf  = [7, 7, 10, 12];
  static const _presetBw  = [250.0, 125.0, 125.0, 125.0];
  static const _presetCr  = [5, 5, 5, 8];
  static const _bwOptions = [62.5, 125.0, 250.0, 500.0];
  static const _crOptions = [5, 6, 7, 8];
  static const _crDesc    = ['4/5 мин.', '4/6', '4/7', '4/8 макс.'];

  late int _sel;
  late int _cSf;
  late double _cBw;
  late int _cCr;

  @override
  void initState() {
    super.initState();
    _syncFromWidget();
  }

  @override
  void didUpdateWidget(_ModemSection old) {
    super.didUpdateWidget(old);
    if (old.modemPreset != widget.modemPreset ||
        old.sf != widget.sf || old.bw != widget.bw || old.cr != widget.cr) {
      _syncFromWidget();
    }
  }

  void _syncFromWidget() {
    _sel = (widget.modemPreset != null && widget.modemPreset! >= 0 && widget.modemPreset! <= 4)
        ? widget.modemPreset! : 1;
    _cSf = widget.sf ?? 7;
    _cBw = widget.bw ?? 125.0;
    _cCr = widget.cr ?? 5;
  }

  @override
  Widget build(BuildContext context) {
    final pal = context.palette;
    final enabled = widget.enabled;
    final isCustom = _sel == 4;
    final isRu = AppLocalizations.currentLocale.languageCode == 'ru';
    final presetDesc = isRu ? _presetDescRu : _presetDescEn;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Wrap(
          spacing: AppSpacing.sm - 2,
          runSpacing: AppSpacing.sm - 2,
          children: [
            for (var i = 0; i < 5; i++)
              _presetChip(context, i, enabled),
          ],
        ),
        const SizedBox(height: AppSpacing.sm),
        AnimatedSize(
          duration: const Duration(milliseconds: 200),
          curve: Curves.easeOut,
          child: Padding(
            padding: const EdgeInsets.symmetric(vertical: AppSpacing.xs),
            child: Text(
              presetDesc[_sel],
              style: AppTypography.chipBase().copyWith(height: 1.4, color: pal.onSurfaceVariant.withOpacity(0.85)),
            ),
          ),
        ),
        if (isCustom) ...[
          const SizedBox(height: AppSpacing.sm + 2),
          Text('SF', style: AppTypography.chipBase().copyWith(fontWeight: FontWeight.w600, color: pal.onSurfaceVariant)),
          const SizedBox(height: AppSpacing.xs),
          _SfPickGrid(
            selectedSf: _cSf,
            enabled: enabled,
            onSelectSf: (sf) {
              setState(() => _cSf = sf);
            },
          ),
          const SizedBox(height: AppSpacing.sm + 2),
          Text('BW (kHz)', style: AppTypography.chipBase().copyWith(fontWeight: FontWeight.w600, color: pal.onSurfaceVariant)),
          const SizedBox(height: AppSpacing.xs),
          _optionRow<double>(
            context,
            options: _bwOptions,
            selected: _cBw,
            label: (v) => v == 62.5 ? '62.5' : '${v.toInt()}',
            enabled: enabled,
            onSelect: (v) {
              setState(() => _cBw = v);
            },
          ),
          const SizedBox(height: AppSpacing.sm + 2),
          Text('CR', style: AppTypography.chipBase().copyWith(fontWeight: FontWeight.w600, color: pal.onSurfaceVariant)),
          const SizedBox(height: AppSpacing.xs),
          _optionRow<int>(
            context,
            options: _crOptions,
            selected: _cCr,
            label: (v) => _crDesc[v - 5],
            enabled: enabled,
            onSelect: (v) {
              setState(() => _cCr = v);
            },
          ),
          const SizedBox(height: AppSpacing.md),
          FilledButton.tonalIcon(
            onPressed: enabled ? () => widget.onCustom(_cSf, _cBw, _cCr) : null,
            icon: const Icon(Icons.tune_rounded, size: 18),
            label: Text(context.l10n.tr('modem_apply_custom')),
          ),
        ],
      ],
    );
  }

  Widget _presetChip(BuildContext context, int idx, bool enabled) {
    final pal = context.palette;
    final l = context.l10n;
    final sel = _sel == idx;
    final label = switch (idx) {
      0 => l.tr('modem_preset_speed'),
      1 => l.tr('modem_preset_normal'),
      2 => l.tr('modem_preset_range'),
      3 => l.tr('modem_preset_maxrange'),
      _ => l.tr('modem_preset_custom'),
    };
    return GestureDetector(
      onTap: enabled ? () {
        HapticFeedback.selectionClick();
        setState(() => _sel = idx);
        if (idx < 4) widget.onPreset(idx);
      } : null,
      child: AnimatedContainer(
        duration: const Duration(milliseconds: 200),
        constraints: const BoxConstraints(minWidth: 84),
        padding: const EdgeInsets.symmetric(horizontal: AppSpacing.md, vertical: AppSpacing.sm),
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(AppRadius.md),
          color: sel ? pal.primary.withOpacity(0.15) : pal.surfaceVariant,
          border: Border.all(
            color: sel ? pal.primary : pal.divider,
            width: sel ? 1.6 : 1,
          ),
        ),
        child: Text(
          label,
          textAlign: TextAlign.center,
          style: AppTypography.chipBase().copyWith(
            fontWeight: sel ? FontWeight.w700 : FontWeight.w500,
            color: sel ? pal.primary : pal.onSurfaceVariant.withOpacity(enabled ? 1.0 : 0.5),
          ),
        ),
      ),
    );
  }

  Widget _optionRow<T>(
    BuildContext context, {
    required List<T> options,
    required T selected,
    required String Function(T) label,
    required bool enabled,
    required ValueChanged<T> onSelect,
  }) {
    final pal = context.palette;
    return Row(
      children: [
        for (var i = 0; i < options.length; i++) ...[
          if (i > 0) const SizedBox(width: AppSpacing.sm - 2),
          Expanded(
            child: GestureDetector(
              onTap: enabled ? () {
                HapticFeedback.selectionClick();
                onSelect(options[i]);
              } : null,
              child: AnimatedContainer(
                duration: const Duration(milliseconds: 200),
                padding: const EdgeInsets.symmetric(vertical: AppSpacing.sm),
                alignment: Alignment.center,
                decoration: BoxDecoration(
                  borderRadius: BorderRadius.circular(AppRadius.md),
                  color: options[i] == selected ? pal.primary.withOpacity(0.15) : pal.surfaceVariant,
                  border: Border.all(
                    color: options[i] == selected ? pal.primary : pal.divider,
                    width: options[i] == selected ? 1.6 : 1,
                  ),
                ),
                child: Text(
                  label(options[i]),
                  style: AppTypography.captionBase().copyWith(
                    fontWeight: options[i] == selected ? FontWeight.w700 : FontWeight.w500,
                    color: options[i] == selected ? pal.primary : pal.onSurfaceVariant.withOpacity(enabled ? 1.0 : 0.5),
                  ),
                ),
              ),
            ),
          ),
        ],
      ],
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

class _SettingsScreenState extends State<SettingsScreen> with WidgetsBindingObserver {
  final FocusNode _nickFocus = FocusNode();
  late final TextEditingController _nickController;
  late final TextEditingController _wifiSsidController;
  late final TextEditingController _wifiPassController;
  late final TextEditingController _inviteIdController;
  late final TextEditingController _inviteKeyController;
  late final TextEditingController _inviteChannelKeyController;
  late final TextEditingController _inviteTokenController;
  late final TextEditingController _voiceAcceptAvgLossController;
  late final TextEditingController _voiceAcceptHardLossController;
  late final TextEditingController _voiceAcceptMinSessionsController;

  late String _region;
  late int? _channel;
  int? _sf;
  double? _bw;
  int? _cr;
  int? _modemPreset;  // 0=Speed,1=Normal,2=Range,3=MaxRange,4=Custom
  late bool _gpsPresent;
  late bool _gpsEnabled;
  late bool _gpsFix;
  late bool _powersave;
  late bool _meshAnimationEnabled;
  int? _offlinePending;
  int? _offlineCourierPending;
  int? _offlineDirectPending;
  int? _batteryMv;
  int? _batteryPercent;
  bool _charging = false;
  int? _blePin;
  String? _version;
  String _radioMode = 'ble';
  String? _radioVariant;
  bool _wifiConnected = false;
  String? _wifiSsid;
  String? _wifiIp;
  int _espNowChannel = 1;
  bool _espNowAdaptive = false;
  bool _modemApplying = false;
  bool _radioModeApplying = false;
  String? _inviteStatusText;
  bool _inviteStatusError = false;
  String? _invitePendingPeerId;
  Timer? _inviteExpiryTimer;
  bool _voiceAcceptanceSaving = false;
  Map<String, String> _nickById = const {};

  /// Актуальный ID (обновляется по evt info, пока открыты настройки).
  late String _nodeIdLive;
  StreamSubscription<RiftLinkEvent>? _bleSub;
  Timer? _infoRetryTimer;
  bool _infoReceivedSinceOpen = false;

  /// Единая точка: evt info и кэш [RiftLinkBle.lastInfo] (до подписки на поток данные уже могли прийти).
  void _applyRiftLinkInfo(RiftLinkInfoEvent evt) {
    if (!mounted) return;
    _infoReceivedSinceOpen = true;
    _infoRetryTimer?.cancel();
    _infoRetryTimer = null;
    setState(() {
      if (evt.id.isNotEmpty) _nodeIdLive = evt.id;
      _region = evt.region;
      if (evt.hasChannelField) _channel = evt.channel;
      _sf = evt.sf;
      _bw = evt.bw;
      _cr = evt.cr;
      _modemPreset = evt.modemPreset;
      _gpsPresent = evt.gpsPresent;
      _gpsEnabled = evt.gpsEnabled;
      _gpsFix = evt.gpsFix;
      _powersave = evt.powersave;
      if (evt.hasOfflinePendingField) _offlinePending = evt.offlinePending;
      if (evt.hasOfflineCourierPendingField) _offlineCourierPending = evt.offlineCourierPending;
      if (evt.hasOfflineDirectPendingField) _offlineDirectPending = evt.offlineDirectPending;
      if (evt.batteryMv != null && evt.batteryMv! > 0) _batteryMv = evt.batteryMv;
      _batteryPercent = evt.batteryPercent;
      _charging = evt.charging;
      _blePin = evt.blePin;
      _version = evt.version;
      _radioMode = evt.radioMode;
      _radioVariant = evt.radioVariant;
      _wifiConnected = evt.wifiConnected;
      _wifiSsid = evt.wifiSsid;
      _wifiIp = evt.wifiIp;
      final w = evt.wifiSsid?.trim();
      if (w != null && w.isNotEmpty && _wifiSsidController.text.trim().isEmpty) {
        _wifiSsidController.text = w;
      }
      if (evt.espNowChannel != null && evt.espNowChannel! >= 1 && evt.espNowChannel! <= 13) {
        _espNowChannel = evt.espNowChannel!;
      }
      _espNowAdaptive = evt.espNowAdaptive;
    });
    final pendingPeer = _invitePendingPeerId;
    if (pendingPeer != null && _peerHasKey(evt, pendingPeer) && mounted) {
      setState(() {
        _inviteStatusError = false;
        _inviteStatusText = context.l10n.tr('invite_status_key_ready');
        _invitePendingPeerId = null;
      });
    }
    // Ключ nickname в JSON может отсутствовать (прошивка) — не перезаписываем поле из «пустого» evt.
    if (evt.hasNicknameField) {
      final nick = evt.nickname?.trim();
      if (nick != null && nick.isNotEmpty && !_nickFocus.hasFocus) {
        if (_nickController.text != nick) {
          _nickController.text = nick;
        }
      }
    }
  }

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    // Сразу подставляем кэш info — иначе первый кадр пустой, а didUpdateWidget (тема и т.д.) затирал бы данные из BLE.
    final li = widget.ble.lastInfo;
    _nodeIdLive = (li != null && li.id.isNotEmpty) ? li.id : widget.nodeId;
    _region = li?.region ?? widget.region;
    _channel = li?.channel ?? widget.channel;
    _sf = li?.sf ?? widget.sf;
    _bw = li?.bw;
    _cr = li?.cr;
    _modemPreset = li?.modemPreset;
    _gpsPresent = li?.gpsPresent ?? widget.gpsPresent;
    _gpsEnabled = li?.gpsEnabled ?? widget.gpsEnabled;
    _gpsFix = li?.gpsFix ?? widget.gpsFix;
    _powersave = li?.powersave ?? widget.powersave;
    _offlinePending = li?.offlinePending ?? widget.offlinePending;
    _offlineCourierPending = li?.offlineCourierPending;
    _offlineDirectPending = li?.offlineDirectPending;
    _batteryMv = li?.batteryMv ?? widget.batteryMv;
    _batteryPercent = li?.batteryPercent;
    _charging = li?.charging ?? false;
    _blePin = li?.blePin;
    _version = li?.version;
    _radioMode = li?.radioMode ?? 'ble';
    _radioVariant = li?.radioVariant;
    _wifiConnected = li?.wifiConnected ?? false;
    _wifiSsid = li?.wifiSsid;
    _wifiIp = li?.wifiIp;
    _espNowChannel = (li?.espNowChannel != null && li!.espNowChannel! >= 1 && li.espNowChannel! <= 13)
        ? li.espNowChannel!
        : 1;
    _espNowAdaptive = li?.espNowAdaptive ?? false;
    _meshAnimationEnabled = widget.meshAnimationEnabled;
    final nick0 = (li?.nickname != null && li!.nickname!.trim().isNotEmpty)
        ? li.nickname!.trim()
        : (widget.nickname ?? '');
    _nickController = TextEditingController(text: nick0);
    _wifiSsidController = TextEditingController();
    _wifiPassController = TextEditingController();
    _inviteIdController = TextEditingController();
    _inviteKeyController = TextEditingController();
    _inviteChannelKeyController = TextEditingController();
    _inviteTokenController = TextEditingController();
    _voiceAcceptAvgLossController = TextEditingController();
    _voiceAcceptHardLossController = TextEditingController();
    _voiceAcceptMinSessionsController = TextEditingController();
    _loadVoiceAcceptancePrefs();
    _loadContactNicknames();

    _bleSub?.cancel();
    _bleSub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      if (evt is RiftLinkInfoEvent) {
        _applyRiftLinkInfo(evt);
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
      } else if (evt is RiftLinkInviteEvent) {
        final ttlSec = evt.inviteTtlMs != null ? (evt.inviteTtlMs! / 1000).round() : null;
        _armInviteExpiryTimer(evt.inviteTtlMs);
        setState(() {
          _inviteStatusError = false;
          _inviteStatusText = ttlSec != null
              ? context.l10n.tr('invite_status_created_ttl', {'sec': '$ttlSec'})
              : context.l10n.tr('invite_created');
        });
      } else if (evt is RiftLinkErrorEvent && evt.code.startsWith('invite_')) {
        String mapped = evt.msg;
        if (evt.code == 'invite_peer_key_mismatch') {
          mapped = context.l10n.tr('invite_status_mismatch');
        } else if (evt.code == 'invite_token_bad_length' || evt.code == 'invite_token_bad_format') {
          mapped = context.l10n.tr('invite_status_token_bad');
        }
        setState(() {
          _inviteStatusError = true;
          _inviteStatusText = mapped;
        });
      }
    });

    WidgetsBinding.instance.addPostFrameCallback((_) async {
      if (!mounted) return;
      if (!widget.ble.isConnected) return;
      final cached = widget.ble.lastInfo;
      if (cached != null) _applyRiftLinkInfo(cached);
      widget.ble.requestDeviceSync(DeviceSyncReason.screenVisible);
      final id = _nodeIdForClipboard(_nodeIdLive);
      if (id.isNotEmpty) {
        final saved = await WifiStaPrefs.load(id);
        if (mounted &&
            saved != null &&
            saved.ssid.isNotEmpty &&
            _wifiSsidController.text.trim().isEmpty) {
          setState(() {
            _wifiSsidController.text = saved.ssid;
            _wifiPassController.text = saved.pass;
          });
        }
      }
      _infoRetryTimer?.cancel();
      _infoRetryTimer = Timer(const Duration(milliseconds: 700), () {
        if (!mounted || !widget.ble.isConnected || _infoReceivedSinceOpen) return;
        widget.ble.requestDeviceSync(DeviceSyncReason.screenVisible);
      });
    });
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    super.didChangeAppLifecycleState(state);
    if (state == AppLifecycleState.resumed && mounted && widget.ble.isConnected) {
      final c = widget.ble.lastInfo;
      if (c != null) _applyRiftLinkInfo(c);
      widget.ble.requestDeviceSync(DeviceSyncReason.screenVisible);
    }
  }

  @override
  void didUpdateWidget(covariant SettingsScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    // Не затираем поля из widget при каждом rebuild (тема и т.д.) — иначе сбрасываются данные, уже пришедшие по BLE.
    if (oldWidget.meshAnimationEnabled != widget.meshAnimationEnabled) {
      setState(() => _meshAnimationEnabled = widget.meshAnimationEnabled);
    }
    if (oldWidget.nodeId != widget.nodeId && widget.nodeId.isNotEmpty) {
      setState(() => _nodeIdLive = widget.nodeId);
    }
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _bleSub?.cancel();
    _infoRetryTimer?.cancel();
    _inviteExpiryTimer?.cancel();
    _nickFocus.dispose();
    _nickController.dispose();
    _wifiSsidController.dispose();
    _wifiPassController.dispose();
    _inviteIdController.dispose();
    _inviteKeyController.dispose();
    _inviteChannelKeyController.dispose();
    _inviteTokenController.dispose();
    _voiceAcceptAvgLossController.dispose();
    _voiceAcceptHardLossController.dispose();
    _voiceAcceptMinSessionsController.dispose();
    super.dispose();
  }

  Future<void> _loadContactNicknames() async {
    final contacts = await ContactsService.load();
    if (!mounted) return;
    setState(() => _nickById = ContactsService.buildNicknameMap(contacts));
  }

  void _snack(String t) => showAppSnackBar(context, t);

  String _normHex(String raw) => raw.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '').toUpperCase();

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

  void _armInviteExpiryTimer(int? ttlMs) {
    _inviteExpiryTimer?.cancel();
    if (ttlMs == null || ttlMs <= 0) return;
    _inviteExpiryTimer = Timer(Duration(milliseconds: ttlMs), () {
      if (!mounted) return;
      setState(() {
        // Не затираем успешный handshake, если он уже завершился.
        if (_invitePendingPeerId == null && _inviteStatusError == false &&
            _inviteStatusText == context.l10n.tr('invite_status_key_ready')) {
          return;
        }
        _inviteStatusError = true;
        _inviteStatusText = context.l10n.tr('invite_status_expired');
      });
    });
  }

  String? _validateInviteToken(String raw) {
    if (raw.isEmpty) return null;
    if (raw.length != 16) return context.l10n.tr('invite_status_token_bad');
    final ok = RegExp(r'^[0-9A-Fa-f]{16}$').hasMatch(raw);
    return ok ? null : context.l10n.tr('invite_status_token_bad');
  }

  Future<void> _loadVoiceAcceptancePrefs() async {
    final avg = await MeshPrefs.getVoiceAcceptMaxAvgLossPercent();
    final hard = await MeshPrefs.getVoiceAcceptMaxHardLossPercent();
    final minSessions = await MeshPrefs.getVoiceAcceptMinSessions();
    if (!mounted) return;
    setState(() {
      _voiceAcceptAvgLossController.text = '$avg';
      _voiceAcceptHardLossController.text = '$hard';
      _voiceAcceptMinSessionsController.text = '$minSessions';
    });
  }

  Future<void> _saveVoiceAcceptancePrefs() async {
    final l = context.l10n;
    final avg = int.tryParse(_voiceAcceptAvgLossController.text.trim());
    final hard = int.tryParse(_voiceAcceptHardLossController.text.trim());
    final minSessions = int.tryParse(_voiceAcceptMinSessionsController.text.trim());
    if (avg == null || hard == null || minSessions == null) {
      _snack(l.tr('voice_acceptance_bad_values'));
      return;
    }
    if (avg < 0 || avg > 100 || hard < 0 || hard > 100 || minSessions < 1 || minSessions > 500) {
      _snack(l.tr('voice_acceptance_bad_values'));
      return;
    }
    setState(() => _voiceAcceptanceSaving = true);
    await MeshPrefs.setVoiceAcceptMaxAvgLossPercent(avg);
    await MeshPrefs.setVoiceAcceptMaxHardLossPercent(hard);
    await MeshPrefs.setVoiceAcceptMinSessions(minSessions);
    if (!mounted) return;
    setState(() => _voiceAcceptanceSaving = false);
    _snack(l.tr('saved'));
  }

  bool _isModemMatch(
    RiftLinkInfoEvent info, {
    required int preset,
    int? sf,
    double? bw,
    int? cr,
  }) {
    if (info.modemPreset != preset) return false;
    if (preset != 4) return true;
    if (sf == null || bw == null || cr == null) return false;
    final infoSf = info.sf;
    final infoBw = info.bw;
    final infoCr = info.cr;
    if (infoSf == null || infoBw == null || infoCr == null) return false;
    return infoSf == sf && (infoBw - bw).abs() < 0.2 && infoCr == cr;
  }

  Future<bool> _waitForModemApply({
    required int preset,
    int? sf,
    double? bw,
    int? cr,
  }) async {
    for (var i = 0; i < 5; i++) {
      await widget.ble.requestDeviceSync(DeviceSyncReason.userRefresh, force: true);
      await Future<void>.delayed(const Duration(milliseconds: 280));
      final li = widget.ble.lastInfo;
      if (li != null && _isModemMatch(li, preset: preset, sf: sf, bw: bw, cr: cr)) {
        return true;
      }
    }
    return false;
  }

  String _radioModeLabel(AppLocalizations l, String mode) {
    if (mode == 'wifi') {
      if (_radioVariant == 'sta') return l.tr('radio_mode_wifi_sta');
      return l.tr('radio_mode_wifi');
    }
    return l.tr('radio_mode_ble');
  }

  Future<void> _switchRadioToBle() async {
    if (!widget.ble.isTransportConnected || _radioModeApplying) return;
    setState(() => _radioModeApplying = true);
    final ok = await widget.ble.switchToBle();
    if (!mounted) return;
    if (ok) {
      await widget.ble.disconnect();
      if (!mounted) return;
      await appResetTo(
        context,
        ScanScreen(
          initialMessage: context.l10n.tr('device_switched_ble_scan'),
          initialSnackKind: AppSnackKind.neutral,
        ),
      );
      return;
    }
    if (mounted) {
      setState(() => _radioModeApplying = false);
      _snack(context.l10n.tr('radio_mode_failed'));
    }
  }

  Future<void> _switchRadioToWifiSta() async {
    if (!widget.ble.isTransportConnected || _radioModeApplying) return;
    final ssid = _wifiSsidController.text.trim();
    if (ssid.isEmpty) {
      _snack(context.l10n.tr('radio_mode_need_ssid'));
      return;
    }
    setState(() => _radioModeApplying = true);
    final ok = await widget.ble.switchToWifiSta(ssid: ssid, pass: _wifiPassController.text);
    if (ok) {
      unawaited(WifiStaPrefs.save(
        nodeIdHex: _nodeIdForClipboard(_nodeIdLive),
        ssid: ssid,
        pass: _wifiPassController.text,
      ));
      if (!widget.ble.isConnected) {
        if (mounted) {
          setState(() => _radioModeApplying = false);
          await appResetTo(
            context,
            ScanScreen(
              initialMessage: context.l10n.tr('device_switched_wifi_scan'),
              initialSnackKind: AppSnackKind.neutral,
            ),
          );
        }
        return;
      }
      for (var i = 0; i < 12; i++) {
        await Future<void>.delayed(const Duration(milliseconds: 200));
        if (!widget.ble.isConnected) {
          if (mounted) {
            setState(() => _radioModeApplying = false);
            await appResetTo(
              context,
              ScanScreen(
                initialMessage: context.l10n.tr('device_switched_wifi_scan'),
                initialSnackKind: AppSnackKind.neutral,
              ),
            );
          }
          return;
        }
        await widget.ble.requestDeviceSync(DeviceSyncReason.userRefresh, force: true);
        final li = widget.ble.lastInfo;
        if (li != null && li.radioMode == 'wifi' && li.radioVariant == 'sta') {
          if (mounted) {
            _snack(context.l10n.tr('radio_mode_switched', {'mode': context.l10n.tr('radio_mode_wifi_sta')}));
          }
          if (mounted) setState(() => _radioModeApplying = false);
          return;
        }
      }
      if (mounted) {
        _snack(context.l10n.tr('radio_mode_wifi_switch_pending'));
      }
    } else {
      if (mounted) _snack(context.l10n.tr('radio_mode_failed'));
    }
    if (mounted) setState(() => _radioModeApplying = false);
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final regions = ['EU', 'RU', 'UK', 'US', 'AU'];
    final isEu = _region == 'EU' || _region == 'UK';
    final connected = widget.ble.isConnected;
    final linkUp = widget.ble.isTransportConnected;
    final usingWifiTransport = widget.ble.isWifiMode;
    final idPlain = _nodeIdForClipboard(_nodeIdLive);
    final idShown = _formatNodeIdDisplay(_nodeIdLive);
    final nodeLabel = _nickController.text.trim().isNotEmpty
        ? _nickController.text.trim()
        : ContactsService.displayNodeLabel(_nodeIdLive, _nickById);
    final isNicknameLabel = nodeLabel != _nodeIdForClipboard(_nodeIdLive);

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
            padding: const EdgeInsets.fromLTRB(
              AppSpacing.lg,
              AppSpacing.sm,
              AppSpacing.lg,
              AppSpacing.xxl + AppSpacing.sm,
            ),
            children: [
              _SettingsCard(
                title: l.tr('settings_device'),
                subtitle: l.tr('settings_node_id_hint'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Text(
                      l.tr('settings_node_id'),
                      style: AppTypography.labelBase().copyWith(color: context.palette.onSurfaceVariant),
                    ),
                    const SizedBox(height: AppSpacing.sm),
                    SelectableText(
                      nodeLabel.isNotEmpty ? nodeLabel : idShown,
                      style: (isNicknameLabel ? AppTypography.bodyBase() : AppTypography.monoBase()).copyWith(
                        color: context.palette.onSurface,
                        height: 1.35,
                        letterSpacing: 0.5,
                      ),
                    ),
                    const SizedBox(height: AppSpacing.md),
                    AppSecondaryButton(
                      fullWidth: true,
                      onPressed: idPlain.isEmpty
                          ? null
                          : () async {
                              await Clipboard.setData(ClipboardData(text: idPlain));
                              if (mounted) _snack(l.tr('copied'));
                            },
                      child: Row(
                        mainAxisAlignment: MainAxisAlignment.center,
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          const Icon(Icons.copy, size: AppIconSize.sm),
                          SizedBox(width: AppSpacing.sm),
                          Text(l.tr('copy')),
                        ],
                      ),
                    ),
                  ],
                ),
              ),
              const SizedBox(height: AppSpacing.md),
              _SettingsCard(
                title: l.tr('nickname'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    TextField(
                      controller: _nickController,
                      focusNode: _nickFocus,
                      maxLength: 16,
                      style: TextStyle(color: context.palette.onSurface),
                      decoration: InputDecoration(
                        hintText: l.tr('nickname_hint'),
                        counterText: '',
                      ),
                    ),
                    const SizedBox(height: AppSpacing.md),
                    AppPrimaryButton(
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
              const SizedBox(height: AppSpacing.md),
              _SettingsCard(
                title: l.tr('region'),
                subtitle: l.tr('settings_region_hint'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Text(
                      l.tr('region_warning'),
                      style: AppTypography.chipBase().copyWith(color: context.palette.onSurfaceVariant, height: 1.35),
                    ),
                    const SizedBox(height: AppSpacing.md + AppSpacing.xs),
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
                      const SizedBox(height: AppSpacing.lg + AppSpacing.xs),
                      Row(
                        children: [
                          Icon(Icons.radio_button_checked, size: AppIconSize.sm, color: context.palette.primary.withOpacity(0.9)),
                          const SizedBox(width: AppSpacing.sm),
                          Text(
                            l.tr('channel_eu'),
                            style: AppTypography.bodyLargeBase().copyWith(
                              color: context.palette.onSurface,
                              fontWeight: FontWeight.w600,
                              letterSpacing: 0.2,
                            ),
                          ),
                        ],
                      ),
                      const SizedBox(height: AppSpacing.sm + 2),
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
              const SizedBox(height: AppSpacing.md),
              _SettingsCard(
                title: l.tr('settings_modem'),
                subtitle: l.tr('settings_modem_hint'),
                child: _ModemSection(
                  modemPreset: _modemPreset,
                  sf: _sf,
                  bw: _bw,
                  cr: _cr,
                  enabled: connected && !_modemApplying,
                  onPreset: (p) async {
                    if (!mounted || _modemApplying) return;
                    setState(() => _modemApplying = true);
                    var sent = false;
                    for (var i = 0; i < 2 && !sent; i++) {
                      sent = await widget.ble.setModemPreset(p);
                      if (!sent) await Future<void>.delayed(const Duration(milliseconds: 120));
                    }
                    if (sent) {
                      final applied = await _waitForModemApply(preset: p);
                      if (mounted) {
                        _snack(applied ? l.tr('saved') : l.tr('modem_apply_failed'));
                      }
                    } else {
                      if (mounted) _snack(l.tr('error'));
                    }
                    if (mounted) setState(() => _modemApplying = false);
                  },
                  onCustom: (sf, bw, cr) async {
                    if (!mounted || _modemApplying) return;
                    setState(() => _modemApplying = true);
                    var sent = false;
                    for (var i = 0; i < 2 && !sent; i++) {
                      sent = await widget.ble.setCustomModem(sf, bw, cr);
                      if (!sent) await Future<void>.delayed(const Duration(milliseconds: 120));
                    }
                    if (sent) {
                      final applied = await _waitForModemApply(
                        preset: 4,
                        sf: sf,
                        bw: bw,
                        cr: cr,
                      );
                      if (applied) {
                        setState(() {
                          _sf = sf;
                          _bw = bw;
                          _cr = cr;
                          _modemPreset = 4;
                        });
                        widget.onSfChanged(sf);
                      }
                      if (mounted) _snack(applied ? l.tr('saved') : l.tr('modem_apply_failed'));
                    } else {
                      if (mounted) _snack(l.tr('error'));
                    }
                    if (mounted) setState(() => _modemApplying = false);
                  },
                ),
              ),
              if (_modemApplying)
                const Padding(
                  padding: EdgeInsets.only(top: AppSpacing.sm - 2),
                  child: LinearProgressIndicator(minHeight: 2),
                ),
              const SizedBox(height: AppSpacing.md),
              _SettingsCard(
                title: l.tr('connection'),
                subtitle: l.tr('settings_connection_hint'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Text(
                      l.tr('ble_pin'),
                      style: AppTypography.labelBase().copyWith(color: context.palette.onSurfaceVariant),
                    ),
                    const SizedBox(height: AppSpacing.sm - 2),
                    Container(
                      padding: const EdgeInsets.symmetric(
                        horizontal: AppSpacing.md,
                        vertical: AppSpacing.sm + 2,
                      ),
                      decoration: BoxDecoration(
                        color: context.palette.surfaceVariant,
                        borderRadius: BorderRadius.circular(AppRadius.md),
                        border: Border.all(color: context.palette.divider),
                      ),
                      child: Text(
                        _blePin != null ? _blePin.toString() : '—',
                        style: TextStyle(
                          fontFamily: 'monospace',
                          color: context.palette.onSurface,
                          fontWeight: FontWeight.w600,
                          fontFeatures: const [FontFeature.tabularFigures()],
                        ),
                      ),
                    ),
                    const SizedBox(height: AppSpacing.sm + 2),
                    FilledButton.tonalIcon(
                      onPressed: linkUp
                          ? () async {
                              final ok = await widget.ble.regeneratePin();
                              if (ok) await widget.ble.requestDeviceSync(DeviceSyncReason.postSettingsChange);
                              if (mounted) _snack(ok ? l.tr('saved') : l.tr('error'));
                            }
                          : null,
                      icon: const Icon(Icons.lock_reset_rounded, size: 18),
                      label: Text(l.tr('regen_pin')),
                    ),
                  ],
                ),
              ),
              const SizedBox(height: AppSpacing.md),
              _SettingsCard(
                title: l.tr('radio_mode_title'),
                subtitle: l.tr('radio_mode_hint'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Row(
                      children: [
                        Icon(
                          _radioMode == 'wifi' ? Icons.wifi_rounded : Icons.bluetooth_rounded,
                          size: 18,
                          color: context.palette.primary,
                        ),
                        const SizedBox(width: AppSpacing.sm),
                        Text(
                          '${l.tr('radio_mode_current')}: ${_radioModeLabel(l, _radioMode)}',
                          style: TextStyle(
                            color: context.palette.onSurface,
                            fontWeight: FontWeight.w600,
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: AppSpacing.md),
                    _SegmentedPickBar(
                      leadingIcon: Icons.swap_horiz_rounded,
                      labels: [
                        l.tr('radio_mode_ble'),
                        l.tr('radio_mode_wifi_sta'),
                      ],
                      selectedIndex: _radioMode == 'wifi' ? 1 : 0,
                      enabled: linkUp && !_radioModeApplying,
                      onSelected: (i) async {
                        if (i == 0) {
                          await _switchRadioToBle();
                          return;
                        }
                        await _switchRadioToWifiSta();
                      },
                    ),
                    if (_radioMode == 'wifi') ...[
                      const SizedBox(height: AppSpacing.sm + 2),
                      Container(
                        padding: const EdgeInsets.all(AppSpacing.sm + 2),
                        decoration: BoxDecoration(
                          color: context.palette.surfaceVariant,
                          borderRadius: BorderRadius.circular(AppRadius.md),
                          border: Border.all(color: context.palette.divider),
                        ),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text(
                              _wifiConnected
                                  ? l.tr('radio_mode_wifi_connected')
                                  : l.tr('radio_mode_wifi_not_connected'),
                              style: TextStyle(
                                color: _wifiConnected ? context.palette.success : context.palette.onSurfaceVariant,
                                fontWeight: FontWeight.w600,
                              ),
                            ),
                            if (_wifiSsid != null && _wifiSsid!.isNotEmpty) ...[
                              const SizedBox(height: AppSpacing.xs),
                              Text(
                                'SSID: $_wifiSsid',
                                style: AppTypography.chipBase().copyWith(color: context.palette.onSurfaceVariant),
                              ),
                            ],
                            if (_wifiIp != null && _wifiIp!.isNotEmpty) ...[
                              const SizedBox(height: AppSpacing.xs / 2),
                              Text(
                                'IP: $_wifiIp',
                                style: AppTypography.chipBase().copyWith(color: context.palette.onSurfaceVariant),
                              ),
                            ],
                          ],
                        ),
                      ),
                    ],
                    const SizedBox(height: AppSpacing.md),
                    TextField(
                      controller: _wifiSsidController,
                      style: TextStyle(color: context.palette.onSurface),
                      decoration: InputDecoration(labelText: l.tr('wifi_ssid')),
                    ),
                    const SizedBox(height: AppSpacing.sm + 2),
                    TextField(
                      controller: _wifiPassController,
                      obscureText: true,
                      style: TextStyle(color: context.palette.onSurface),
                      decoration: InputDecoration(labelText: l.tr('wifi_password')),
                    ),
                    const SizedBox(height: AppSpacing.md),
                    FilledButton.icon(
                      onPressed: linkUp && !_radioModeApplying ? _switchRadioToWifiSta : null,
                      icon: const Icon(Icons.wifi_find_rounded),
                      label: Text(l.tr('radio_mode_connect_sta')),
                    ),
                    if (_radioModeApplying)
                      const Padding(
                        padding: EdgeInsets.only(top: AppSpacing.sm + 2),
                        child: LinearProgressIndicator(minHeight: 2),
                      ),
                  ],
                ),
              ),
              const SizedBox(height: AppSpacing.md),
              if (_radioMode == 'wifi') ...[
                _SettingsCard(
                  title: l.tr('espnow_section'),
                  subtitle: l.tr('espnow_section_hint'),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      DropdownButtonFormField<int>(
                        value: _espNowChannel,
                        isDense: true,
                        decoration: InputDecoration(labelText: l.tr('espnow_channel')),
                        items: List.generate(
                          13,
                          (i) => DropdownMenuItem<int>(
                            value: i + 1,
                            child: Text('${i + 1}'),
                          ),
                        ),
                        onChanged: linkUp
                            ? (v) async {
                                if (v == null) return;
                                if (await widget.ble.setEspNowChannel(v)) {
                                  setState(() => _espNowChannel = v);
                                  widget.ble.requestDeviceSync(DeviceSyncReason.screenVisible);
                                }
                              }
                            : null,
                      ),
                      const SizedBox(height: AppSpacing.sm),
                      SwitchListTile(
                        contentPadding: EdgeInsets.zero,
                        title: Text(
                          l.tr('espnow_adaptive'),
                          style: TextStyle(color: context.palette.onSurface),
                        ),
                        value: _espNowAdaptive,
                        activeThumbColor: context.palette.primary,
                        activeTrackColor: context.palette.primary.withOpacity(0.45),
                        onChanged: linkUp
                            ? (v) async {
                                if (await widget.ble.setEspNowAdaptive(v)) {
                                  setState(() => _espNowAdaptive = v);
                                  widget.ble.requestDeviceSync(DeviceSyncReason.screenVisible);
                                }
                              }
                            : null,
                      ),
                    ],
                  ),
                ),
                const SizedBox(height: AppSpacing.md),
              ],
              _SettingsCard(
                title: l.tr('firmware_update_title'),
                subtitle: l.tr('firmware_update_hint'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    if (!usingWifiTransport) ...[
                      FilledButton.icon(
                        onPressed: linkUp
                            ? () => showOtaDialog(context, widget.ble)
                            : null,
                        icon: const Icon(Icons.bluetooth_searching_rounded),
                        label: Text(l.tr('firmware_update_ble')),
                      ),
                      const SizedBox(height: AppSpacing.sm + 2),
                      Text(
                        l.tr('firmware_update_where_hint'),
                        style: AppTypography.chipBase().copyWith(color: context.palette.onSurfaceVariant),
                      ),
                      const SizedBox(height: AppSpacing.sm - 2),
                      Text(
                        l.tr('firmware_update_path'),
                        style: AppTypography.monoBase().copyWith(
                          color: context.palette.onSurfaceVariant.withOpacity(0.95),
                        ),
                      ),
                    ] else ...[
                      Text(
                        l.tr('firmware_update_wifi'),
                        style: AppTypography.labelBase().copyWith(
                          fontWeight: FontWeight.w600,
                          color: context.palette.onSurface,
                        ),
                      ),
                      const SizedBox(height: AppSpacing.sm),
                      Text(
                        l.tr('firmware_update_where_hint'),
                        style: AppTypography.chipBase().copyWith(color: context.palette.onSurfaceVariant),
                      ),
                      const SizedBox(height: AppSpacing.sm - 2),
                      Text(
                        l.tr('firmware_update_path'),
                        style: AppTypography.monoBase().copyWith(
                          color: context.palette.onSurfaceVariant.withOpacity(0.95),
                        ),
                      ),
                    ],
                    if (!usingWifiTransport && _radioMode != 'wifi') ...[
                      const SizedBox(height: AppSpacing.md),
                      Row(
                        children: [
                          Icon(Icons.info_outline_rounded, size: AppIconSize.compact, color: context.palette.onSurfaceVariant),
                          const SizedBox(width: AppSpacing.sm - 2),
                          Expanded(
                            child: Text(
                              l.tr('firmware_update_wifi_requires_wifi_mode'),
                              style: AppTypography.chipBase().copyWith(color: context.palette.onSurfaceVariant),
                            ),
                          ),
                        ],
                      ),
                    ],
                    if (usingWifiTransport || _radioMode == 'wifi') ...[
                      const SizedBox(height: AppSpacing.md),
                      Row(
                        children: [
                          Icon(Icons.check_circle_outline_rounded, size: 16, color: context.palette.success),
                          const SizedBox(width: AppSpacing.sm - 2),
                          Expanded(
                            child: Text(
                              l.tr('firmware_update_wifi_mode_ready'),
                              style: AppTypography.chipBase().copyWith(color: context.palette.onSurfaceVariant),
                            ),
                          ),
                        ],
                      ),
                    ],
                    const SizedBox(height: AppSpacing.md),
                    AppSecondaryButton(
                      fullWidth: true,
                      onPressed: linkUp && !_radioModeApplying && _radioMode != 'ble'
                          ? _switchRadioToBle
                          : null,
                      child: Row(
                        mainAxisAlignment: MainAxisAlignment.center,
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          const Icon(Icons.bluetooth_rounded),
                          SizedBox(width: AppSpacing.sm),
                          Text(l.tr('radio_mode_back_to_ble')),
                        ],
                      ),
                    ),
                  ],
                ),
              ),
              if (_gpsPresent) ...[
                const SizedBox(height: AppSpacing.md),
                _SettingsCard(
                  title: l.tr('gps_section'),
                  child: SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    title: Text(l.tr('gps_enable'), style: TextStyle(color: context.palette.onSurface)),
                    subtitle: Text(
                      _gpsFix ? l.tr('gps_fix_yes') : l.tr('gps_fix_no'),
                      style: AppTypography.labelBase().copyWith(color: context.palette.onSurfaceVariant),
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
              const SizedBox(height: AppSpacing.md),
              _SettingsCard(
                title: l.tr('settings_energy_title'),
                subtitle: l.tr('settings_energy_hint'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    _SectionAccentHeader(
                      text: l.tr('settings_energy_node'),
                      trailingIcon: Icons.memory_rounded,
                    ),
                    const SizedBox(height: AppSpacing.sm + 2),
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
                    const SizedBox(height: AppSpacing.xl),
                    _SectionAccentHeader(
                      text: l.tr('settings_energy_app'),
                      trailingIcon: Icons.smartphone_rounded,
                    ),
                    const SizedBox(height: AppSpacing.sm - 2),
                    ListTile(
                      contentPadding: EdgeInsets.zero,
                      leading: Icon(Icons.dark_mode_outlined, color: context.palette.primary),
                      title: Text(l.tr('theme'), style: TextStyle(color: context.palette.onSurface, fontWeight: FontWeight.w500)),
                      subtitle: Text(
                        l.tr('theme_hint'),
                        style: AppTypography.chipBase().copyWith(color: context.palette.onSurfaceVariant, height: 1.35),
                      ),
                      trailing: Icon(Icons.chevron_right, color: context.palette.onSurfaceVariant),
                      onTap: () {
                        HapticFeedback.selectionClick();
                        showThemeModeSheet(context);
                      },
                    ),
                    const SizedBox(height: AppSpacing.xs),
                    SwitchListTile(
                      contentPadding: EdgeInsets.zero,
                      title: Text(
                        l.tr('mesh_animation'),
                        style: TextStyle(color: context.palette.onSurface, fontWeight: FontWeight.w500),
                      ),
                      subtitle: Text(
                        l.tr('mesh_animation_hint'),
                        style: AppTypography.chipBase().copyWith(color: context.palette.onSurfaceVariant, height: 1.35),
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
              const SizedBox(height: AppSpacing.md),
              _SettingsCard(
                title: l.tr('e2e_invite'),
                subtitle: l.tr('e2e_invite_hint'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    OutlinedButton.icon(
                      style: OutlinedButton.styleFrom(
                        foregroundColor: context.palette.primary,
                        side: BorderSide(color: context.palette.primary, width: 1.2),
                        padding: const EdgeInsets.symmetric(
                          vertical: AppSpacing.buttonPrimaryV,
                          horizontal: AppSpacing.lg,
                        ),
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(AppRadius.md),
                        ),
                      ),
                      onPressed: connected
                          ? () {
                              HapticFeedback.lightImpact();
                              setState(() {
                                _inviteStatusError = false;
                                _inviteStatusText = l.tr('invite_status_creating');
                              });
                              widget.ble.createInvite();
                            }
                          : null,
                      icon: const Icon(Icons.add_moderator_outlined, size: 22),
                      label: Text(l.tr('create_invite'), style: const TextStyle(fontWeight: FontWeight.w600)),
                    ),
                    SizedBox(height: AppSpacing.xl + AppSpacing.xs),
                    _SectionAccentHeader(text: l.tr('invite_accept_section')),
                    const SizedBox(height: AppSpacing.md),
                    Container(
                      padding: const EdgeInsets.all(AppSpacing.md + AppSpacing.xs),
                      decoration: BoxDecoration(
                        color: context.palette.surfaceVariant.withOpacity(0.65),
                        borderRadius: BorderRadius.circular(AppRadius.md),
                        border: Border.all(color: context.palette.divider),
                      ),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        children: [
                          TextField(
                            controller: _inviteIdController,
                            style: AppTypography.monoBase().copyWith(color: context.palette.onSurface),
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
                                      final tok = (m['inviteToken'] as String?) ?? '';
                                      final ttlMs = (m['inviteTtlMs'] as num?)?.toInt();
                                      _inviteIdController.text = id.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '');
                                      _inviteKeyController.text = pk;
                                      _inviteChannelKeyController.text = ck;
                                      _inviteTokenController.text = tok.replaceAll(RegExp(r'[^0-9A-Fa-f]'), '');
                                      if (ttlMs != null && mounted) {
                                        _snack(l.tr('invite_ttl', {'sec': '${(ttlMs / 1000).round()}'}));
                                      }
                                      if (mounted) setState(() {});
                                    }
                                  } catch (_) {}
                                },
                              ),
                            ),
                            maxLength: 16,
                          ),
                          const SizedBox(height: AppSpacing.md),
                          TextField(
                            controller: _inviteKeyController,
                            style: AppTypography.labelBase().copyWith(color: context.palette.onSurface, height: 1.35),
                            decoration: InputDecoration(
                              isDense: true,
                              labelText: l.tr('invite_pubkey'),
                              alignLabelWithHint: true,
                            ),
                            maxLines: 3,
                            minLines: 2,
                          ),
                          const SizedBox(height: AppSpacing.md),
                          TextField(
                            controller: _inviteChannelKeyController,
                            style: AppTypography.labelBase().copyWith(color: context.palette.onSurface, height: 1.35),
                            decoration: InputDecoration(
                              isDense: true,
                              labelText: l.tr('invite_channel_key'),
                              alignLabelWithHint: true,
                            ),
                            maxLines: 2,
                            minLines: 1,
                          ),
                          const SizedBox(height: AppSpacing.md),
                          TextField(
                            controller: _inviteTokenController,
                            style: AppTypography.monoBase().copyWith(color: context.palette.onSurface),
                            decoration: InputDecoration(
                              isDense: true,
                              labelText: l.tr('invite_token_optional'),
                            ),
                            maxLength: 16,
                          ),
                        ],
                      ),
                    ),
                    const SizedBox(height: AppSpacing.md + AppSpacing.xs),
                    FilledButton.icon(
                      style: FilledButton.styleFrom(
                        padding: const EdgeInsets.symmetric(
                          vertical: AppSpacing.buttonPrimaryV,
                          horizontal: AppSpacing.buttonPrimaryH - 2,
                        ),
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(AppRadius.md),
                        ),
                      ),
                      onPressed: connected
                          ? () async {
                              HapticFeedback.lightImpact();
                              final id = _inviteIdController.text.trim().replaceAll(RegExp(r'[^0-9A-Fa-f]'), '');
                              final key = _inviteKeyController.text.trim();
                              final ck = _inviteChannelKeyController.text.trim();
                              final token = _inviteTokenController.text.trim().replaceAll(RegExp(r'[^0-9A-Fa-f]'), '');
                              if (id.length != 16 || key.isEmpty) return;
                              final tokenErr = _validateInviteToken(token);
                              if (tokenErr != null) {
                                setState(() {
                                  _inviteStatusError = true;
                                  _inviteStatusText = tokenErr;
                                });
                                _snack(tokenErr);
                                return;
                              }
                              setState(() {
                                _inviteStatusError = false;
                                _inviteStatusText = l.tr('invite_status_handshake_pending');
                              });
                              if (await widget.ble.acceptInvite(
                                id: id,
                                pubKey: key,
                                channelKey: ck.isEmpty ? null : ck,
                                inviteToken: token.isEmpty ? null : token,
                              )) {
                                if (mounted) {
                                  setState(() {
                                    _inviteStatusError = false;
                                    _inviteStatusText = l.tr('invite_status_accepted_wait_key');
                                    _invitePendingPeerId = id.substring(0, id.length > 16 ? 16 : id.length);
                                  });
                                  _snack(l.tr('invite_accepted'));
                                }
                              }
                            }
                          : null,
                      icon: const Icon(Icons.check_circle_outline_rounded, size: 22),
                      label: Text(l.tr('accept_invite'), style: const TextStyle(fontWeight: FontWeight.w600)),
                    ),
                    if (_inviteStatusText != null && _inviteStatusText!.isNotEmpty) ...[
                      const SizedBox(height: AppSpacing.sm + 2),
                      AppStateChip(
                        label: _inviteStatusText!,
                        kind: _inviteStatusError ? AppStateKind.error : AppStateKind.success,
                      ),
                    ],
                  ],
                ),
              ),
              const SizedBox(height: AppSpacing.md),
              _SettingsCard(
                title: l.tr('other'),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Text(
                      l.tr('voice_acceptance_title'),
                      style: TextStyle(
                        color: context.palette.onSurface,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                    const SizedBox(height: AppSpacing.sm - 2),
                    Text(
                      l.tr('voice_acceptance_hint'),
                      style: AppTypography.chipBase().copyWith(color: context.palette.onSurfaceVariant),
                    ),
                    const SizedBox(height: AppSpacing.sm + 2),
                    TextField(
                      controller: _voiceAcceptAvgLossController,
                      keyboardType: TextInputType.number,
                      inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                      decoration: InputDecoration(
                        isDense: true,
                        labelText: l.tr('voice_acceptance_avg_loss'),
                      ),
                    ),
                    const SizedBox(height: AppSpacing.sm),
                    TextField(
                      controller: _voiceAcceptHardLossController,
                      keyboardType: TextInputType.number,
                      inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                      decoration: InputDecoration(
                        isDense: true,
                        labelText: l.tr('voice_acceptance_hard_loss'),
                      ),
                    ),
                    const SizedBox(height: AppSpacing.sm),
                    TextField(
                      controller: _voiceAcceptMinSessionsController,
                      keyboardType: TextInputType.number,
                      inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                      decoration: InputDecoration(
                        isDense: true,
                        labelText: l.tr('voice_acceptance_min_sessions'),
                      ),
                    ),
                    const SizedBox(height: AppSpacing.sm + 2),
                    FilledButton.tonalIcon(
                      onPressed: _voiceAcceptanceSaving ? null : _saveVoiceAcceptancePrefs,
                      icon: _voiceAcceptanceSaving
                          ? SizedBox(
                              width: 14,
                              height: 14,
                              child: CircularProgressIndicator(
                                strokeWidth: 2,
                                color: context.palette.primary,
                              ),
                            )
                          : const Icon(Icons.tune_rounded, size: 18),
                      label: Text(l.tr('voice_acceptance_apply')),
                    ),
                    const SizedBox(height: AppSpacing.md),
                    if (_offlinePending != null && _offlinePending! > 0)
                      ListTile(
                        contentPadding: EdgeInsets.zero,
                        leading: Icon(Icons.schedule, color: context.palette.onSurfaceVariant),
                        title: Text(
                          '${l.tr('offline_pending')}: $_offlinePending',
                          style: TextStyle(color: context.palette.onSurface),
                        ),
                      ),
                    if (_offlineCourierPending != null && _offlineCourierPending! > 0)
                      ListTile(
                        contentPadding: EdgeInsets.zero,
                        leading: Icon(Icons.local_shipping_outlined, color: context.palette.onSurfaceVariant),
                        title: Text(
                          '${l.tr('scf_courier_status_count', {'n': '$_offlineCourierPending'})}',
                          style: TextStyle(color: context.palette.onSurface),
                        ),
                      ),
                    if (_offlineDirectPending != null && _offlineDirectPending! > 0)
                      ListTile(
                        contentPadding: EdgeInsets.zero,
                        leading: Icon(Icons.mark_email_unread_outlined, color: context.palette.onSurfaceVariant),
                        title: Text(
                          '${l.tr('offline_direct_status_count', {'n': '$_offlineDirectPending'})}',
                          style: TextStyle(color: context.palette.onSurface),
                        ),
                      ),
                    if (_version != null && _version!.isNotEmpty)
                      ListTile(
                        contentPadding: EdgeInsets.zero,
                        leading: Icon(Icons.info_outline, color: context.palette.onSurfaceVariant),
                        title: Text(
                          '${l.tr('settings_firmware_version')}: $_version',
                          style: TextStyle(color: context.palette.onSurface),
                        ),
                      ),
                    if (_batteryMv != null && _batteryMv! > 0)
                      ListTile(
                        contentPadding: EdgeInsets.zero,
                        leading: Icon(
                          _charging ? Icons.battery_charging_full : Icons.battery_std,
                          color: _batteryPercent != null && _batteryPercent! <= 15
                              ? context.palette.error
                              : context.palette.onSurfaceVariant,
                        ),
                        title: Text(
                          _batteryPercent != null
                              ? '$_batteryPercent% (${(_batteryMv! / 1000).toStringAsFixed(2)} V)${_charging ? ' ⚡' : ''}'
                              : '${(_batteryMv! / 1000).toStringAsFixed(2)} V${_charging ? ' ⚡' : ''}',
                          style: TextStyle(color: context.palette.onSurface),
                        ),
                      ),
                    const Divider(height: 1),
                    ListTile(
                      contentPadding: EdgeInsets.zero,
                      leading: Icon(Icons.power_settings_new, color: context.palette.error),
                      title: Text(l.tr('shutdown_device'), style: TextStyle(color: context.palette.onSurface)),
                      onTap: () async {
                        final confirmed = await showDialog<bool>(
                          context: context,
                          builder: (ctx) => AlertDialog(
                            title: Text(l.tr('shutdown_device')),
                            content: Text(l.tr('shutdown_confirm')),
                            actions: [
                              TextButton(onPressed: () => Navigator.pop(ctx, false), child: Text(l.tr('cancel'))),
                              TextButton(onPressed: () => Navigator.pop(ctx, true), child: Text(l.tr('shutdown_device'))),
                            ],
                          ),
                        );
                        if (confirmed == true) {
                          widget.ble.shutdown();
                        }
                      },
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

/// Заголовок подсекции с акцентной полосой слева (единый ритм с другими экранами DS).
class _SectionAccentHeader extends StatelessWidget {
  final String text;
  final IconData? trailingIcon;

  const _SectionAccentHeader({
    required this.text,
    this.trailingIcon,
  });

  @override
  Widget build(BuildContext context) {
    final p = context.palette;
    return Row(
      children: [
        Container(
          width: 3,
          height: 14,
          decoration: BoxDecoration(
            color: p.primary,
            borderRadius: BorderRadius.circular(2),
          ),
        ),
        const SizedBox(width: AppSpacing.sm + 2),
        Expanded(
          child: Text(
            text,
            style: AppTypography.bodyLargeBase().copyWith(
              fontWeight: FontWeight.w600,
              letterSpacing: 0.2,
              color: p.onSurface,
            ),
          ),
        ),
        if (trailingIcon != null)
          Icon(trailingIcon, size: AppIconSize.sm, color: p.onSurfaceVariant),
      ],
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
    return AppSectionCard(
      padding: const EdgeInsets.fromLTRB(AppSpacing.lg, 14, AppSpacing.lg, AppSpacing.lg),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Text(
            title,
            style: AppTypography.navTitleBase().copyWith(
              fontWeight: FontWeight.w600,
              color: context.palette.onSurface,
              letterSpacing: 0.2,
            ),
          ),
          if (subtitle != null) ...[
            const SizedBox(height: AppSpacing.xs),
            Text(
              subtitle!,
              style: AppTypography.chipBase().copyWith(
                color: context.palette.onSurfaceVariant,
                height: 1.3,
              ),
            ),
          ],
          const SizedBox(height: AppSpacing.sm),
          child,
        ],
      ),
    );
  }
}
