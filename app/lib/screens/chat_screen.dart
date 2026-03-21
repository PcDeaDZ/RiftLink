import 'dart:async';
import 'dart:convert';
import 'dart:ui' show FontFeature;
import 'package:flutter/material.dart';
import 'package:flutter/cupertino.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:geolocator/geolocator.dart';
import '../ble/riftlink_ble.dart';
import '../voice/voice_service.dart';
import '../contacts/contacts_service.dart';
import '../recent_devices/recent_devices_service.dart';
import '../prefs/mesh_prefs.dart';
import '../app_navigator.dart';
import '../l10n/app_localizations.dart';
import 'map_screen.dart';
import 'mesh_screen.dart';
import '../notifications/local_notifications_service.dart';
import 'contacts_groups_hub_screen.dart';
import 'settings_screen.dart';
import 'scan_screen.dart';
import '../locale_notifier.dart';

import '../theme/app_theme.dart';
import '../mesh_constants.dart';
import '../widgets/mesh_background.dart';
import '../widgets/app_snackbar.dart';
import '../widgets/app_popover_menu.dart';
import '../widgets/rift_dialogs.dart';

List<int> _filterUserGroups(List<int> raw) =>
    raw.where((g) => g != kMeshBroadcastGroupId).toList();

class ChatScreen extends StatefulWidget {
  final RiftLinkBle ble;
  const ChatScreen({super.key, required this.ble});
  @override
  State<ChatScreen> createState() => _ChatScreenState();
}

class _ChatScreenState extends State<ChatScreen> with TickerProviderStateMixin, WidgetsBindingObserver {
  final _controller = TextEditingController();
  final _scrollController = ScrollController();
  final _messages = <_Msg>[];
  StreamSubscription<RiftLinkEvent>? _sub;
  String _nodeId = '';
  String? _nickname;
  /// Пусто, пока не пришёл evt info (избегаем ложного EU в настройках).
  String _region = '';
  int? _channel;
  String? _version;
  int? _sf;
  int? _offlinePending;
  bool _gpsPresent = false;
  bool _gpsEnabled = false;
  bool _gpsFix = false;
  bool _powersave = false;
  int? _batteryMv;
  int? _batteryPercent;
  bool _charging = false;
  List<String> _neighbors = [];
  List<int> _neighborsRssi = [];
  List<bool> _neighborsHasKey = [];  // true = можно отправить
  List<Map<String, dynamic>> _routes = [];
  List<int> _groups = [];
  Map<String, String> _contactNicknames = {};
  int _group = 0;
  String? _unicastTo;
  bool _locationLoading = false;
  bool _voiceRecording = false;
  int _voiceTtlMinutes = 0;  // TTL для текущей записи (выбирается до старта)
  DateTime? _voiceRecordStartTime;
  Ticker? _voiceRecordTicker;
  bool _hasText = false;
  final Map<String, Map<int, String>> _voiceChunks = {};
  final Set<String> _readSent = {};
  final Set<String> _pendingPings = {};
  Timer? _ttlTimer;
  Timer? _neighborsPollTimer;
  Timer? _gpsSyncTimer;
  bool _meshAnimationEnabled = true;
  AnimationController? _meshAnimController;
  StreamSubscription? _connStateSub;
  bool _reconnecting = false;
  int _reconnectAttempt = 0;
  bool _intentionalDisconnect = false;
  String? _currentBleRemoteId;
  List<RecentDevice> _recentDevices = [];
  AppLifecycleState _appLifecycle = AppLifecycleState.resumed;

  List<_Msg> get _visibleMessages =>
      _messages.where((m) => m.deleteAt == null || DateTime.now().isBefore(m.deleteAt!)).toList();

