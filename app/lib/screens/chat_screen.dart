import 'dart:async';
import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:flutter/cupertino.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';
import 'package:geolocator/geolocator.dart';
import '../ble/riftlink_ble.dart';
import '../voice/voice_service.dart';
import '../contacts/contacts_service.dart';
import '../l10n/app_localizations.dart';
import 'map_screen.dart';
import 'mesh_screen.dart';
import 'contacts_screen.dart';
import 'groups_screen.dart';
import 'settings_screen.dart';
import 'scan_screen.dart';
import '../locale_notifier.dart';

import '../theme/app_theme.dart';

class ChatScreen extends StatefulWidget {
  final RiftLinkBle ble;
  const ChatScreen({super.key, required this.ble});
  @override
  State<ChatScreen> createState() => _ChatScreenState();
}

class _ChatScreenState extends State<ChatScreen> with SingleTickerProviderStateMixin {
  final _controller = TextEditingController();
  final _scrollController = ScrollController();
  final _messages = <_Msg>[];
  StreamSubscription? _sub;
  String _nodeId = '';
  String? _nickname;
  String _region = 'EU';
  int? _channel;
  String? _version;
  int? _sf;
  int? _offlinePending;
  bool _gpsPresent = false;
  bool _gpsEnabled = false;
  bool _gpsFix = false;
  bool _powersave = false;
  int? _batteryMv;
  List<String> _neighbors = [];
  List<int> _neighborsRssi = [];
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
  Timer? _ttlTimer;

  List<_Msg> get _visibleMessages =>
      _messages.where((m) => m.deleteAt == null || DateTime.now().isBefore(m.deleteAt!)).toList();

  // ── Lifecycle ──