  // ── Lifecycle ──

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _controller.addListener(_onTextChanged);
    _listenEvents();
    _loadContactNicknames();
    _loadMeshPrefs();
    localeNotifier.addListener(_onLocaleChanged);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _sendReadForUnread();
      _sendLangToFirmware();
      VoiceService.requestPermission();
      if (widget.ble.isConnected) {
        _currentBleRemoteId = widget.ble.device?.remoteId.toString();
        _listenConnectionState();
        _applyNodeIdFromDeviceName();
        final cachedInfo = widget.ble.lastInfo;
        if (cachedInfo != null) _onInfoEvent(cachedInfo);
        if (mounted && widget.ble.isConnected) widget.ble.getInfo();
        if (cachedInfo == null) {
          Future.delayed(const Duration(milliseconds: 450), () {
            if (mounted && widget.ble.isConnected && widget.ble.lastInfo == null) {
              widget.ble.getInfo();
            }
          });
        }
      }
    });
    _ttlTimer = Timer.periodic(const Duration(minutes: 1), (_) {
      if (mounted && _messages.any((m) => m.deleteAt != null && DateTime.now().isAfter(m.deleteAt!))) setState(() {});
    });
    // Периодический getInfo: пустые соседи — discovery; есть без ключа — обновить hasKey после KEY_EXCHANGE
    _neighborsPollTimer = Timer.periodic(const Duration(seconds: 15), (_) {
      if (!mounted || !widget.ble.isConnected) return;
      final needRefresh = _neighbors.isEmpty ||
          _neighborsHasKey.length != _neighbors.length ||
          _neighborsHasKey.any((k) => !k);
      if (needRefresh) widget.ble.getInfo();
    });
    // GPS sync от телефона: UTC + координаты для beacon-sync (устройство без GPS)
    _gpsSyncTimer = Timer.periodic(const Duration(seconds: 15), (_) => _sendGpsSyncFromPhone());
  }

  void _onTextChanged() {
    final hasText = _controller.text.trim().isNotEmpty;
    if (hasText != _hasText && mounted) setState(() => _hasText = hasText);
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

  void _onMeshAnimationChanged(bool enabled) async {
    await MeshPrefs.setAnimationEnabled(enabled);
    if (mounted) {
      setState(() => _meshAnimationEnabled = enabled);
      _updateMeshAnimation(enabled);
    }
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _connStateSub?.cancel();
    _controller.removeListener(_onTextChanged);
    localeNotifier.removeListener(_onLocaleChanged);
    _ttlTimer?.cancel();
    _neighborsPollTimer?.cancel();
    _gpsSyncTimer?.cancel();
    _voiceRecordTicker?.dispose();
    _meshAnimController?.dispose();
    _sub?.cancel();
    _sub = null;
    _controller.dispose();
    _scrollController.dispose();
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    _appLifecycle = state;
  }

  void _listenConnectionState() {
    final dev = widget.ble.device;
    if (dev == null) return;
    _connStateSub?.cancel();
    _connStateSub = dev.connectionState.listen((state) {
      if (!mounted || _intentionalDisconnect) return;
      if (state == BluetoothConnectionState.disconnected) {
        // When node switches to Wi-Fi transport, BLE disconnect (often status=133 on Android)
        // is expected and should not trigger BLE reconnect flow.
        if (widget.ble.isWifiMode) return;
        _onConnectionLost();
      }
    });
  }

  Future<void> _onConnectionLost() async {
    if (widget.ble.isWifiMode) return;
    if (_reconnecting || !mounted) return;
    final remoteId = _currentBleRemoteId ?? widget.ble.device?.remoteId.toString();
    if (remoteId == null || remoteId.isEmpty) return;
    setState(() { _reconnecting = true; _reconnectAttempt = 1; });
    final l = context.l10n;
    for (var attempt = 1; attempt <= 3; attempt++) {
      if (!mounted) return;
      setState(() => _reconnectAttempt = attempt);
      _showSnack(l.tr('reconnecting', {'n': '$attempt'}), duration: const Duration(seconds: 2));
      try {
        widget.ble.disconnect();
        await Future<void>.delayed(const Duration(milliseconds: 500));
        final device = BluetoothDevice.fromId(remoteId);
        final ok = await widget.ble.connect(device);
        if (mounted && ok) {
          _currentBleRemoteId = remoteId;
          _listenConnectionState();
          _listenEvents(); // новый BLE-поток после connect(), иначе самотест/ping/info не приходят
          widget.ble.getInfo();
          setState(() => _reconnecting = false);
          _showSnack(l.tr('reconnect_ok'), backgroundColor: context.palette.success);
          return;
        }
      } catch (_) {}
      if (attempt < 3) await Future<void>.delayed(const Duration(seconds: 2));
    }
    if (!mounted) return;
    setState(() => _reconnecting = false);
    await widget.ble.disconnect();
    Navigator.of(context).pushAndRemoveUntil(
      MaterialPageRoute(builder: (_) => ScanScreen(initialMessage: l.tr('reconnect_failed'))),
      (r) => false,
    );
  }

  Future<void> _showConnectMenu() async {
    if (!widget.ble.isConnected) return;
    _recentDevices = await RecentDevicesService.load();
    final currentRemoteId = widget.ble.device?.remoteId.toString();
    final others = _recentDevices
        .where((d) => currentRemoteId == null || !RiftLinkBle.remoteIdsMatch(d.remoteId, currentRemoteId))
        .toList();
    final l = context.l10n;
    FocusScope.of(context).unfocus();
    final value = await showAppModalBottomSheet<String>(
      context: context,
      backgroundColor: context.palette.card,
      shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(16))),
      builder: (ctx) => SafeArea(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Padding(
              padding: const EdgeInsets.all(16),
              child: Text(l.tr('switch_node'), style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: context.palette.onSurface)),
            ),
            ...others.map((d) => _buildSwitchItem(ctx, d)),
            ListTile(
              leading: Icon(Icons.link_off, color: context.palette.error),
              title: Text(l.tr('disconnect'), style: TextStyle(color: context.palette.error, fontWeight: FontWeight.w500)),
              onTap: () => Navigator.pop(ctx, 'disconnect'),
            ),
            const SizedBox(height: 8),
          ],
        ),
      ),
    );
    if (value != null && mounted) _onConnectMenuSelected(value);
  }

  Widget _buildSwitchItem(BuildContext ctx, RecentDevice d) {
    final l = context.l10n;
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        ListTile(
          leading: Icon(Icons.bluetooth, color: context.palette.primary),
          title: Text(d.displayName, style: TextStyle(color: context.palette.onSurface, fontWeight: FontWeight.w500)),
          subtitle: d.displayName != d.nodeId ? Text(d.nodeId, style: TextStyle(fontSize: 12, color: context.palette.onSurfaceVariant, fontFamily: 'monospace')) : null,
          trailing: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              IconButton(
                icon: Icon(Icons.delete_outline, size: 20, color: context.palette.onSurfaceVariant),
                tooltip: l.tr('forget_device'),
                onPressed: () => _confirmForgetAndPop(ctx, d),
              ),
              Icon(Icons.chevron_right, color: context.palette.onSurfaceVariant),
            ],
          ),
          onTap: () => Navigator.pop(ctx, 'switch:${d.remoteId}'),
        ),
      ],
    );
  }

  Future<void> _confirmForgetAndPop(BuildContext ctx, RecentDevice d) async {
    final l = context.l10n;
    final confirm = await showRiftConfirmDialog(
      context: ctx,
      title: l.tr('forget_device'),
      message: l.tr('forget_device_confirm', {'name': d.displayName}),
      cancelText: l.tr('cancel'),
      confirmText: l.tr('delete'),
      danger: true,
      icon: Icons.delete_outline_rounded,
    );
    if (confirm == true && mounted) {
      await RecentDevicesService.remove(d.remoteId);
      Navigator.pop(ctx, 'forget:${d.remoteId}');
    }
  }

  void _onConnectMenuSelected(String value) async {
    if (value.startsWith('forget:')) return;
    if (value == 'disconnect') {
      _intentionalDisconnect = true;
      await widget.ble.disconnect();
      if (!mounted) return;
      Navigator.of(context).pushAndRemoveUntil(
        MaterialPageRoute(builder: (_) => const ScanScreen()),
        (r) => false,
      );
    } else if (value.startsWith('switch:')) {
      final remoteId = value.substring(7);
      _intentionalDisconnect = true;
      await _switchToDevice(remoteId);
    }
  }

  Future<void> _switchToDevice(String remoteId) async {
    _intentionalDisconnect = true;
    final l = context.l10n;
    final dev = _recentDevices.where((d) => RiftLinkBle.remoteIdsMatch(d.remoteId, remoteId)).firstOrNull;
    final name = dev?.displayName ?? remoteId;
    _showSnack(l.tr('connecting_to', {'name': name}));
    await widget.ble.disconnect();
    await RiftLinkBle.stopScan();
    // Как на ScanScreen: стек BLE после disconnect + FBP требует, чтобы цель была в scan до connect().
    // BluetoothDevice.fromId без свежего скана часто не подключается ко «второму» узлу.
    await Future<void>.delayed(const Duration(milliseconds: 800));

    if (!mounted) return;
    StreamSubscription? scanSub;
    final foundCompleter = Completer<BluetoothDevice?>();
    scanSub = FlutterBluePlus.scanResults.listen((results) {
      if (foundCompleter.isCompleted) return;
      final r = results.where(RiftLinkBle.isRiftLink).toList();
      for (final r0 in r) {
        if (RiftLinkBle.remoteIdsMatch(r0.device.remoteId.toString(), remoteId)) {
          scanSub?.cancel();
          RiftLinkBle.stopScan();
          if (!foundCompleter.isCompleted) foundCompleter.complete(r0.device);
          return;
        }
      }
    });
    const scanDuration = Duration(seconds: 12);
    try {
      await RiftLinkBle.startScan(timeout: scanDuration);
      await Future.any([
        foundCompleter.future,
        Future<void>.delayed(scanDuration, () {
          if (!foundCompleter.isCompleted) foundCompleter.complete(null);
        }),
      ]);
    } catch (_) {
      if (!foundCompleter.isCompleted) foundCompleter.complete(null);
    }
    await scanSub.cancel();
    await RiftLinkBle.stopScan();
    final found = await foundCompleter.future;
    if (!mounted) return;
    if (found != null) {
      final ok = await widget.ble.connect(found);
      if (mounted && ok) {
        _intentionalDisconnect = false;
        _currentBleRemoteId = found.remoteId.toString();
        _listenConnectionState();
          _listenEvents(); // переподписка после reconnect (await cancel + новый listen)
        setState(_resetStateUntilInfo);
        _applyNodeIdFromDeviceName();
        widget.ble.getInfo();
        _showSnack(l.tr('reconnect_ok'), backgroundColor: context.palette.success);
      } else {
        _intentionalDisconnect = false;
        _showSnack(l.tr('ble_no_service'), backgroundColor: context.palette.error);
      }
    } else {
      _intentionalDisconnect = false;
      _showSnack(l.tr('ble_timeout'), backgroundColor: context.palette.error);
    }
  }

  void _onLocaleChanged() { if (mounted) _sendLangToFirmware(); }

  /// После смены BLE-узла сбрасываем кэш, пока не придёт свежий evt info (иначе в UI «залипает» EU и старый SF).
  void _resetStateUntilInfo() {
    _region = '';
    _channel = null;
    _sf = null;
    _version = null;
    _nodeId = '';
    _nickname = null;
    _offlinePending = null;
    _neighbors = [];
    _neighborsRssi = [];
    _neighborsHasKey = [];
    _routes = [];
    _groups = [];
    _gpsPresent = false;
    _gpsEnabled = false;
    _gpsFix = false;
    _powersave = false;
    _batteryMv = null;
  }

  void _applyNodeIdFromDeviceName() {
    final dev = widget.ble.device;
    if (dev == null || _nodeId.isNotEmpty) return;
    final hint = RiftLinkBle.nodeIdHintFromDevice(dev);
    if (hint != null && mounted) setState(() => _nodeId = hint);
  }
  void _sendLangToFirmware() { if (widget.ble.isConnected) widget.ble.setLang(AppLocalizations.currentLocale.languageCode); }

  Future<void> _loadContactNicknames() async {
    final contacts = await ContactsService.load();
    if (!mounted) return;
    setState(() {
      final m = <String, String>{};
      for (final c in contacts) {
        m[c.id] = c.nickname;
        if (c.id.length >= 8) {
          m[c.id.substring(0, 8).toUpperCase()] = c.nickname;
        }
      }
      _contactNicknames = m;
    });
  }

  String? _nicknameForId(String id) {
    final n = _contactNicknames[id];
    if (n != null && n.isNotEmpty) return n;
    final short = id.length >= 8 ? id.substring(0, 8).toUpperCase() : id.toUpperCase();
    final n2 = _contactNicknames[short];
    if (n2 != null && n2.isNotEmpty) return n2;
    return null;
  }

  String _normId8(String id) =>
      id.length >= 8 ? id.substring(0, 8).toUpperCase() : id.toUpperCase();

  String _recipientPillLabel(AppLocalizations l) {
    if (_group > 0) return 'G$_group';
    if (_unicastTo != null) {
      final id = _unicastTo!;
      final nick = _nicknameForId(id);
      if (nick != null) return nick;
      return _normId8(id);
    }
    return 'BC';
  }

  bool _matchesRecipientQuery(String query, Iterable<String> fields) {
    final q = query.trim().toLowerCase();
    if (q.isEmpty) return true;
    for (final f in fields) {
      if (f.toLowerCase().contains(q)) return true;
    }
    return false;
  }

  Future<void> _showRecipientPickerSheet() async {
    if (!widget.ble.isConnected) return;
    FocusScope.of(context).unfocus();
    final l = context.l10n;
    final contacts = await ContactsService.load();
    if (!mounted) return;
    final neighborNorm = _neighbors.map(_normId8).toSet();
    final contactsOnly = contacts.where((c) => !neighborNorm.contains(_normId8(c.id))).toList();

    await showAppModalBottomSheet<void>(
      context: context,
      backgroundColor: Colors.transparent,
      isScrollControlled: true,
      builder: (ctx) {
        final searchCtrl = TextEditingController();
        var query = '';
        final maxH = MediaQuery.of(context).size.height * 0.62;
        return StatefulBuilder(
          builder: (ctx, setModalState) {
            void apply(void Function() fn) {
              fn();
              setModalState(() {});
            }

            bool rowMatch(String id, String? nick, List<String> extra) {
              return _matchesRecipientQuery(query, [id, if (nick != null && nick.isNotEmpty) nick, ...extra]);
            }

            final bcLabel = l.tr('broadcast');
            final showBc = _matchesRecipientQuery(query, [bcLabel, 'bc', 'broadcast']);

            final groupTiles = <Widget>[];
            for (final gid in _groups) {
              final title = '${l.tr('group')} $gid';
              if (!rowMatch('', null, [title, 'g$gid', '$gid'])) continue;
              final sel = _group == gid && _unicastTo == null;
              groupTiles.add(
                ListTile(
                  leading: Icon(Icons.group, color: sel ? context.palette.primary : context.palette.onSurfaceVariant),
                  title: Text(title, style: TextStyle(fontWeight: sel ? FontWeight.w600 : FontWeight.normal, color: context.palette.onSurface)),
                  trailing: sel ? Icon(Icons.check, color: context.palette.primary, size: 20) : null,
                  onTap: () {
                    Navigator.pop(ctx);
                    setState(() {
                      _unicastTo = null;
                      _group = gid;
                    });
                  },
                ),
              );
            }

            final neighTiles = <Widget>[];
            for (var i = 0; i < _neighbors.length; i++) {
              final id = _neighbors[i];
              final rssi = i < _neighborsRssi.length ? _neighborsRssi[i] : 0;
              final hasKey = i < _neighborsHasKey.length ? _neighborsHasKey[i] : true;
              final nick = _nicknameForId(id);
              if (!rowMatch(id, nick, [if (rssi != 0) '$rssi'])) continue;
              final sel = _unicastTo == id && _group == 0;
              final title = nick ?? _normId8(id);
              final sub = nick != null ? Text(id, style: TextStyle(fontSize: 12, fontFamily: 'monospace', color: context.palette.onSurfaceVariant)) : null;
              neighTiles.add(
                ListTile(
                  leading: Icon(Icons.person_outline, color: hasKey ? (sel ? context.palette.primary : context.palette.onSurfaceVariant) : context.palette.onSurfaceVariant.withOpacity(0.45)),
                  title: Text(
                    hasKey ? title : '$title — ${l.tr('waiting_key')}',
                    style: TextStyle(fontWeight: sel ? FontWeight.w600 : FontWeight.normal, color: context.palette.onSurface),
                  ),
                  subtitle: sub,
                  trailing: sel ? Icon(Icons.check, color: context.palette.primary, size: 20) : (rssi != 0 ? Text('$rssi dBm', style: TextStyle(fontSize: 11, color: context.palette.onSurfaceVariant)) : null),
                  onTap: () {
                    if (!hasKey) {
                      _showSnack(l.tr('waiting_key'));
                      return;
                    }
                    Navigator.pop(ctx);
                    setState(() {
                      _unicastTo = _unicastTo == id ? null : id;
                      _group = 0;
                    });
                  },
                  onLongPress: hasKey ? () => _showAddContactDialog(id) : null,
                ),
              );
            }

            final extraTiles = <Widget>[];
            for (final c in contactsOnly) {
              final id = c.id.length >= 8 ? c.id.substring(0, 8).toUpperCase() : c.id.toUpperCase();
              final nick = c.nickname.trim().isNotEmpty ? c.nickname : null;
              if (!rowMatch(id, nick, [id])) continue;
              final sel = _unicastTo != null && _normId8(_unicastTo!) == id && _group == 0;
              extraTiles.add(
                ListTile(
                  leading: Icon(Icons.bookmark_outline, color: sel ? context.palette.primary : context.palette.onSurfaceVariant),
                  title: Text(nick ?? id, style: TextStyle(fontWeight: sel ? FontWeight.w600 : FontWeight.normal, color: context.palette.onSurface)),
                  subtitle: nick != null ? Text(id, style: TextStyle(fontSize: 12, fontFamily: 'monospace', color: context.palette.onSurfaceVariant)) : null,
                  trailing: sel ? Icon(Icons.check, color: context.palette.primary, size: 20) : null,
                  onTap: () {
                    Navigator.pop(ctx);
                    setState(() {
                      _unicastTo = id;
                      _group = 0;
                    });
                  },
                  onLongPress: () => _showAddContactDialog(id),
                ),
              );
            }

            return Container(
              decoration: BoxDecoration(
                color: context.palette.card,
                borderRadius: const BorderRadius.vertical(top: Radius.circular(16)),
              ),
              child: SizedBox(
                height: maxH,
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                  Padding(
                    padding: const EdgeInsets.fromLTRB(16, 12, 8, 8),
                    child: Row(
                      children: [
                        Expanded(
                          child: Text(l.tr('recipient_title'), style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: context.palette.onSurface)),
                        ),
                        IconButton(icon: Icon(Icons.close), onPressed: () => Navigator.pop(ctx), color: context.palette.onSurfaceVariant),
                      ],
                    ),
                  ),
                  Padding(
                    padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
                    child: TextField(
                      controller: searchCtrl,
                      onChanged: (v) => apply(() => query = v),
                      style: TextStyle(color: context.palette.onSurface),
                      decoration: InputDecoration(
                        isDense: true,
                        prefixIcon: Icon(Icons.search, color: context.palette.onSurfaceVariant, size: 22),
                        hintText: l.tr('recipient_search_hint'),
                        hintStyle: TextStyle(color: context.palette.onSurfaceVariant),
                        filled: true,
                        fillColor: context.palette.surfaceVariant.withOpacity(0.5),
                        border: OutlineInputBorder(borderRadius: BorderRadius.circular(12), borderSide: BorderSide.none),
                        contentPadding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
                      ),
                    ),
                  ),
                  Expanded(
                    child: ListView(
                      children: [
                        if (showBc)
                          ListTile(
                            leading: Icon(Icons.public, color: _unicastTo == null && _group == 0 ? context.palette.primary : context.palette.onSurfaceVariant),
                            title: Text(bcLabel, style: TextStyle(fontWeight: _unicastTo == null && _group == 0 ? FontWeight.w600 : FontWeight.normal, color: context.palette.onSurface)),
                            trailing: _unicastTo == null && _group == 0 ? Icon(Icons.check, color: context.palette.primary, size: 20) : null,
                            onTap: () {
                              Navigator.pop(ctx);
                              setState(() {
                                _unicastTo = null;
                                _group = 0;
                              });
                            },
                          ),
                        if (groupTiles.isNotEmpty) ...[
                          Padding(
                            padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
                            child: Text(l.tr('groups').toUpperCase(), style: TextStyle(fontSize: 11, fontWeight: FontWeight.w600, color: context.palette.onSurfaceVariant)),
                          ),
                          ...groupTiles,
                        ],
                        if (neighTiles.isNotEmpty) ...[
                          Padding(
                            padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
                            child: Text(l.tr('neighbors').toUpperCase(), style: TextStyle(fontSize: 11, fontWeight: FontWeight.w600, color: context.palette.onSurfaceVariant)),
                          ),
                          ...neighTiles,
                        ],
                        if (extraTiles.isNotEmpty) ...[
                          Padding(
                            padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
                            child: Text(l.tr('saved_contacts').toUpperCase(), style: TextStyle(fontSize: 11, fontWeight: FontWeight.w600, color: context.palette.onSurfaceVariant)),
                          ),
                          ...extraTiles,
                        ],
                        if (!showBc && groupTiles.isEmpty && neighTiles.isEmpty && extraTiles.isEmpty)
                          Padding(
                            padding: const EdgeInsets.all(24),
                            child: Text(l.tr('recipient_no_match'), textAlign: TextAlign.center, style: TextStyle(color: context.palette.onSurfaceVariant)),
                          ),
                      ],
                    ),
                  ),
                  SizedBox(height: MediaQuery.of(ctx).padding.bottom + 8),
                ],
                ),
              ),
            );
          },
        );
      },
    );
  }

  void _sendReadForUnread() {
    if (!widget.ble.isConnected) return;
    for (final m in _messages) {
      if (m.isIncoming && m.msgId != null) {
        final key = '${m.from}_${m.msgId}';
        if (!_readSent.contains(key)) { _readSent.add(key); widget.ble.sendRead(from: m.from, msgId: m.msgId!); }
      }
    }
  }

  void _onInfoEvent(RiftLinkInfoEvent evt) {
    final bleDev = widget.ble.device;
    if (bleDev != null) _currentBleRemoteId = bleDev.remoteId.toString();
    var resolvedId = evt.id.isNotEmpty ? evt.id : _nodeId;
    if (resolvedId.isEmpty) {
      resolvedId = RiftLinkBle.nodeIdHintFromDevice(bleDev) ?? '';
    }
    if (resolvedId.isEmpty && bleDev != null) {
      resolvedId = bleDev.remoteId.toString();
    }
    setState(() {
      if (resolvedId.isNotEmpty) _nodeId = resolvedId;
      // Прошивка опускает ключ nickname, если пусто — не затираем прошлый ник в UI.
      if (evt.hasNicknameField) {
        _nickname = evt.nickname?.isNotEmpty == true ? evt.nickname : null;
      }
      _region = evt.region;
      if (evt.hasChannelField) _channel = evt.channel;
      _version = evt.version; _sf = evt.sf;
      if (evt.hasOfflinePendingField) _offlinePending = evt.offlinePending;
      _gpsPresent = evt.gpsPresent; _gpsEnabled = evt.gpsEnabled;
      _gpsFix = evt.gpsFix; _powersave = evt.powersave; _neighbors = evt.neighbors;
      _neighborsRssi = evt.neighborsRssi; _neighborsHasKey = evt.neighborsHasKey; _routes = evt.routes;
      if (evt.batteryMv != null && evt.batteryMv! > 0) _batteryMv = evt.batteryMv;
      _batteryPercent = evt.batteryPercent;
      _charging = evt.charging;
      if (_batteryPercent != null && _batteryPercent! <= 15) {
        LocalNotificationsService.showLowBattery(percent: _batteryPercent!);
      }
      _groups = _filterUserGroups(evt.groups);
      if (_group > 0 && !_groups.contains(_group)) _group = 0;
    });
    if (bleDev != null) {
      RecentDevicesService.addOrUpdate(
        remoteId: bleDev.remoteId.toString(),
        nodeId: resolvedId,
        nickname: evt.nickname?.isNotEmpty == true ? evt.nickname : null,
      );
    }
  }

  // ── BLE Events ──

  /// Синхронная подписка: `async` + `await cancel` откладывал listen — broadcast терял notify до первого кадра.
  void _listenEvents() {
    _sub?.cancel();
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      _handleBleEvent(evt);
    });
    final li = widget.ble.lastInfo;
    if (li != null && mounted) _onInfoEvent(li);
  }

  void _handleBleEvent(RiftLinkEvent evt) {
    if (!mounted) return;
    if (evt is RiftLinkMsgEvent) {
      setState(() {
        final ttl = evt.ttlMinutes ?? 0;
        _messages.add(_Msg(from: evt.from, text: evt.text, isIncoming: true, msgId: evt.msgId, rssi: evt.rssi,
            deleteAt: ttl > 0 ? DateTime.now().add(Duration(minutes: ttl)) : null));
      });
      if (_appLifecycle != AppLifecycleState.resumed) {
        LocalNotificationsService.showIncomingMessage(from: evt.from, text: evt.text);
      }
      _sendReadForUnread();
      _scrollToBottom();
    } else if (evt is RiftLinkSentEvent) {
      setState(() {
        final toMatch = evt.to.isEmpty ? null : evt.to;
        for (var i = _messages.length - 1; i >= 0; i--) {
          final m = _messages[i];
          if (!m.isIncoming && m.msgId == null && m.to == toMatch) {
            _messages[i] = m.copyWith(msgId: evt.msgId, to: evt.to, status: _St.sent);
            break;
          }
        }
      });
    } else if (evt is RiftLinkDeliveredEvent) {
      setState(() { for (var i = 0; i < _messages.length; i++) { final m = _messages[i]; if (!m.isIncoming && m.to == evt.from && m.msgId == evt.msgId) { _messages[i] = m.copyWith(status: _St.delivered); break; } } });
    } else if (evt is RiftLinkReadEvent) {
      setState(() { for (var i = 0; i < _messages.length; i++) { final m = _messages[i]; if (!m.isIncoming && m.to == evt.from && m.msgId == evt.msgId) { _messages[i] = m.copyWith(status: _St.read); break; } } });
    } else if (evt is RiftLinkUndeliveredEvent) {
      setState(() {
        for (var i = 0; i < _messages.length; i++) {
          final m = _messages[i];
          if (!m.isIncoming && m.msgId == evt.msgId) {
            final isBroadcast = evt.to.isEmpty || m.to == _broadcastTo;
            if (isBroadcast || m.to == evt.to) {
              _messages[i] = m.copyWith(status: _St.undelivered, delivered: evt.delivered ?? 0, total: evt.total ?? 0);
              break;
            }
          }
        }
      });
    } else if (evt is RiftLinkBroadcastDeliveryEvent) {
      setState(() {
        for (var i = 0; i < _messages.length; i++) {
          final m = _messages[i];
          if (!m.isIncoming && m.msgId == evt.msgId && (m.to == _broadcastTo || m.to == null)) {
            _messages[i] = m.copyWith(status: evt.delivered > 0 ? _St.delivered : _St.undelivered, delivered: evt.delivered, total: evt.total);
            break;
          }
        }
      });
    } else if (evt is RiftLinkInfoEvent) {
      _onInfoEvent(evt);
    } else if (evt is RiftLinkRoutesEvent) { setState(() => _routes = evt.routes); }
    else if (evt is RiftLinkGroupsEvent) {
      setState(() {
        _groups = _filterUserGroups(evt.groups);
        if (_group > 0 && !_groups.contains(_group)) _group = 0;
      });
    }
    else if (evt is RiftLinkTelemetryEvent) {
      setState(() {
        if (evt.from == _nodeId && evt.batteryMv > 0) _batteryMv = evt.batteryMv;
        _messages.add(_Msg(from: evt.from, text: '🔋 ${(evt.batteryMv / 1000).toStringAsFixed(2)}V, ${evt.heapKb} KB', isIncoming: true));
      });
      _scrollToBottom();
    } else if (evt is RiftLinkLocationEvent) {
      setState(() { _messages.add(_Msg(from: evt.from, text: '📍 ${evt.lat.toStringAsFixed(5)}, ${evt.lon.toStringAsFixed(5)}', isIncoming: true, isLocation: true)); });
      _scrollToBottom();
    } else if (evt is RiftLinkRegionEvent) { setState(() { _region = evt.region; _channel = evt.channel; }); }
    else if (evt is RiftLinkNeighborsEvent) { setState(() { _neighbors = evt.neighbors; _neighborsRssi = evt.rssi; _neighborsHasKey = evt.hasKey; }); }
    else if (evt is RiftLinkPongEvent) {
      final fromNorm = evt.from.length >= 8 ? evt.from.substring(0, 8).toUpperCase() : evt.from.toUpperCase();
      _pendingPings.remove(fromNorm);
      _showSnack('✓ ${context.l10n.tr('link_ok', {'from': evt.from})}', backgroundColor: context.palette.success, duration: const Duration(seconds: 4));
    }
    else if (evt is RiftLinkErrorEvent) { _showSnack('${context.l10n.tr('error')}: ${evt.msg}', backgroundColor: context.palette.error); }
    else if (evt is RiftLinkWifiEvent) {
      _showSnack(
        evt.connected
            ? context.l10n.tr('wifi_status_connected', {'ssid': evt.ssid, 'ip': evt.ip})
            : context.l10n.tr('wifi_status_disconnected'),
      );
    }
    else if (evt is RiftLinkGpsEvent) { setState(() { _gpsPresent = evt.present; _gpsEnabled = evt.enabled; _gpsFix = evt.hasFix; }); }
    else if (evt is RiftLinkInviteEvent) { _showInviteDialog(evt.id, evt.pubKey, evt.channelKey); }
    else if (evt is RiftLinkSelftestEvent) {
      if (mounted) _showSelftestDialog(evt);
      if (evt.batteryMv > 0) setState(() => _batteryMv = evt.batteryMv);
    } else if (evt is RiftLinkVoiceEvent) {
      _voiceChunks[evt.from] ??= {};
      _voiceChunks[evt.from]![evt.chunk] = evt.data;
      final chunks = _voiceChunks[evt.from]!;
      if (chunks.length == evt.total) {
        final parts = List.generate(evt.total, (i) => chunks[i] ?? '');
        try {
          var bytes = base64Decode(parts.join());
          DateTime? deleteAt;
          if (bytes.length >= 2 && bytes[0] == 0xFF && bytes[1] >= 1 && bytes[1] <= 255) {
            final ttl = bytes[1];
            bytes = bytes.sublist(2);
            deleteAt = DateTime.now().add(Duration(minutes: ttl));
          }
          _voiceChunks.remove(evt.from);
          setState(() { _messages.add(_Msg(from: evt.from, text: '🎤 ${context.l10n.tr('voice')}', isIncoming: true, isVoice: true, voiceData: bytes, deleteAt: deleteAt)); });
          _scrollToBottom();
        } catch (_) {}
      }
    }
  }

  void _scrollToBottom() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scrollController.hasClients) _scrollController.animateTo(_scrollController.position.maxScrollExtent, duration: const Duration(milliseconds: 200), curve: Curves.easeOut);
    });
  }

  // ── Actions ──

  static const _broadcastTo = 'FFFFFFFFFFFFFFFF';  // совпадает с evt.to в "sent" для broadcast

  Future<void> _send({int ttlMinutes = 0}) async {
    final text = _controller.text.trim();
    if (text.isEmpty) return;
    _controller.clear();
    // Broadcast: to = FFFF... чтобы RiftLinkSentEvent нашёл сообщение; group = null
    final toForMsg = _group > 0 ? null : (_unicastTo ?? _broadcastTo);
    setState(() { _messages.add(_Msg(from: _nodeId, text: text, isIncoming: false, to: toForMsg)); });
    _scrollToBottom();
    await widget.ble.send(text: text, to: _unicastTo, group: _group > 0 ? _group : null, ttlMinutes: ttlMinutes);
  }

  Future<void> _showTtlSendMenu() async {
    final text = _controller.text.trim();
    if (text.isEmpty) return;
    final v = await _pickTtlMinutes();
    if (v != null && mounted) _send(ttlMinutes: v);
  }

  Future<int?> _pickTtlMinutes() async {
    if (!mounted) return null;
    final l = context.l10n;
    FocusScope.of(context).unfocus();
    HapticFeedback.mediumImpact();
    return showAppModalBottomSheet<int>(
      context: context,
      backgroundColor: Colors.transparent,
      isScrollControlled: true,
      builder: (ctx) => _TtlPickerSheet(l: l),
    );
  }

  Future<void> _sendLocation() async {
    if (!widget.ble.isConnected) return;
    setState(() => _locationLoading = true);
    try {
      var perm = await Geolocator.checkPermission();
      if (perm == LocationPermission.denied) perm = await Geolocator.requestPermission();
      if (perm == LocationPermission.denied || perm == LocationPermission.deniedForever) { if (mounted) _showSnack(context.l10n.tr('loc_denied')); return; }
      final pos = await Geolocator.getCurrentPosition();
      await widget.ble.sendLocation(lat: pos.latitude, lon: pos.longitude, alt: pos.altitude.toInt());
      if (mounted) { setState(() { _messages.add(_Msg(from: _nodeId, text: '📍 ${pos.latitude.toStringAsFixed(5)}, ${pos.longitude.toStringAsFixed(5)}', isIncoming: false, isLocation: true)); }); _scrollToBottom(); }
    } catch (e) { if (mounted) _showSnack(e.toString()); }
    finally { if (mounted) setState(() => _locationLoading = false); }
  }

  /// Периодическая отправка GPS от телефона для beacon-sync (устройство без аппаратного GPS)
  Future<void> _sendGpsSyncFromPhone() async {
    if (!mounted || !widget.ble.isConnected) return;
    try {
      final perm = await Geolocator.checkPermission();
      if (perm == LocationPermission.denied || perm == LocationPermission.deniedForever) return;
      final pos = await Geolocator.getCurrentPosition();
      final utcMs = DateTime.now().millisecondsSinceEpoch;
      await widget.ble.sendGpsSync(utcMs: utcMs, lat: pos.latitude, lon: pos.longitude, alt: pos.altitude.toInt());
    } catch (_) {}
  }

  Future<void> _toggleVoiceRecord() async {
    if (_voiceRecording) {
      _voiceRecordTicker?.dispose();
      _voiceRecordTicker = null;
      setState(() => _voiceRecording = false);
      final bytes = await VoiceService.stopRecord();
      if (!mounted || bytes == null || bytes.isEmpty) {
        if (mounted) _showSnack(context.l10n.tr('voice_mic_error'));
        return;
      }
      if (_neighbors.isEmpty) { _showSnack(context.l10n.tr('no_neighbors_voice')); return; }
      final to = await _pickNeighborDialog();
      if (to == null || !mounted) return;
      final ttl = _voiceTtlMinutes;
      List<int> payload = bytes;
      if (ttl > 0) payload = [0xFF, ttl, ...bytes];
      // Согласовано с прошивкой: BLE_ATT_MAX_JSON_BYTES=512, один JSON на write; сырой чанк ≤300 B → base64 ≤400.
      const chunkSize = 300;
      final chunks = <String>[];
      for (var i = 0; i < payload.length; i += chunkSize) { chunks.add(base64Encode(payload.sublist(i, (i + chunkSize < payload.length) ? i + chunkSize : payload.length))); }
      final ok = await widget.ble.sendVoice(to: to, chunks: chunks);
      if (mounted) {
        if (ok) { setState(() { _messages.add(_Msg(from: _nodeId, text: '🎤 ${context.l10n.tr('voice')}', isIncoming: false, isVoice: true)); }); _scrollToBottom(); }
        else { _showSnack(context.l10n.tr('voice_send_error')); }
      }
    } else {
      _startVoiceRecord(ttlMinutes: 0);
    }
  }

  // Запуск записи с заданным TTL (0 = без TTL)
  Future<void> _startVoiceRecord({required int ttlMinutes}) async {
    FocusScope.of(context).unfocus();
    try {
      final ok = await VoiceService.startRecord();
      if (mounted) {
        setState(() {
          _voiceTtlMinutes = ttlMinutes;
          _voiceRecording = ok;
          _voiceRecordStartTime = ok ? DateTime.now() : null;
        });
        if (ok) {
          _voiceRecordTicker?.dispose();
          var tickCount = 0;
          _voiceRecordTicker = createTicker((elapsed) {
            if (!mounted || !_voiceRecording) return;
            tickCount++;
            if (tickCount % 3 == 0) setState(() {});
          })..start();
        } else {
          _showSnack(context.l10n.tr('voice_mic_error'));
        }
      }
    } catch (e) {
      if (mounted) _showSnack('${context.l10n.tr('voice_mic_error')}: $e');
    }
  }

  Future<void> _cancelVoiceRecord() async {
    _voiceRecordTicker?.dispose();
    _voiceRecordTicker = null;
    await VoiceService.cancelRecord();
    if (mounted) setState(() { _voiceRecording = false; _voiceRecordStartTime = null; });
  }

  // Long-press на микрофон: выбор TTL → старт записи
  Future<void> _onVoiceLongPress() async {
    if (!widget.ble.isConnected || _voiceRecording) return;
    final ttl = await _pickVoiceTtlDialog();
    if (ttl == null || !mounted) return;
    await _startVoiceRecord(ttlMinutes: ttl);
  }

  Future<String?> _pickNeighborDialog() {
    FocusScope.of(context).unfocus();
    return showAppModalBottomSheet<String>(
      context: context,
      backgroundColor: Colors.transparent,
      isScrollControlled: true,
      builder: (ctx) => Container(
        decoration: BoxDecoration(
          color: context.palette.card,
          borderRadius: const BorderRadius.vertical(top: Radius.circular(16)),
        ),
        child: SafeArea(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Padding(
                padding: const EdgeInsets.all(16),
                child: Text(context.l10n.tr('send_voice'), style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: context.palette.onSurface)),
              ),
              ..._neighbors.map((id) {
                final nick = _nicknameForId(id);
                return ListTile(
                  title: Text(nick ?? id, style: TextStyle(fontSize: 15, color: context.palette.onSurface)),
                  subtitle: nick != null ? Text(id, style: TextStyle(fontFamily: 'monospace', fontSize: 12, color: context.palette.onSurfaceVariant)) : null,
                  onTap: () => Navigator.pop(ctx, id),
                );
              }),
              ListTile(title: Text(context.l10n.tr('cancel'), style: TextStyle(color: context.palette.onSurfaceVariant)), onTap: () => Navigator.pop(ctx)),
            ],
          ),
        ),
      ),
    );
  }

  Future<int?> _pickVoiceTtlDialog() => _pickTtlMinutes();

  void _showSnack(String text, {Color? backgroundColor, Duration duration = const Duration(seconds: 3)}) {
    var kind = AppSnackKind.neutral;
    if (backgroundColor == context.palette.error) {
      kind = AppSnackKind.error;
    } else if (backgroundColor == context.palette.success) {
      kind = AppSnackKind.success;
    }
    showAppSnackBar(
      context,
      text,
      kind: kind,
      duration: duration,
      margin: kSnackBarMarginChat,
    );
  }

  Future<void> _showSelftestDialog(RiftLinkSelftestEvent evt) async {
    await showRiftSelftestDialog(context, evt, lastInfo: widget.ble.lastInfo);
  }

  Color _batteryColorForMv(int mv) {
    final v = mv / 1000.0;
    if (v >= 3.75) return context.palette.success;
    if (v >= 3.45) return const Color(0xFFFFB300);
    return context.palette.error;
  }

  Widget _buildBatteryBadge() {
    final mv = _batteryMv;
    if (mv == null || mv <= 0) return const SizedBox.shrink();
    final color = _batteryColorForMv(mv);
    final pct = _batteryPercent;
    final label = pct != null ? '$pct%' : '${(mv / 1000.0).toStringAsFixed(2)} V';
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      decoration: BoxDecoration(
        color: color.withOpacity(0.14),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: color.withOpacity(0.35), width: 0.5),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(_charging ? Icons.battery_charging_full : Icons.battery_std, size: 15, color: color),
          const SizedBox(width: 4),
          Text(
            label,
            style: TextStyle(
              fontSize: 11,
              fontWeight: FontWeight.w700,
              color: color,
              fontFeatures: const [FontFeature.tabularFigures()],
            ),
          ),
        ],
      ),
    );
  }

  void _showPingDialog({String? prefilledId}) {
    Navigator.push(
      context,
      _PingDialogRoute(
        l: context.l10n,
        prefilledId: prefilledId,
        onPing: (id) async {
          final ok = await widget.ble.sendPing(id);
          if (!mounted) return;
          if (!ok) { _showSnack(context.l10n.tr('error')); return; }
          final idNorm = id.length >= 8 ? id.substring(0, 8).toUpperCase() : id.toUpperCase();
          _pendingPings.add(idNorm);
          _showSnack(context.l10n.tr('ping_sent', {'id': id}));
          Future.delayed(const Duration(seconds: 20), () {
            if (!mounted) return;
            if (_pendingPings.remove(idNorm)) {
              _showSnack(context.l10n.tr('ping_timeout', {'id': id}), backgroundColor: context.palette.error, duration: const Duration(seconds: 4));
            }
          });
        },
      ),
    );
  }

  void _showAddContactDialog(String id) {
    final nc = TextEditingController(text: _contactNicknames[id] ?? '');
    final p = context.palette;
    final l = context.l10n;
    showAppDialog(context: context, builder: (ctx) => RiftDialogFrame(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Text(
            l.tr('add_contact'),
            style: Theme.of(context).textTheme.titleMedium?.copyWith(
                  fontWeight: FontWeight.w700,
                  color: p.onSurface,
                ),
          ),
          const SizedBox(height: 14),
          TextField(
            controller: nc,
            decoration: InputDecoration(labelText: l.tr('contact_nickname')),
            maxLength: 16,
            autofocus: true,
          ),
          const SizedBox(height: 6),
          Row(
            mainAxisAlignment: MainAxisAlignment.end,
            children: [
              TextButton(
                style: TextButton.styleFrom(
                  foregroundColor: p.onSurfaceVariant,
                  padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                  minimumSize: Size.zero,
                  tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                ),
                onPressed: () => Navigator.pop(ctx),
                child: Text(l.tr('cancel')),
              ),
              const SizedBox(width: 4),
              TextButton(
                style: TextButton.styleFrom(
                  foregroundColor: p.primary,
                  padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                  minimumSize: Size.zero,
                  tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                  textStyle: const TextStyle(fontWeight: FontWeight.w600),
                ),
                onPressed: () async {
                  final nick = nc.text.trim();
                  Navigator.pop(ctx);
                  await ContactsService.add(Contact(id: id, nickname: nick));
                  await _loadContactNicknames();
                  if (mounted) _showSnack(l.tr('contact_added'));
                },
                child: Text(l.tr('save')),
              ),
            ],
          ),
        ],
      ),
    ));
  }

  void _showInviteDialog(String id, String pubKey, [String? channelKey]) {
    final map = <String, String>{'id': id, 'pubKey': pubKey};
    if (channelKey != null && channelKey.isNotEmpty) map['channelKey'] = channelKey;
    final data = jsonEncode(map);
    final p = context.palette;
    final l = context.l10n;
    showAppDialog(
      context: context,
      builder: (ctx) => RiftDialogFrame(
        maxWidth: 360,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(
              l.tr('invite_created'),
              style: Theme.of(context).textTheme.titleMedium?.copyWith(
                    fontWeight: FontWeight.w700,
                    color: p.onSurface,
                  ),
            ),
            const SizedBox(height: 12),
            ConstrainedBox(
              constraints: BoxConstraints(maxHeight: MediaQuery.sizeOf(context).height * 0.36),
              child: SingleChildScrollView(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text('ID: $id', style: TextStyle(fontFamily: 'monospace', fontSize: 12, color: p.onSurface)),
                    const SizedBox(height: 8),
                    Text(
                      'PubKey: ${pubKey.length > 40 ? '${pubKey.substring(0, 40)}…' : pubKey}',
                      style: TextStyle(fontFamily: 'monospace', fontSize: 11, color: p.onSurface),
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 12),
            FilledButton.tonalIcon(
              style: FilledButton.styleFrom(
                foregroundColor: p.primary,
                backgroundColor: p.primary.withOpacity(0.12),
                padding: const EdgeInsets.symmetric(vertical: 12),
              ),
              onPressed: () {
                Clipboard.setData(ClipboardData(text: data));
                _showSnack(l.tr('copied'));
              },
              icon: const Icon(Icons.copy_rounded, size: 20),
              label: Text(l.tr('copy')),
            ),
            Align(
              alignment: Alignment.centerRight,
              child: TextButton(
                style: TextButton.styleFrom(foregroundColor: p.primary),
                onPressed: () => Navigator.pop(ctx),
                child: Text(l.tr('ok')),
              ),
            ),
          ],
        ),
      ),
    );
  }

  // ── Build ──

  void _runSelftestFromToolsMenu() {
    if (!widget.ble.isConnected) {
      _showSnack(context.l10n.tr('connect_first'));
      return;
    }
    HapticFeedback.lightImpact();
    widget.ble.selftest();
  }

  Future<void> _showAppMenu(BuildContext context, AppLocalizations l) async {
    FocusScope.of(context).unfocus();
    final value = await Navigator.push<String>(
      context,
      AppPopoverMenuRoute<String>(
        toolbarHeight: 44,
        child: _AppMenuPopover(l: l),
      ),
    );
    if (value != null && mounted) _onMenuSelected(value);
  }

  void _onMenuSelected(String? value) {
    if (value == null || !mounted) return;
    FocusScope.of(context).unfocus();
    switch (value) {
      case 'map':
        Navigator.push(context, MaterialPageRoute(builder: (_) => MapScreen(ble: widget.ble)));
        break;
      case 'mesh':
        Navigator.push(context, MaterialPageRoute(builder: (_) => MeshScreen(ble: widget.ble, nodeId: _nodeId, neighbors: _neighbors, neighborsRssi: _neighborsRssi, routes: _routes)));
        break;
      case 'contacts_hub':
        Navigator.push(
          context,
          MaterialPageRoute(
            builder: (_) => ContactsGroupsHubScreen(
              ble: widget.ble,
              neighbors: _neighbors,
              initialGroups: _groups,
            ),
          ),
        ).then((_) {
          _loadContactNicknames();
          widget.ble.getGroups();
        });
        break;
      case 'ping':
        _showPingDialog();
        break;
      case 'selftest':
        _runSelftestFromToolsMenu();
        break;
      case 'send_location':
        if (widget.ble.isConnected) {
          _sendLocation();
        } else {
          _showSnack(context.l10n.tr('connect_first'));
        }
        break;
      case 'settings':
        if (widget.ble.isConnected) {
          Navigator.push(context, MaterialPageRoute(builder: (_) => SettingsScreen(
            ble: widget.ble, nodeId: _nodeId, nickname: _nickname, region: _region, channel: _channel,
            sf: _sf,
            gpsPresent: _gpsPresent, gpsEnabled: _gpsEnabled, gpsFix: _gpsFix, powersave: _powersave,
            offlinePending: _offlinePending, batteryMv: _batteryMv,
            onNicknameChanged: (n) => setState(() => _nickname = n),
            onRegionChanged: (r, c) => setState(() { _region = r; _channel = c; }),
            onSfChanged: (v) => setState(() => _sf = v),
            onPowersaveChanged: (v) => setState(() => _powersave = v),
            onGpsChanged: (v) => setState(() => _gpsEnabled = v),
            meshAnimationEnabled: _meshAnimationEnabled,
            onMeshAnimationChanged: _onMeshAnimationChanged,
          ))).then((_) => setState(() {}));
        } else {
          _showSnack(context.l10n.tr('connect_first'));
        }
        break;
    }
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    return Scaffold(
      resizeToAvoidBottomInset: true,
      backgroundColor: context.palette.surface,
      extendBody: true,
      appBar: AppBar(
        toolbarHeight: 44,
        title: Builder(
          builder: (context) {
            final name = (_nickname ?? _nodeId).trim();
            final label = widget.ble.isConnected
                ? (name.isNotEmpty ? name : '—')
                : l.tr('disconnected');
            final row = Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(widget.ble.isConnected ? Icons.bluetooth_connected : Icons.bluetooth_disabled, size: 18, color: widget.ble.isConnected ? context.palette.success : context.palette.onSurfaceVariant),
                const SizedBox(width: 6),
                Flexible(
                  child: Text(
                    label,
                    style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: widget.ble.isConnected ? context.palette.success : context.palette.onSurfaceVariant),
                    overflow: TextOverflow.ellipsis,
                  ),
                ),
                if (widget.ble.isConnected && _batteryMv != null && _batteryMv! > 0) ...[
                  const SizedBox(width: 8),
                  _buildBatteryBadge(),
                ],
                if (_offlinePending != null && _offlinePending! > 0) ...[
                  const SizedBox(width: 6),
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                    decoration: BoxDecoration(
                      color: context.palette.primary.withOpacity(0.16),
                      borderRadius: BorderRadius.circular(8),
                      border: Border.all(color: context.palette.primary.withOpacity(0.45)),
                    ),
                    child: Text(
                      '${_offlinePending!}',
                      style: TextStyle(
                        fontSize: 11,
                        fontWeight: FontWeight.w700,
                        color: context.palette.primary,
                      ),
                    ),
                  ),
                ],
              ],
            );
            if (!widget.ble.isConnected) {
              return GestureDetector(
                onTap: () => Navigator.of(context).pushAndRemoveUntil(
                  MaterialPageRoute(builder: (_) => const ScanScreen()),
                  (r) => false,
                ),
                child: row,
              );
            }
            return GestureDetector(
              onTap: _showConnectMenu,
              child: row,
            );
          },
        ),
        backgroundColor: context.palette.surfaceVariant,
        foregroundColor: context.palette.onSurface,
        elevation: 0,
        actions: [
          IconButton(
            icon: const Icon(Icons.more_vert),
            tooltip: l.tr('settings'),
            onPressed: () => _showAppMenu(context, l),
          ),
        ],
      ),
      body: Stack(
        fit: StackFit.expand,
        children: [
          if (_reconnecting)
            Positioned.fill(
              child: Container(
                color: Colors.black54,
                child: Center(
                  child: Card(
                    color: context.palette.card,
                    child: Padding(
                      padding: const EdgeInsets.all(24),
                      child: Column(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          CircularProgressIndicator(color: context.palette.primary),
                          const SizedBox(height: 16),
                          Text(l.tr('reconnecting', {'n': '${_reconnectAttempt}/3'}), style: TextStyle(color: context.palette.onSurface)),
                        ],
                      ),
                    ),
                  ),
                ),
              ),
            ),
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
          SafeArea(
            top: false,
            bottom: false,
            child: Column(
              mainAxisSize: MainAxisSize.max,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
            Divider(height: 1, color: context.palette.divider),
            Expanded(
              child: Stack(
                children: [
                  Container(
                    color: Colors.transparent,
                    child: _visibleMessages.isEmpty
                      ? Center(
                          child: Column(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              Icon(Icons.chat_bubble_outline, size: 48, color: context.palette.onSurfaceVariant.withOpacity(0.4)),
                              const SizedBox(height: 12),
                              Text(l.tr('no_messages'), style: TextStyle(color: context.palette.onSurfaceVariant.withOpacity(0.7), fontSize: 14)),
                            ],
                          ),
                        )
                      : ListView(
                          controller: _scrollController,
                          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                          children: _visibleMessages.map((m) => _buildMessageBubble(m)).toList(),
                        ),
                  ),
                  // Градиент-затухание над панелью ввода — плавный переход при смахивании
                  Positioned(
                    left: 0,
                    right: 0,
                    bottom: 0,
                    height: 24,
                    child: IgnorePointer(
                      child: Container(
                        decoration: BoxDecoration(
                          gradient: LinearGradient(
                            begin: Alignment.topCenter,
                            end: Alignment.bottomCenter,
                            colors: [
                              context.palette.surface.withOpacity(0),
                              context.palette.surface.withOpacity(0.95),
                            ],
                          ),
                        ),
                      ),
                    ),
                  ),
                ],
              ),
            ),
            _buildInputBar(l),
              ],
            ),
          ),
        ],
      ),
    );
  }

  // ── Input bar: одна панель. При записи — кнопка растёт на месте, слева таймер и свайп. ──

  Widget _buildInputBar(AppLocalizations l) {
    final elapsed = _voiceRecordStartTime != null
        ? DateTime.now().difference(_voiceRecordStartTime!)
        : Duration.zero;
    final sec = elapsed.inSeconds;
    final tenths = (elapsed.inMilliseconds % 1000) ~/ 100;
    final timeStr = '${sec ~/ 60}:${(sec % 60).toString().padLeft(2, '0')},$tenths';

    final bottomPad = MediaQuery.of(context).padding.bottom;
    return Container(
      padding: EdgeInsets.fromLTRB(20, 10, 8, 10 + bottomPad),
      decoration: BoxDecoration(
        color: context.palette.surfaceVariant.withOpacity(0.92),
        border: Border(top: BorderSide(color: context.palette.divider, width: 0.5)),
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.end,
        children: [
          Expanded(
            child: Container(
              decoration: BoxDecoration(
                color: context.palette.card,
                borderRadius: BorderRadius.circular(24),
                border: Border.all(color: context.palette.divider),
              ),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.center,
                children: [
                  Padding(
                    padding: const EdgeInsets.only(left: 4),
                    child: _inputIcon(
                      _group > 0 ? Icons.group_outlined : (_unicastTo != null ? Icons.person_outline : Icons.public),
                      widget.ble.isConnected ? _showRecipientPickerSheet : null,
                      tooltip: '${l.tr('to')} ${_recipientPillLabel(l)}',
                      iconColor: (_group > 0 || _unicastTo != null) ? context.palette.primary : null,
                    ),
                  ),
                  Expanded(
                    child: AnimatedSwitcher(
              duration: const Duration(milliseconds: 500),
              switchInCurve: Curves.easeOut,
              switchOutCurve: Curves.easeIn,
              transitionBuilder: (child, animation) => FadeTransition(opacity: animation, child: child),
              child: _voiceRecording
                  ? KeyedSubtree(
                      key: const ValueKey('rec'),
                      child: Padding(
                        padding: const EdgeInsets.fromLTRB(14, 10, 8, 10),
                        child: Row(
                          children: [
                            Container(
                              width: 10,
                              height: 10,
                              decoration: BoxDecoration(color: context.palette.error, shape: BoxShape.circle),
                            ),
                            const SizedBox(width: 10),
                            Text(timeStr, style: TextStyle(fontSize: 16, fontWeight: FontWeight.w500, color: context.palette.onSurface, fontFamily: 'monospace')),
                            const SizedBox(width: 16),
                            Expanded(
                              child: Text(
                                l.tr('voice_swipe_cancel'),
                                style: TextStyle(fontSize: 14, color: context.palette.onSurfaceVariant),
                                textAlign: TextAlign.center,
                              ),
                            ),
                          ],
                        ),
                      ),
                    )
                  : KeyedSubtree(
                      key: const ValueKey('input'),
                      child: CupertinoTextField(
                        controller: _controller,
                        maxLines: 6,
                        minLines: 1,
                        maxLength: _group > 0 ? 160 : 200,
                        style: TextStyle(color: context.palette.onSurface, fontSize: 16),
                        placeholder: l.tr('message_hint'),
                        placeholderStyle: TextStyle(color: context.palette.onSurfaceVariant),
                        padding: const EdgeInsets.fromLTRB(14, 10, 4, 10),
                        decoration: const BoxDecoration(color: Colors.transparent),
                        onSubmitted: (_) => _send(),
                      ),
                    ),
                    ),
                  ),
                  Padding(
                    padding: const EdgeInsets.only(right: 2),
                    child: AnimatedScale(
                      scale: _voiceRecording ? 1.35 : 1.0,
                      duration: const Duration(milliseconds: 300),
                      curve: Curves.easeInOut,
                      child: _voiceRecording
                          ? Dismissible(
                              key: const ValueKey('voice_mic_btn'),
                              direction: DismissDirection.endToStart,
                              movementDuration: const Duration(milliseconds: 500),
                              dismissThresholds: const {DismissDirection.endToStart: 0.4},
                              confirmDismiss: (dir) async {
                                if (dir == DismissDirection.endToStart) {
                                  await _cancelVoiceRecord();
                                  return true;
                                }
                                return false;
                              },
                              child: _TtlTapButton(
                                onTap: _toggleVoiceRecord,
                                onLongPress: null,
                                icon: Icons.mic,
                                size: 36,
                                iconSize: 20,
                              ),
                            )
                          : _hasText
                            ? _TtlTapButton(
                                onTap: widget.ble.isConnected ? () => _send() : null,
                                onLongPress: widget.ble.isConnected ? _showTtlSendMenu : null,
                                icon: Icons.send,
                                size: 36,
                                iconSize: 18,
                              )
                            : _TtlTapButton(
                                onTap: widget.ble.isConnected ? _toggleVoiceRecord : null,
                                onLongPress: widget.ble.isConnected ? _onVoiceLongPress : null,
                                icon: Icons.mic,
                                size: 36,
                                iconSize: 20,
                              ),
                    ),
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  // ── Message bubble ──

  Widget _buildMessageBubble(_Msg m) {
    final mine = !m.isIncoming;
    return Align(
      alignment: mine ? Alignment.centerRight : Alignment.centerLeft,
      child: Container(
        margin: const EdgeInsets.only(bottom: 6),
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 8),
        constraints: BoxConstraints(maxWidth: MediaQuery.of(context).size.width * 0.78),
        decoration: BoxDecoration(
          color: mine ? context.palette.primary.withOpacity(0.25) : context.palette.card,
          borderRadius: BorderRadius.circular(14),
          border: Border.all(color: mine ? context.palette.primary : context.palette.divider),
        ),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
          if (m.isIncoming && m.from.isNotEmpty)
            Padding(padding: const EdgeInsets.only(bottom: 2), child: Text(
              _nicknameForId(m.from) ?? m.from,
              style: TextStyle(fontSize: 11, fontWeight: FontWeight.w600, color: context.palette.primary),
            )),
          Row(mainAxisSize: MainAxisSize.min, children: [
            Flexible(child: Text(m.text, style: TextStyle(color: context.palette.onSurface, fontSize: 14))),
            if (!m.isIncoming) Padding(padding: const EdgeInsets.only(left: 6), child: Text(
              m.status == _St.undelivered
                  ? (m.total != null && m.total! > 0 ? '✗ 0/${m.total}' : '✗')
                  : m.status == _St.read
                      ? '✓✓'
                      : m.status == _St.delivered
                          ? (m.delivered != null && m.total != null && m.total! > 0 ? '✓ ${m.delivered}/${m.total}' : '✓✓')
                          : '✓',
              style: TextStyle(fontSize: 10, color: m.status == _St.undelivered ? context.palette.error : (m.status == _St.read ? context.palette.primary : context.palette.onSurfaceVariant)),
            )),
            if (m.isIncoming && m.rssi != null && m.rssi != 0) Padding(padding: const EdgeInsets.only(left: 6), child: Text('${m.rssi}dBm', style: TextStyle(fontSize: 9, color: context.palette.onSurfaceVariant))),
            if (m.isVoice && m.voiceData != null) IconButton(
              icon: Icon(Icons.play_circle_outline, color: context.palette.primary, size: 22),
              onPressed: () async { if (m.voiceData != null && m.voiceData!.isNotEmpty) await VoiceService.play(m.voiceData!); },
              tooltip: context.l10n.tr('play'), padding: EdgeInsets.zero, constraints: const BoxConstraints(),
            ),
          ]),
        ]),
      ),
    );
  }

  // ── Компактные иконки в строке ввода (как в мессенджерах) ──

  Widget _inputIcon(IconData icon, VoidCallback? onTap, {bool loading = false, bool active = false, String? tooltip, String? badge, Color? iconColor}) {
    final enabled = onTap != null;
    final defaultIconColor = enabled ? context.palette.onSurfaceVariant : context.palette.divider;
    final resolvedColor = loading
        ? context.palette.primary
        : (active ? context.palette.error : (iconColor ?? defaultIconColor));
    final core = SizedBox(
      width: 40,
      height: 40,
      child: Stack(
        alignment: Alignment.center,
        children: [
          if (loading)
            SizedBox(width: 24, height: 24, child: CircularProgressIndicator(strokeWidth: 2, color: context.palette.primary))
          else
            Icon(icon, size: 22, color: resolvedColor),
          if (badge != null && badge.isNotEmpty)
            Positioned(
              right: 0,
              top: 0,
              child: Container(
                padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 1),
                decoration: BoxDecoration(color: context.palette.primary, borderRadius: BorderRadius.circular(8)),
                child: Text(badge, style: TextStyle(fontSize: 9, color: Colors.white, fontWeight: FontWeight.w600)),
              ),
            ),
        ],
      ),
    );
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: onTap,
      child: (tooltip != null && tooltip.isNotEmpty) ? Tooltip(message: tooltip!, child: core) : core,
    );
  }

  Widget _quickChip(IconData icon, String? label, VoidCallback? onTap, {bool loading = false, Color? iconColor}) {
    final enabled = onTap != null;
    return Padding(
      padding: const EdgeInsets.only(right: 8),
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
          decoration: BoxDecoration(
            color: enabled ? context.palette.card : context.palette.surfaceVariant,
            borderRadius: BorderRadius.circular(24),
            border: Border.all(color: context.palette.divider),
          ),
          child: Row(mainAxisSize: MainAxisSize.min, children: [
            if (loading) SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2, color: context.palette.primary))
            else Icon(icon, size: 22, color: iconColor ?? (enabled ? context.palette.onSurface : context.palette.onSurfaceVariant)),
            if (label != null) ...[const SizedBox(width: 6), Text(label, style: TextStyle(fontSize: 15, fontWeight: FontWeight.w500, color: enabled ? context.palette.onSurface : context.palette.onSurfaceVariant))],
          ]),
        ),
      ),
    );
  }

}