  @override
  void initState() {
    super.initState();
    _controller.addListener(_onTextChanged);
    _listenEvents();
    _loadContactNicknames();
    localeNotifier.addListener(_onLocaleChanged);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _sendReadForUnread();
      _sendLangToFirmware();
      VoiceService.requestPermission();
    });
    _ttlTimer = Timer.periodic(const Duration(minutes: 1), (_) {
      if (mounted && _messages.any((m) => m.deleteAt != null && DateTime.now().isAfter(m.deleteAt!))) setState(() {});
    });
  }

  void _onTextChanged() {
    final hasText = _controller.text.trim().isNotEmpty;
    if (hasText != _hasText && mounted) setState(() => _hasText = hasText);
  }

  @override
  void dispose() {
    _controller.removeListener(_onTextChanged);
    localeNotifier.removeListener(_onLocaleChanged);
    _ttlTimer?.cancel();
    _voiceRecordTicker?.dispose();
    _sub?.cancel();
    _controller.dispose();
    _scrollController.dispose();
    super.dispose();
  }

  void _onLocaleChanged() { if (mounted) _sendLangToFirmware(); }
  void _sendLangToFirmware() { if (widget.ble.isConnected) widget.ble.setLang(AppLocalizations.currentLocale.languageCode); }

  Future<void> _loadContactNicknames() async {
    final contacts = await ContactsService.load();
    if (!mounted) return;
    setState(() { _contactNicknames = {for (var c in contacts) c.id: c.nickname}; });
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

  // ── BLE Events ──

  void _listenEvents() {
    _sub?.cancel();
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      if (evt is RiftLinkMsgEvent) {
        setState(() {
          final ttl = evt.ttlMinutes ?? 0;
          _messages.add(_Msg(from: evt.from, text: evt.text, isIncoming: true, msgId: evt.msgId, rssi: evt.rssi,
              deleteAt: ttl > 0 ? DateTime.now().add(Duration(minutes: ttl)) : null));
        });
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
      } else if (evt is RiftLinkInfoEvent) {
        setState(() {
          _nodeId = evt.id; _nickname = evt.nickname?.isNotEmpty == true ? evt.nickname : null;
          _region = evt.region; _channel = evt.channel; _version = evt.version; _sf = evt.sf;
          _offlinePending = evt.offlinePending; _gpsPresent = evt.gpsPresent; _gpsEnabled = evt.gpsEnabled;
          _gpsFix = evt.gpsFix; _powersave = evt.powersave; _neighbors = evt.neighbors;
          _neighborsRssi = evt.neighborsRssi; _routes = evt.routes; _groups = evt.groups;
        });
      } else if (evt is RiftLinkRoutesEvent) { setState(() => _routes = evt.routes); }
      else if (evt is RiftLinkGroupsEvent) { setState(() => _groups = evt.groups); }
      else if (evt is RiftLinkTelemetryEvent) {
        setState(() {
          if (evt.from == _nodeId && evt.batteryMv > 0) _batteryMv = evt.batteryMv;
          _messages.add(_Msg(from: evt.from, text: '🔋 ${(evt.batteryMv / 1000).toStringAsFixed(2)}V, ${evt.heapKb} KB', isIncoming: true));
        });
        _scrollToBottom();
      } else if (evt is RiftLinkLocationEvent) {
        setState(() { _messages.add(_Msg(from: evt.from, text: '📍 ${evt.lat.toStringAsFixed(5)}, ${evt.lon.toStringAsFixed(5)}', isIncoming: true, isLocation: true)); });
        _scrollToBottom();
      } else if (evt is RiftLinkOtaEvent) { _showOtaDialog(evt.ip, evt.ssid, evt.password); }
      else if (evt is RiftLinkRegionEvent) { setState(() { _region = evt.region; _channel = evt.channel; }); }
      else if (evt is RiftLinkNeighborsEvent) { setState(() { _neighbors = evt.neighbors; _neighborsRssi = evt.rssi; }); }
      else if (evt is RiftLinkPongEvent) { ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(context.l10n.tr('link_ok', {'from': evt.from})))); }
      else if (evt is RiftLinkErrorEvent) { ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('${context.l10n.tr('error')}: ${evt.msg}'), backgroundColor: AppColors.error)); }
      else if (evt is RiftLinkWifiEvent) { ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(evt.connected ? 'WiFi: ${evt.ssid} — ${evt.ip}' : 'WiFi: не подключено'))); }
      else if (evt is RiftLinkGpsEvent) { setState(() { _gpsPresent = evt.present; _gpsEnabled = evt.enabled; _gpsFix = evt.hasFix; }); }
      else if (evt is RiftLinkInviteEvent) { _showInviteDialog(evt.id, evt.pubKey); }
      else if (evt is RiftLinkSelftestEvent) {
        final ok = evt.radioOk && evt.displayOk;
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Selftest: ${evt.radioOk ? "✓" : "✗"} радио, ${evt.displayOk ? "✓" : "✗"} дисплей. ${evt.batteryMv}mV'), backgroundColor: ok ? null : AppColors.error));
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
    });
  }

  void _scrollToBottom() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scrollController.hasClients) _scrollController.animateTo(_scrollController.position.maxScrollExtent, duration: const Duration(milliseconds: 200), curve: Curves.easeOut);
    });
  }

  // ── Actions ──

  Future<void> _send({int ttlMinutes = 0}) async {
    final text = _controller.text.trim();
    if (text.isEmpty) return;
    _controller.clear();
    setState(() { _messages.add(_Msg(from: _nodeId, text: text, isIncoming: false, to: _unicastTo)); });
    _scrollToBottom();
    await widget.ble.send(text: text, to: _unicastTo, group: _group > 0 ? _group : null, ttlMinutes: ttlMinutes);
  }

  void _showTtlSendMenu() {
    final text = _controller.text.trim();
    if (text.isEmpty) return;
    FocusScope.of(context).unfocus();
    HapticFeedback.mediumImpact();
    showDialog<void>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: AppColors.card,
        title: Text(context.l10n.tr('ttl_title'), style: const TextStyle(color: AppColors.onSurface)),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            for (final v in [0, 1, 5, 10])
              ListTile(
                title: Text(v == 0 ? context.l10n.tr('ttl_none') : '${v}m', style: const TextStyle(fontSize: 16, color: AppColors.onSurface)),
                onTap: () { Navigator.pop(ctx); _send(ttlMinutes: v); },
              ),
          ],
        ),
      ),
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
      const chunkSize = 384;
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
    return showModalBottomSheet<String>(
      context: context,
      backgroundColor: Colors.transparent,
      isScrollControlled: true,
      builder: (ctx) => Container(
        decoration: const BoxDecoration(
          color: AppColors.card,
          borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
        ),
        child: SafeArea(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Padding(
                padding: const EdgeInsets.all(16),
                child: Text(context.l10n.tr('send_voice'), style: const TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: AppColors.onSurface)),
              ),
              ..._neighbors.map((id) {
                final label = _contactNicknames[id]?.isNotEmpty == true ? '${_contactNicknames[id]} ($id)' : id;
                return ListTile(title: Text(label, style: const TextStyle(fontFamily: 'monospace', fontSize: 13, color: AppColors.onSurface)), onTap: () => Navigator.pop(ctx, id));
              }),
              ListTile(title: Text(context.l10n.tr('cancel'), style: const TextStyle(color: AppColors.onSurfaceVariant)), onTap: () => Navigator.pop(ctx)),
            ],
          ),
        ),
      ),
    );
  }

  Future<int?> _pickVoiceTtlDialog() async {
    FocusScope.of(context).unfocus();
    HapticFeedback.mediumImpact();
    return showDialog<int>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: AppColors.card,
        title: Text(context.l10n.tr('ttl_title'), style: const TextStyle(color: AppColors.onSurface)),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            for (final v in [0, 1, 5, 10])
              ListTile(
                title: Text(v == 0 ? context.l10n.tr('ttl_none') : '${v}m', style: const TextStyle(fontSize: 16, color: AppColors.onSurface)),
                onTap: () => Navigator.pop(ctx, v),
              ),
          ],
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx),
            child: Text(context.l10n.tr('cancel'), style: const TextStyle(color: AppColors.onSurfaceVariant)),
          ),
        ],
      ),
    );
  }

  void _showSnack(String text) => ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(text)));

  Future<void> _startOta() async {
    if (!widget.ble.isConnected) return;
    final ok = await widget.ble.sendOta();
    if (mounted) _showSnack(ok ? context.l10n.tr('ota_started') : context.l10n.tr('ota_failed'));
  }

  void _showPingDialog({String? prefilledId}) {
    final c = TextEditingController(text: prefilledId ?? '');
    showDialog(context: context, builder: (ctx) => AlertDialog(
      title: Text(context.l10n.tr('ping'), style: const TextStyle(color: AppColors.onSurface)),
      content: TextField(controller: c, decoration: const InputDecoration(hintText: 'A1B2C3D4'), maxLength: 8, autofocus: true),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx), child: Text(context.l10n.tr('cancel'))),
        ElevatedButton(
          style: ElevatedButton.styleFrom(backgroundColor: AppColors.primary, foregroundColor: Colors.white),
          onPressed: () async {
            final id = c.text.trim().toUpperCase();
            if (id.length != 8) { _showSnack(context.l10n.tr('ping_invalid')); return; }
            Navigator.pop(ctx);
            final ok = await widget.ble.sendPing(id);
            if (mounted) _showSnack(ok ? 'PING → $id' : context.l10n.tr('error'));
          },
          child: const Text('Ping'),
        ),
      ],
    ));
  }

  void _showAddContactDialog(String id) {
    final nc = TextEditingController(text: _contactNicknames[id] ?? '');
    showDialog(context: context, builder: (ctx) => AlertDialog(
      title: Text(context.l10n.tr('add_contact'), style: const TextStyle(color: AppColors.onSurface)),
      content: TextField(controller: nc, decoration: InputDecoration(labelText: context.l10n.tr('contact_nickname')), maxLength: 16, autofocus: true),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx), child: Text(context.l10n.tr('cancel'))),
        ElevatedButton(
          style: ElevatedButton.styleFrom(backgroundColor: AppColors.primary, foregroundColor: Colors.white),
          onPressed: () async { final nick = nc.text.trim(); Navigator.pop(ctx); await ContactsService.add(Contact(id: id, nickname: nick)); await _loadContactNicknames(); if (mounted) _showSnack(context.l10n.tr('contact_added')); },
          child: Text(context.l10n.tr('save')),
        ),
      ],
    ));
  }

  void _showInviteDialog(String id, String pubKey) {
    final data = '{"id":"$id","pubKey":"$pubKey"}';
    showDialog(context: context, builder: (ctx) => AlertDialog(
      title: Text(context.l10n.tr('invite_created'), style: const TextStyle(color: AppColors.onSurface)),
      content: SingleChildScrollView(child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text('ID: $id', style: const TextStyle(fontFamily: 'monospace', fontSize: 12, color: AppColors.onSurface)),
        const SizedBox(height: 8),
        Text('PubKey: ${pubKey.length > 40 ? '${pubKey.substring(0, 40)}…' : pubKey}', style: const TextStyle(fontFamily: 'monospace', fontSize: 11, color: AppColors.onSurface)),
        const SizedBox(height: 16),
        ElevatedButton.icon(
          style: ElevatedButton.styleFrom(backgroundColor: AppColors.primary, foregroundColor: Colors.white),
          onPressed: () { Clipboard.setData(ClipboardData(text: data)); _showSnack(context.l10n.tr('copied')); },
          icon: const Icon(Icons.copy, size: 18), label: Text(context.l10n.tr('copy')),
        ),
      ])),
      actions: [TextButton(onPressed: () => Navigator.pop(ctx), child: Text(context.l10n.tr('ok')))],
    ));
  }

  void _showOtaDialog(String ip, String ssid, String password) {
    showDialog(context: context, builder: (ctx) => AlertDialog(
      title: const Text('OTA', style: TextStyle(color: AppColors.onSurface)),
      content: SingleChildScrollView(child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text(context.l10n.tr('ota_connect'), style: const TextStyle(color: AppColors.onSurface)),
        const SizedBox(height: 8),
        Text('SSID: $ssid', style: const TextStyle(fontWeight: FontWeight.bold, color: AppColors.onSurface)),
        Text('Pass: $password', style: const TextStyle(color: AppColors.onSurface)),
        const SizedBox(height: 12),
        Text(context.l10n.tr('ota_then'), style: const TextStyle(color: AppColors.onSurface)),
        const SizedBox(height: 4),
        SelectableText('cd firmware\npio run -t upload -e heltec_v3_ota', style: const TextStyle(fontFamily: 'monospace', fontSize: 12, color: AppColors.onSurface)),
        const SizedBox(height: 8),
        Text('IP: $ip', style: const TextStyle(color: AppColors.onSurfaceVariant, fontSize: 12)),
      ])),
      actions: [TextButton(onPressed: () => Navigator.pop(ctx), child: Text(context.l10n.tr('ok')))],
    ));
  }

  // ── Build ──

  Future<void> _showAppMenu(BuildContext context, AppLocalizations l) async {
    FocusScope.of(context).unfocus();
    final value = await showModalBottomSheet<String>(
      context: context,
      useRootNavigator: true,
      backgroundColor: Colors.transparent,
      isScrollControlled: true,
      builder: (ctx) => Container(
        decoration: const BoxDecoration(
          color: AppColors.card,
          borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
        ),
        child: SafeArea(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              ListTile(leading: const Icon(Icons.people), title: Text(l.tr('contacts')), onTap: () => Navigator.pop(ctx, 'contacts')),
              ListTile(leading: const Icon(Icons.group), title: Text(l.tr('groups')), onTap: () => Navigator.pop(ctx, 'groups')),
              ListTile(leading: const Icon(Icons.radar), title: Text(l.tr('ping')), onTap: () => Navigator.pop(ctx, 'ping')),
              ListTile(leading: const Icon(Icons.update), title: const Text('OTA'), onTap: () => Navigator.pop(ctx, 'ota')),
              ListTile(leading: const Icon(Icons.settings), title: Text(l.tr('settings')), onTap: () => Navigator.pop(ctx, 'settings')),
            ],
          ),
        ),
      ),
    );
    if (value != null && mounted) _onMenuSelected(value);
  }

  void _onMenuSelected(String? value) {
    if (value == null || !mounted) return;
    FocusScope.of(context).unfocus();
    switch (value) {
      case 'contacts':
        Navigator.push(context, MaterialPageRoute(builder: (_) => ContactsScreen(neighbors: _neighbors))).then((_) => _loadContactNicknames());
        break;
      case 'groups':
        Navigator.push(context, MaterialPageRoute(builder: (_) => GroupsScreen(ble: widget.ble, initialGroups: _groups))).then((_) => widget.ble.getGroups());
        break;
      case 'ping':
        _showPingDialog();
        break;
      case 'ota':
        _startOta();
        break;
      case 'settings':
        if (widget.ble.isConnected) {
          Navigator.push(context, MaterialPageRoute(builder: (_) => SettingsScreen(
            ble: widget.ble, nodeId: _nodeId, nickname: _nickname, region: _region, channel: _channel,
            gpsPresent: _gpsPresent, gpsEnabled: _gpsEnabled, gpsFix: _gpsFix, powersave: _powersave,
            offlinePending: _offlinePending, batteryMv: _batteryMv,
            onDisconnect: () async { await widget.ble.disconnect(); if (!mounted) return; Navigator.of(context).pushAndRemoveUntil(MaterialPageRoute(builder: (_) => const ScanScreen()), (r) => false); },
            onNicknameChanged: (n) => setState(() => _nickname = n),
            onRegionChanged: (r, c) => setState(() { _region = r; _channel = c; }),
            onPowersaveChanged: (v) => setState(() => _powersave = v),
            onGpsChanged: (v) => setState(() => _gpsEnabled = v),
          ))).then((_) => setState(() {}));
        } else {
          ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(context.l10n.tr('connect_first'))));
        }
        break;
    }
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    return Scaffold(
      resizeToAvoidBottomInset: true,
      backgroundColor: AppColors.card,
      appBar: AppBar(
        title: Text(l.tr('app_title'), style: const TextStyle(fontWeight: FontWeight.w600)),
        backgroundColor: AppColors.surfaceVariant,
        foregroundColor: AppColors.onSurface,
        elevation: 0,
        actions: [
          IconButton(
            icon: const Icon(Icons.map),
            onPressed: () => Navigator.push(context, MaterialPageRoute(builder: (_) => MapScreen(ble: widget.ble))),
            tooltip: l.tr('map'),
          ),
          IconButton(
            icon: const Icon(Icons.hub),
            onPressed: () => Navigator.push(context, MaterialPageRoute(builder: (_) => MeshScreen(ble: widget.ble, nodeId: _nodeId, neighbors: _neighbors, neighborsRssi: _neighborsRssi, routes: _routes))),
            tooltip: l.tr('mesh_topology'),
          ),
          IconButton(
            icon: const Icon(Icons.more_vert),
            tooltip: l.tr('settings'),
            onPressed: () => _showAppMenu(context, l),
          ),
        ],
      ),
      body: SafeArea(
        top: false,
        child: Stack(
          children: [
            Positioned.fill(
              child: IgnorePointer(
                child: ColoredBox(
                  color: AppColors.surface,
                  child: CustomPaint(painter: _MeshBackgroundPainter()),
                ),
              ),
            ),
            Column(
              mainAxisSize: MainAxisSize.max,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
              color: AppColors.surface,
              child: Row(
                children: [
                  Icon(widget.ble.isConnected ? Icons.bluetooth_connected : Icons.bluetooth_disabled, size: 18, color: widget.ble.isConnected ? AppColors.success : AppColors.onSurfaceVariant),
                  const SizedBox(width: 8),
                  Text(widget.ble.isConnected ? l.tr('connected') : l.tr('disconnected'), style: TextStyle(fontSize: 13, color: widget.ble.isConnected ? AppColors.success : AppColors.onSurfaceVariant)),
                  if (_nodeId.isNotEmpty) ...[const Spacer(), Text(_nickname ?? _nodeId, style: const TextStyle(fontSize: 13, color: AppColors.onSurfaceVariant, fontFamily: 'monospace'), overflow: TextOverflow.ellipsis)],
                  const Spacer(),
                  GestureDetector(
                    onTap: widget.ble.isConnected ? () => setState(() => _group = _group == 0 ? 1 : 0) : null,
                    child: Container(
                      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                      decoration: BoxDecoration(
                        color: _group > 0 ? AppColors.primary.withOpacity(0.2) : AppColors.surfaceVariant,
                        borderRadius: BorderRadius.circular(12),
                        border: Border.all(color: _group > 0 ? AppColors.primary : AppColors.divider),
                      ),
                      child: Row(mainAxisSize: MainAxisSize.min, children: [
                        Icon(_group == 0 ? Icons.public : Icons.group, size: 16, color: _group > 0 ? AppColors.primary : AppColors.onSurfaceVariant),
                        const SizedBox(width: 4),
                        Text(_group == 0 ? 'BC' : 'G$_group', style: TextStyle(fontSize: 13, fontWeight: FontWeight.w500, color: _group > 0 ? AppColors.primary : AppColors.onSurface)),
                      ]),
                    ),
                  ),
                ],
              ),
            ),
            const Divider(height: 1, color: AppColors.divider),
            if (_nodeId.isNotEmpty && _neighbors.isNotEmpty) _buildRecipientBar(l),
            Expanded(
              child: Stack(
                children: [
                  Container(
                    color: Colors.transparent,
                    child: _visibleMessages.isEmpty
                      ? Center(child: Text(l.tr('no_messages'), style: const TextStyle(color: AppColors.onSurfaceVariant, fontSize: 15)))
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
                              AppColors.surface.withOpacity(0),
                              AppColors.surface.withOpacity(0.95),
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
          ],
        ),
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

    return Container(
      padding: const EdgeInsets.fromLTRB(20, 10, 8, 10),
      decoration: BoxDecoration(
        color: AppColors.surfaceVariant.withOpacity(0.92),
        border: const Border(top: BorderSide(color: AppColors.divider, width: 0.5)),
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.end,
        children: [
          Expanded(
            child: Container(
              decoration: BoxDecoration(
                color: AppColors.card,
                borderRadius: BorderRadius.circular(24),
                border: Border.all(color: AppColors.divider),
              ),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.center,
                children: [
                  Padding(
                    padding: const EdgeInsets.only(left: 2),
                    child: _inputIcon(Icons.location_on, widget.ble.isConnected && !_locationLoading ? _sendLocation : null, loading: _locationLoading, tooltip: l.tr('location')),
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
                              decoration: const BoxDecoration(color: AppColors.error, shape: BoxShape.circle),
                            ),
                            const SizedBox(width: 10),
                            Text(timeStr, style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w500, color: AppColors.onSurface, fontFamily: 'monospace')),
                            const SizedBox(width: 16),
                            Expanded(
                              child: Text(
                                l.tr('voice_swipe_cancel'),
                                style: const TextStyle(fontSize: 14, color: AppColors.onSurfaceVariant),
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
                        style: const TextStyle(color: AppColors.onSurface, fontSize: 16),
                        placeholder: l.tr('message_hint'),
                        placeholderStyle: const TextStyle(color: AppColors.onSurfaceVariant),
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

  // ── Recipient bar ──

  Widget _buildRecipientBar(AppLocalizations l) {
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 8),
      decoration: const BoxDecoration(color: AppColors.surface, border: Border(bottom: BorderSide(color: AppColors.divider))),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
        Text('${l.tr('to')}', style: const TextStyle(fontSize: 11, fontWeight: FontWeight.w600, color: AppColors.onSurfaceVariant)),
        const SizedBox(height: 4),
        Wrap(spacing: 6, runSpacing: 4, children: [
          _rcpChip(l.tr('broadcast'), _unicastTo == null && _group == 0, () => setState(() { _unicastTo = null; _group = 0; })),
          ..._groups.map((gid) => _rcpChip('${l.tr('group')} $gid', _group == gid && _unicastTo == null, () => setState(() { _unicastTo = null; _group = _group == gid ? 0 : gid; }))),
          ..._neighbors.asMap().entries.map((e) {
            final id = e.value; final rssi = e.key < _neighborsRssi.length ? _neighborsRssi[e.key] : 0;
            final label = _contactNicknames[id]?.isNotEmpty == true ? _contactNicknames[id]! : (rssi != 0 ? '$id ($rssi)' : id);
            return GestureDetector(
              onLongPress: () => _showAddContactDialog(id),
              child: _rcpChip(label, _unicastTo == id, () => setState(() { _unicastTo = _unicastTo == id ? null : id; _group = 0; })),
            );
          }),
        ]),
      ]),
    );
  }

  Widget _rcpChip(String label, bool selected, VoidCallback onTap) {
    return GestureDetector(
      onTap: onTap,
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
        decoration: BoxDecoration(
          color: selected ? AppColors.primary.withOpacity(0.2) : AppColors.card,
          borderRadius: BorderRadius.circular(16),
          border: Border.all(color: selected ? AppColors.primary : AppColors.divider),
        ),
        child: Text(label, style: TextStyle(fontSize: 12, color: selected ? AppColors.primary : AppColors.onSurface, fontWeight: selected ? FontWeight.w600 : FontWeight.normal)),
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
          color: mine ? AppColors.primary.withOpacity(0.25) : AppColors.card,
          borderRadius: BorderRadius.circular(14),
          border: Border.all(color: mine ? AppColors.primary : AppColors.divider),
        ),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
          if (m.isIncoming && m.from.isNotEmpty)
            Padding(padding: const EdgeInsets.only(bottom: 2), child: Text(
              _contactNicknames[m.from]?.isNotEmpty == true ? '${_contactNicknames[m.from]} (${m.from})' : m.from,
              style: const TextStyle(fontSize: 11, fontWeight: FontWeight.w600, color: AppColors.primary),
            )),
          Row(mainAxisSize: MainAxisSize.min, children: [
            Flexible(child: Text(m.text, style: const TextStyle(color: AppColors.onSurface, fontSize: 14))),
            if (!m.isIncoming && m.to != null) Padding(padding: const EdgeInsets.only(left: 6), child: Text(
              m.status == _St.read ? '✓✓✓' : m.status == _St.delivered ? '✓✓' : '✓',
              style: TextStyle(fontSize: 10, color: m.status == _St.read ? AppColors.primary : AppColors.onSurfaceVariant),
            )),
            if (m.isIncoming && m.rssi != null && m.rssi != 0) Padding(padding: const EdgeInsets.only(left: 6), child: Text('${m.rssi}dBm', style: const TextStyle(fontSize: 9, color: AppColors.onSurfaceVariant))),
            if (m.isVoice && m.voiceData != null) IconButton(
              icon: const Icon(Icons.play_circle_outline, color: AppColors.primary, size: 22),
              onPressed: () async { if (m.voiceData != null && m.voiceData!.isNotEmpty) await VoiceService.play(m.voiceData!); },
              tooltip: context.l10n.tr('play'), padding: EdgeInsets.zero, constraints: const BoxConstraints(),
            ),
          ]),
        ]),
      ),
    );
  }

  // ── Компактные иконки в строке ввода (как в мессенджерах) ──

  Widget _inputIcon(IconData icon, VoidCallback? onTap, {bool loading = false, bool active = false, String? tooltip, String? badge}) {
    final enabled = onTap != null;
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: onTap,
      child: Tooltip(
        message: tooltip ?? '',
        child: SizedBox(
          width: 40,
          height: 40,
          child: Stack(
            alignment: Alignment.center,
            children: [
              if (loading)
                const SizedBox(width: 24, height: 24, child: CircularProgressIndicator(strokeWidth: 2, color: AppColors.primary))
              else
                Icon(icon, size: 24, color: active ? AppColors.error : (enabled ? AppColors.onSurfaceVariant : AppColors.divider)),
              if (badge != null && badge.isNotEmpty)
                Positioned(
                  right: 0,
                  top: 0,
                  child: Container(
                    padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 1),
                    decoration: BoxDecoration(color: AppColors.primary, borderRadius: BorderRadius.circular(8)),
                    child: Text(badge, style: const TextStyle(fontSize: 9, color: Colors.white, fontWeight: FontWeight.w600)),
                  ),
                ),
            ],
          ),
        ),
      ),
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
            color: enabled ? AppColors.card : AppColors.surfaceVariant,
            borderRadius: BorderRadius.circular(24),
            border: Border.all(color: AppColors.divider),
          ),
          child: Row(mainAxisSize: MainAxisSize.min, children: [
            if (loading) const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2, color: AppColors.primary))
            else Icon(icon, size: 22, color: iconColor ?? (enabled ? AppColors.onSurface : AppColors.onSurfaceVariant)),
            if (label != null) ...[const SizedBox(width: 6), Text(label, style: TextStyle(fontSize: 15, fontWeight: FontWeight.w500, color: enabled ? AppColors.onSurface : AppColors.onSurfaceVariant))],
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
            color: enabled ? AppColors.primary : AppColors.divider,
            shape: BoxShape.circle,
          ),
          alignment: Alignment.center,
          child: Icon(widget.icon, size: widget.iconSize, color: Colors.white),
        ),
      ),
    );
  }
}