// ── TTL tap/long-press button (400ms long press для вызова TTL) ──

class _TtlTapButton extends StatefulWidget {
  final VoidCallback? onTap;
  final VoidCallback? onLongPress;
  final IconData icon;
  final double size;
  final double iconSize;

  const _TtlTapButton({
    required this.onTap,
    required this.onLongPress,
    required this.icon,
    required this.size,
    required this.iconSize,
  });

  @override
  State<_TtlTapButton> createState() => _TtlTapButtonState();
}

class _TtlTapButtonState extends State<_TtlTapButton> {
  Timer? _longPressTimer;

  @override
  void dispose() {
    _longPressTimer?.cancel();
    super.dispose();
  }

  void _handlePointerDown(PointerDownEvent _) {
    if (widget.onLongPress == null) return;
    _longPressTimer = Timer(const Duration(milliseconds: 400), () {
      _longPressTimer = null;
      widget.onLongPress?.call();
    });
  }

  void _handlePointerUp(PointerUpEvent _) {
    if (_longPressTimer != null) {
      _longPressTimer?.cancel();
      _longPressTimer = null;
      widget.onTap?.call();
    }
  }

  void _handlePointerCancel(PointerCancelEvent _) {
    _longPressTimer?.cancel();
    _longPressTimer = null;
  }

  @override
  Widget build(BuildContext context) {
    final enabled = widget.onTap != null || widget.onLongPress != null;
    return Listener(
      onPointerDown: _handlePointerDown,
      onPointerUp: _handlePointerUp,
      onPointerCancel: _handlePointerCancel,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: widget.onLongPress != null ? null : widget.onTap,
        child: Container(
          width: widget.size,
          height: widget.size,
          decoration: BoxDecoration(
            color: enabled ? context.palette.primary : context.palette.divider,
            shape: BoxShape.circle,
          ),
          alignment: Alignment.center,
          child: Icon(widget.icon, size: widget.iconSize, color: Colors.white),
        ),
      ),
    );
  }
}

/// Окно Ping по центру экрана — красивая карточка с вводом (Navigator.push)
class _PingDialogRoute extends PageRouteBuilder<void> {
  final AppLocalizations l;
  final String? prefilledId;
  final Future<void> Function(String id) onPing;

  _PingDialogRoute({required this.l, this.prefilledId, required this.onPing})
      : super(
          opaque: false,
          barrierColor: Colors.black54,
          barrierDismissible: true,
          pageBuilder: (context, animation, secondaryAnimation) => _PingDialogContent(l: l, prefilledId: prefilledId, onPing: onPing),
          transitionsBuilder: (context, animation, secondaryAnimation, child) {
            return Stack(
              children: [
                Positioned.fill(
                  child: GestureDetector(
                    onTap: () => Navigator.pop(context),
                    behavior: HitTestBehavior.opaque,
                    child: Container(color: Colors.transparent),
                  ),
                ),
                Center(
                  child: FadeTransition(
                    opacity: CurvedAnimation(parent: animation, curve: Curves.easeOut),
                    child: ScaleTransition(
                      scale: Tween<double>(begin: 0.9, end: 1.0).animate(CurvedAnimation(parent: animation, curve: Curves.easeOut)),
                      child: child,
                    ),
                  ),
                ),
              ],
            );
          },
        );
}