/// Незатейливый абстрактный фон: узлы сети и тонкие линии связи
class _MeshBackgroundPainter extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    const spacing = 56.0;
    const dotRadius = 1.2;
    const lineOpacity = 0.04;
    const dotOpacity = 0.06;
    final paint = Paint()..color = AppColors.primary.withOpacity(dotOpacity);
    final linePaint = Paint()
      ..color = AppColors.primary.withOpacity(lineOpacity)
      ..strokeWidth = 0.8
      ..style = PaintingStyle.stroke;

    final cols = (size.width / spacing).floor() + 2;
    final rows = (size.height / spacing).floor() + 2;
    final points = <Offset>[];

    for (var r = 0; r < rows; r++) {
      for (var c = 0; c < cols; c++) {
        final x = c * spacing + (r.isOdd ? spacing * 0.5 : 0);
        final y = r * spacing * 0.85;
        if (x <= size.width + spacing && y <= size.height + spacing) {
          points.add(Offset(x, y));
        }
      }
    }

    for (final p in points) {
      canvas.drawCircle(p, dotRadius, paint);
    }

    for (var i = 0; i < points.length; i++) {
      for (var j = i + 1; j < points.length; j++) {
        if ((points[i] - points[j]).distance < spacing * 1.5) {
          canvas.drawLine(points[i], points[j], linePaint);
        }
      }
    }
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}

// ── Data classes ──

enum _St { sent, delivered, read }

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

  _Msg({required this.from, required this.text, required this.isIncoming, this.isLocation = false, this.isVoice = false, this.voiceData, this.msgId, this.to, this.rssi, this.deleteAt, this.status = _St.sent});

  _Msg copyWith({int? msgId, String? to, _St? status}) => _Msg(
    from: from, text: text, isIncoming: isIncoming, isLocation: isLocation, isVoice: isVoice, voiceData: voiceData,
    msgId: msgId ?? this.msgId, to: to ?? this.to, rssi: rssi, deleteAt: deleteAt, status: status ?? this.status,
  );
}