class _PingDialogContent extends StatefulWidget {
  final AppLocalizations l;
  final String? prefilledId;
  final Future<void> Function(String id) onPing;

  const _PingDialogContent({required this.l, this.prefilledId, required this.onPing});

  @override
  State<_PingDialogContent> createState() => _PingDialogContentState();
}

class _PingDialogContentState extends State<_PingDialogContent> {
  late final TextEditingController _controller;

  @override
  void initState() {
    super.initState();
    _controller = TextEditingController(text: widget.prefilledId ?? '');
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  Future<void> _doPing() async {
    final id = _controller.text.trim().toUpperCase();
    if (id.length != 8) {
      showAppSnackBar(
        context,
        widget.l.tr('ping_invalid'),
        kind: AppSnackKind.error,
        margin: kSnackBarMarginChat,
      );
      return;
    }
    Navigator.pop(context);
    await widget.onPing(id);
  }

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.transparent,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 32),
        child: Container(
          padding: const EdgeInsets.all(24),
          decoration: BoxDecoration(
            color: context.palette.card,
            borderRadius: BorderRadius.circular(16),
            border: Border.all(color: context.palette.divider),
            boxShadow: [BoxShadow(color: Colors.black.withOpacity(0.4), blurRadius: 24, offset: const Offset(0, 8))],
          ),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Row(
                children: [
                  Icon(Icons.radar, color: context.palette.primary, size: 28),
                  const SizedBox(width: 12),
                  Text(widget.l.tr('ping'), style: TextStyle(fontSize: 20, fontWeight: FontWeight.w600, color: context.palette.onSurface)),
                ],
              ),
              const SizedBox(height: 16),
              Container(
                decoration: BoxDecoration(
                  color: context.palette.surfaceVariant,
                  borderRadius: BorderRadius.circular(12),
                  border: Border.all(color: context.palette.divider),
                ),
                child: CupertinoTextField(
                  controller: _controller,
                  placeholder: 'A1B2C3D4',
                  maxLength: 8,
                  style: TextStyle(fontFamily: 'monospace', fontSize: 16, color: context.palette.onSurface),
                  padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
                  decoration: const BoxDecoration(color: Colors.transparent),
                  placeholderStyle: TextStyle(color: context.palette.onSurfaceVariant),
                  textCapitalization: TextCapitalization.characters,
                  onSubmitted: (_) => _doPing(),
                ),
              ),
              const SizedBox(height: 20),
              Row(
                mainAxisAlignment: MainAxisAlignment.end,
                children: [
                  TextButton(onPressed: () => Navigator.pop(context), child: Text(widget.l.tr('cancel'))),
                  const SizedBox(width: 8),
                  ElevatedButton(
                    style: ElevatedButton.styleFrom(backgroundColor: context.palette.primary, foregroundColor: Colors.white),
                    onPressed: _doPing,
                    child: const Text('Ping'),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}

/// Popover меню в стиле Telegram — компактная карточка сверху справа.
/// Второй уровень «Инструменты»: карта, геолокация в mesh, топология, ping, самотест.
class _AppMenuPopover extends StatefulWidget {
  final AppLocalizations l;

  const _AppMenuPopover({required this.l});

  @override
  State<_AppMenuPopover> createState() => _AppMenuPopoverState();
}

class _AppMenuPopoverState extends State<_AppMenuPopover> {
  bool _tools = false;

  AppLocalizations get l => widget.l;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.transparent,
      child: AnimatedSize(
        duration: const Duration(milliseconds: 220),
        curve: Curves.easeInOutCubic,
        alignment: Alignment.topRight,
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
            child: AnimatedSwitcher(
              duration: const Duration(milliseconds: 220),
              switchInCurve: Curves.easeOut,
              switchOutCurve: Curves.easeIn,
              transitionBuilder: (child, anim) => FadeTransition(
                opacity: anim,
                child: SlideTransition(
                  position: Tween<Offset>(begin: const Offset(0.06, 0), end: Offset.zero).animate(anim),
                  child: child,
                ),
              ),
              child: _tools
                  ? _buildToolsPage(key: const ValueKey('tools'))
                  : _buildMainPage(key: const ValueKey('main')),
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildMainPage({required Key key}) {
    return Column(
      key: key,
      mainAxisSize: MainAxisSize.min,
      children: [
        _popoverItem(context, Icons.contact_mail_outlined, l.tr('contacts_groups_title'), 'contacts_hub'),
        _toolsEntry(),
        _popoverItem(context, Icons.settings, l.tr('settings'), 'settings'),
      ],
    );
  }

  Widget _toolsEntry() {
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: () => setState(() => _tools = true),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
          child: Row(
            children: [
              Icon(Icons.handyman_outlined, size: 22, color: context.palette.onSurface),
              const SizedBox(width: 14),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text(l.tr('menu_tools'), style: TextStyle(fontSize: 15, color: context.palette.onSurface)),
                    const SizedBox(height: 2),
                    Text(
                      l.tr('menu_tools_subtitle'),
                      style: TextStyle(fontSize: 11, height: 1.2, color: context.palette.onSurfaceVariant.withOpacity(0.95)),
                    ),
                  ],
                ),
              ),
              Icon(Icons.chevron_right, size: 22, color: context.palette.onSurfaceVariant.withOpacity(0.9)),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildToolsPage({required Key key}) {
    return Column(
      key: key,
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Material(
          color: Colors.transparent,
          child: InkWell(
            onTap: () => setState(() => _tools = false),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 10),
              child: Row(
                children: [
                  Icon(Icons.arrow_back_ios_new, size: 16, color: context.palette.primary),
                  const SizedBox(width: 6),
                  Text(
                    l.tr('menu_tools'),
                    style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: context.palette.onSurface),
                  ),
                ],
              ),
            ),
          ),
        ),
        Divider(height: 1, color: context.palette.divider),
        _popoverItem(context, Icons.map, l.tr('map'), 'map'),
        _popoverItem(context, Icons.location_on, l.tr('location'), 'send_location'),
        _popoverItem(context, Icons.hub, l.tr('mesh_topology'), 'mesh'),
        _popoverItem(context, Icons.radar, l.tr('ping'), 'ping'),
        _popoverItem(context, Icons.health_and_safety, l.tr('selftest'), 'selftest'),
      ],
    );
  }

  Widget _popoverItem(BuildContext context, IconData icon, String label, String value) {
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: () => Navigator.pop(context, value),
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

/// Нижний лист выбора TTL (текст и голос).
class _TtlPickerSheet extends StatelessWidget {
  final AppLocalizations l;

  const _TtlPickerSheet({required this.l});

  static const List<int> _minutes = [0, 1, 5, 10];

  @override
  Widget build(BuildContext context) {
    return Container(
      decoration: BoxDecoration(
        color: context.palette.card,
        borderRadius: const BorderRadius.vertical(top: Radius.circular(22)),
        boxShadow: [
          BoxShadow(color: Colors.black.withOpacity(0.2), blurRadius: 24, offset: const Offset(0, -4)),
        ],
      ),
      child: SafeArea(
        top: false,
        child: Padding(
          padding: EdgeInsets.fromLTRB(20, 10, 20, 12 + MediaQuery.of(context).padding.bottom),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Center(
                child: Container(
                  width: 40,
                  height: 4,
                  decoration: BoxDecoration(
                    color: context.palette.onSurfaceVariant.withOpacity(0.35),
                    borderRadius: BorderRadius.circular(2),
                  ),
                ),
              ),
              const SizedBox(height: 18),
              Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Container(
                    padding: const EdgeInsets.all(10),
                    decoration: BoxDecoration(
                      color: context.palette.primary.withOpacity(0.12),
                      borderRadius: BorderRadius.circular(14),
                    ),
                    child: Icon(Icons.auto_delete_outlined, color: context.palette.primary, size: 26),
                  ),
                  const SizedBox(width: 14),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          l.tr('ttl_title'),
                          style: TextStyle(
                            fontSize: 18,
                            fontWeight: FontWeight.w700,
                            color: context.palette.onSurface,
                            letterSpacing: -0.3,
                          ),
                        ),
                        const SizedBox(height: 6),
                        Text(
                          l.tr('ttl_sheet_hint'),
                          style: TextStyle(
                            fontSize: 13,
                            height: 1.4,
                            color: context.palette.onSurfaceVariant.withOpacity(0.95),
                          ),
                        ),
                      ],
                    ),
                  ),
                  IconButton(
                    icon: Icon(Icons.close_rounded, color: context.palette.onSurfaceVariant.withOpacity(0.85)),
                    onPressed: () => Navigator.pop(context),
                    visualDensity: VisualDensity.compact,
                  ),
                ],
              ),
              const SizedBox(height: 22),
              Row(
                children: [
                  Expanded(child: _ttlTile(context, _minutes[0])),
                  const SizedBox(width: 10),
                  Expanded(child: _ttlTile(context, _minutes[1])),
                ],
              ),
              const SizedBox(height: 10),
              Row(
                children: [
                  Expanded(child: _ttlTile(context, _minutes[2])),
                  const SizedBox(width: 10),
                  Expanded(child: _ttlTile(context, _minutes[3])),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _ttlTile(BuildContext context, int minutes) {
    final isNone = minutes == 0;
    final label = isNone ? l.tr('ttl_none') : '${minutes}m';
    return Material(
      color: context.palette.surfaceVariant.withOpacity(0.55),
      borderRadius: BorderRadius.circular(16),
      child: InkWell(
        onTap: () {
          HapticFeedback.selectionClick();
          Navigator.pop(context, minutes);
        },
        borderRadius: BorderRadius.circular(16),
        child: Padding(
          padding: const EdgeInsets.symmetric(vertical: 18, horizontal: 8),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Icon(
                isNone ? Icons.all_inclusive_rounded : Icons.timer_outlined,
                size: 30,
                color: context.palette.primary,
              ),
              const SizedBox(height: 10),
              Text(
                label,
                textAlign: TextAlign.center,
                maxLines: 2,
                style: TextStyle(
                  fontSize: 14,
                  fontWeight: FontWeight.w600,
                  color: context.palette.onSurface,
                  height: 1.15,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

// ── Data classes ──

enum _St { sent, delivered, read, undelivered }

/// Для broadcast: delivered/total — доставлено X из Y
class _Msg {
  final String from;
  final String text;
  final bool isIncoming;
  final bool isLocation;
  final bool isVoice;
  final List<int>? voiceData;
  final int? msgId;
  final String? to;
  final int? rssi;
  final DateTime? deleteAt;
  final _St status;
  final int? delivered;  // broadcast: доставлено X
  final int? total;     // broadcast: всего Y

  _Msg({required this.from, required this.text, required this.isIncoming, this.isLocation = false, this.isVoice = false, this.voiceData, this.msgId, this.to, this.rssi, this.deleteAt, this.status = _St.sent, this.delivered, this.total});

  _Msg copyWith({int? msgId, String? to, _St? status, int? delivered, int? total}) => _Msg(
    from: from, text: text, isIncoming: isIncoming, isLocation: isLocation, isVoice: isVoice, voiceData: voiceData,
    msgId: msgId ?? this.msgId, to: to ?? this.to, rssi: rssi, deleteAt: deleteAt, status: status ?? this.status,
    delivered: delivered ?? this.delivered, total: total ?? this.total,
  );
}
