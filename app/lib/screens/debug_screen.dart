import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../ble/riftlink_ble.dart';
import '../ble/riftlink_ble_scope.dart';
import '../contacts/contacts_service.dart';
import '../l10n/app_localizations.dart';
import '../prefs/mesh_prefs.dart';
import '../theme/app_theme.dart';

class DebugScreen extends StatefulWidget {
  const DebugScreen({super.key});

  @override
  State<DebugScreen> createState() => _DebugScreenState();
}

class _DebugScreenState extends State<DebugScreen> {
  static const double _kScreenPadH = 16;
  static const double _kScreenPadV = 12;
  static const double _kScreenPadBottom = 24;
  static const double _kSectionGap = 20;
  static const double _kRowGap = 8;
  static const double _kCardRadius = 12;

  int _voiceAcceptMaxAvgLossPercent = 20;
  int _voiceAcceptMaxHardLossPercent = 15;
  int _voiceAcceptMinSessions = 5;
  StreamSubscription<RiftLinkEvent>? _sub;
  final List<String> _lines = <String>[];
  final Map<int, _RelayTraceRow> _traceByMsgId = <int, _RelayTraceRow>{};
  final Map<String, _VoiceRxDebugStats> _voiceStatsByFrom = <String, _VoiceRxDebugStats>{};
  final Map<String, _VoiceRxAssembly> _voiceAssemblies = <String, _VoiceRxAssembly>{};
  Timer? _voiceCleanupTimer;
  Map<String, String> _nickById = const {};

  @override
  void initState() {
    super.initState();
    _loadVoiceAcceptancePrefs();
    _loadContactNicknames();
    _voiceCleanupTimer = Timer.periodic(
      const Duration(seconds: 5),
      (_) => _cleanupStaleVoiceAssemblies(),
    );
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final ble = RiftLinkBleScope.of(context);
      _sub = ble.events.listen((evt) {
        if (!mounted) return;
        final t = DateTime.now().toIso8601String().substring(11, 19);
        setState(() {
          _lines.add('[$t] ${_formatEvent(evt)}');
          if (_lines.length > 200) {
            _lines.removeRange(0, _lines.length - 200);
          }
          _trackRelayTrace(evt);
          _trackVoice(evt);
        });
      });
    });
  }

  Future<void> _loadVoiceAcceptancePrefs() async {
    final avg = await MeshPrefs.getVoiceAcceptMaxAvgLossPercent();
    final hard = await MeshPrefs.getVoiceAcceptMaxHardLossPercent();
    final minSessions = await MeshPrefs.getVoiceAcceptMinSessions();
    if (!mounted) return;
    setState(() {
      _voiceAcceptMaxAvgLossPercent = avg;
      _voiceAcceptMaxHardLossPercent = hard;
      _voiceAcceptMinSessions = minSessions;
    });
  }

  Future<void> _loadContactNicknames() async {
    final contacts = await ContactsService.load();
    if (!mounted) return;
    setState(() => _nickById = ContactsService.buildNicknameMap(contacts));
  }

  @override
  void dispose() {
    _voiceCleanupTimer?.cancel();
    _sub?.cancel();
    super.dispose();
  }

  String _formatEvent(RiftLinkEvent evt) {
    if (evt is RiftLinkMsgEvent) return 'msg from=${_displayNodeLabel(evt.from)} lane=${evt.lane} type=${evt.type} text=${evt.text}';
    if (evt is RiftLinkSentEvent) return 'sent to=${_displayNodeLabel(evt.to)} msgId=${evt.msgId}';
    if (evt is RiftLinkReadEvent) return 'read from=${_displayNodeLabel(evt.from)} msgId=${evt.msgId}';
    if (evt is RiftLinkDeliveredEvent) return 'delivered from=${_displayNodeLabel(evt.from)} msgId=${evt.msgId}';
    if (evt is RiftLinkUndeliveredEvent) return 'undelivered to=${_displayNodeLabel(evt.to)} msgId=${evt.msgId}';
    if (evt is RiftLinkInfoEvent) return 'info id=${_displayNodeLabel(evt.id)} neighbors=${evt.neighbors.length} mode=${evt.radioMode}';
    if (evt is RiftLinkPongEvent) return 'pong from=${_displayNodeLabel(evt.from)} rssi=${evt.rssi ?? 0}';
    if (evt is RiftLinkRoutesEvent) return 'routes count=${evt.routes.length}';
    if (evt is RiftLinkRelayProofEvent) return 'relayProof by=${_displayNodeLabel(evt.relayedBy)} from=${_displayNodeLabel(evt.from)} to=${_displayNodeLabel(evt.to)} pkt=${evt.pktId} op=${evt.opcode}';
    if (evt is RiftLinkTimeCapsuleQueuedEvent) return 'timeCapsuleQueued to=${evt.to == null ? "-" : _displayNodeLabel(evt.to!)} trigger=${evt.trigger} at=${evt.triggerAtMs ?? 0}';
    if (evt is RiftLinkTimeCapsuleReleasedEvent) return 'timeCapsuleReleased to=${_displayNodeLabel(evt.to)} msgId=${evt.msgId} trigger=${evt.trigger}';
    if (evt is RiftLinkErrorEvent) return 'error code=${evt.code} msg=${evt.msg}';
    return evt.runtimeType.toString();
  }

  String _shortLabel(String raw) {
    final label = _displayNodeLabel(raw);
    if (label != raw.toUpperCase()) return label;
    final s = raw.toUpperCase();
    if (s.length <= 4) return s;
    return s.substring(0, 4);
  }

  String _displayNodeLabel(String raw) {
    return ContactsService.displayNodeLabel(raw, _nickById);
  }

  static bool _sameNodeId(String a, String b) {
    if (a.isEmpty || b.isEmpty) return false;
    final an = a.toUpperCase();
    final bn = b.toUpperCase();
    return an == bn || an.startsWith(bn) || bn.startsWith(an);
  }

  static String _outcomeWithRatio(String base, int? delivered, int? total) {
    if (total != null && total > 0) {
      return '$base ${delivered ?? 0}/$total';
    }
    return base;
  }

  void _trackRelayTrace(RiftLinkEvent evt) {
    final now = DateTime.now();
    if (evt is RiftLinkSentEvent) {
      final row = _traceByMsgId[evt.msgId] ?? _RelayTraceRow(msgId: evt.msgId);
      row.to = evt.to;
      row.outcome = 'sent';
      row.updatedAt = now;
      _traceByMsgId[evt.msgId] = row;
      return;
    }
    if (evt is RiftLinkDeliveredEvent) {
      final row = _traceByMsgId[evt.msgId] ?? _RelayTraceRow(msgId: evt.msgId);
      row.to = evt.from;
      row.outcome = 'delivered';
      row.updatedAt = now;
      _traceByMsgId[evt.msgId] = row;
      return;
    }
    if (evt is RiftLinkReadEvent) {
      final row = _traceByMsgId[evt.msgId] ?? _RelayTraceRow(msgId: evt.msgId);
      row.to = evt.from;
      row.outcome = 'read';
      row.updatedAt = now;
      _traceByMsgId[evt.msgId] = row;
      return;
    }
    if (evt is RiftLinkUndeliveredEvent) {
      final row = _traceByMsgId[evt.msgId] ?? _RelayTraceRow(msgId: evt.msgId);
      row.to = evt.to;
      row.outcome = _outcomeWithRatio('undelivered', evt.delivered, evt.total);
      row.updatedAt = now;
      _traceByMsgId[evt.msgId] = row;
      return;
    }
    if (evt is RiftLinkBroadcastDeliveryEvent) {
      final row = _traceByMsgId[evt.msgId] ?? _RelayTraceRow(msgId: evt.msgId);
      row.outcome = _outcomeWithRatio(evt.delivered > 0 ? 'delivered' : 'undelivered', evt.delivered, evt.total);
      row.updatedAt = now;
      _traceByMsgId[evt.msgId] = row;
      return;
    }
    if (evt is RiftLinkRelayProofEvent) {
      _traceByMsgId.forEach((msgId, row) {
        if ((msgId & 0xFFFF) != evt.pktId) return;
        if (row.to != null && row.to!.isNotEmpty && !_sameNodeId(row.to!, evt.to)) return;
        final relay = _shortLabel(evt.relayedBy);
        if (!row.relays.contains(relay) && row.relays.length < 8) {
          row.relays.add(relay);
        }
        row.updatedAt = now;
      });
    }
  }

  void _trackVoice(RiftLinkEvent evt) {
    if (evt is! RiftLinkVoiceEvent) return;
    final from = evt.from.toUpperCase();
    final assembly = _voiceAssemblies.putIfAbsent(
      from,
      () => _VoiceRxAssembly(
        from: from,
        total: evt.total,
        startedAt: DateTime.now(),
        lastUpdated: DateTime.now(),
      ),
    );
    if (assembly.total != evt.total) {
      assembly.total = evt.total;
      assembly.parts.clear();
    }
    assembly.parts[evt.chunk] = evt.data;
    assembly.lastUpdated = DateTime.now();
    final stat = _statsFor(from);
    stat.chunksSeen += 1;
    stat.updatedAt = DateTime.now();
    if (assembly.parts.length == evt.total) {
      stat.complete += 1;
      stat.lossPercentSum += _calcLossPercent(
        totalChunks: evt.total,
        contiguousPrefixChunks: evt.total,
      );
      _voiceAssemblies.remove(from);
    }
  }

  void _cleanupStaleVoiceAssemblies() {
    if (!mounted || _voiceAssemblies.isEmpty) return;
    final now = DateTime.now();
    final expired = <String>[];
    _voiceAssemblies.forEach((from, assembly) {
      final inactiveSec = now.difference(assembly.lastUpdated).inSeconds;
      final ageSec = now.difference(assembly.startedAt).inSeconds;
      if (inactiveSec > 20 || ageSec > 45) expired.add(from);
    });
    if (expired.isEmpty) return;
    setState(() {
      for (final from in expired) {
        final assembly = _voiceAssemblies.remove(from);
        if (assembly == null) continue;
        final contiguousPrefixChunks = _countContiguousPrefixChunks(assembly);
        final stats = _statsFor(from);
        if (contiguousPrefixChunks >= 2 && _canDecodePrefix(assembly, contiguousPrefixChunks)) {
          stats.partial += 1;
        } else {
          stats.loss += 1;
        }
        stats.lossPercentSum += _calcLossPercent(
          totalChunks: assembly.total,
          contiguousPrefixChunks: contiguousPrefixChunks,
        );
        stats.updatedAt = now;
      }
    });
  }

  _VoiceRxDebugStats _statsFor(String from) =>
      _voiceStatsByFrom.putIfAbsent(from, () => _VoiceRxDebugStats(from: from));

  static int _countContiguousPrefixChunks(_VoiceRxAssembly assembly) {
    var n = 0;
    while (n < assembly.total && assembly.parts.containsKey(n)) {
      n++;
    }
    return n;
  }

  static bool _canDecodePrefix(_VoiceRxAssembly assembly, int contiguousPrefixChunks) {
    if (contiguousPrefixChunks < 2) return false;
    try {
      final joined = List.generate(contiguousPrefixChunks, (i) => assembly.parts[i] ?? '').join();
      base64Decode(joined);
      return true;
    } catch (_) {
      return false;
    }
  }

  static int _calcLossPercent({
    required int totalChunks,
    required int contiguousPrefixChunks,
  }) {
    if (totalChunks <= 0) return 0;
    final missing = (totalChunks - contiguousPrefixChunks).clamp(0, totalChunks);
    return ((missing * 100) / totalChunks).round();
  }

  String _voiceQualityVerdict(_VoiceRxDebugStats row) {
    if (row.sessions < _voiceAcceptMinSessions) return 'WARMUP';
    final hardLossPercent = ((row.loss * 100) / row.sessions).round();
    if (row.avgLossPercent <= _voiceAcceptMaxAvgLossPercent &&
        hardLossPercent <= _voiceAcceptMaxHardLossPercent) {
      return 'PASS';
    }
    if (row.avgLossPercent <= (_voiceAcceptMaxAvgLossPercent + 15) &&
        hardLossPercent <= (_voiceAcceptMaxHardLossPercent + 15)) {
      return 'WARN';
    }
    return 'FAIL';
  }

  String _exportSnapshot() {
    final traceRows = _traceByMsgId.values.toList()
      ..sort((a, b) => b.updatedAt.compareTo(a.updatedAt));
    final voiceRows = _voiceStatsByFrom.values.toList()
      ..sort((a, b) => b.updatedAt.compareTo(a.updatedAt));
    final buf = StringBuffer();
    final sessionVerdicts = _buildVoiceVerdictSummary(voiceRows);
    buf.writeln('Proof-of-Relay table (msgId)');
    for (final row in traceRows) {
      final relays = row.relays.isEmpty ? '-' : row.relays.join(' -> ');
      final toShort = row.to == null || row.to!.isEmpty ? '-' : _shortLabel(row.to!);
      buf.writeln('msgId=${row.msgId} to=$toShort relays=$relays outcome=${row.outcome}');
    }
    buf.writeln('');
    buf.writeln('Voice RX diagnostics');
    buf.writeln(
      'voiceAcceptance thresholds: avgLoss<=${_voiceAcceptMaxAvgLossPercent}% hardLoss<=${_voiceAcceptMaxHardLossPercent}% minSessions=${_voiceAcceptMinSessions}',
    );
    buf.writeln(
      'voiceAcceptance summary: PASS=${sessionVerdicts['PASS']} WARN=${sessionVerdicts['WARN']} FAIL=${sessionVerdicts['FAIL']} WARMUP=${sessionVerdicts['WARMUP']}',
    );
    for (final row in voiceRows) {
      final hardLossPercent = row.sessions <= 0 ? 0 : ((row.loss * 100) / row.sessions).round();
      buf.writeln(
        'from=${_shortLabel(row.from)} complete=${row.complete} partial=${row.partial} loss=${row.loss} chunks=${row.chunksSeen} avgLoss=${row.avgLossPercent}% hardLoss=$hardLossPercent% verdict=${_voiceQualityVerdict(row)}',
      );
    }
    buf.writeln('');
    buf.writeln('Event log');
    for (final line in _lines) {
      buf.writeln(line);
    }
    return buf.toString();
  }

  Map<String, int> _buildVoiceVerdictSummary(List<_VoiceRxDebugStats> rows) {
    final out = <String, int>{'PASS': 0, 'WARN': 0, 'FAIL': 0, 'WARMUP': 0};
    for (final row in rows) {
      final verdict = _voiceQualityVerdict(row);
      out[verdict] = (out[verdict] ?? 0) + 1;
    }
    return out;
  }

  double _dividerOpacity(BuildContext context) {
    return Theme.of(context).brightness == Brightness.dark ? 0.88 : 0.62;
  }

  Widget _sectionTitle(BuildContext context, String title) {
    final p = context.palette;
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.center,
        children: [
          Container(
            width: 3,
            height: 18,
            decoration: BoxDecoration(
              color: p.primary,
              borderRadius: BorderRadius.circular(2),
            ),
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              title,
              style: TextStyle(
                color: p.onSurface,
                fontWeight: FontWeight.w700,
                fontSize: 14,
                height: 1.25,
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _monoLineCard(BuildContext context, String text) {
    final p = context.palette;
    final o = _dividerOpacity(context);
    return Container(
      margin: const EdgeInsets.only(bottom: _kRowGap),
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
      decoration: BoxDecoration(
        color: p.card,
        borderRadius: BorderRadius.circular(_kCardRadius),
        border: Border.all(color: p.divider.withOpacity(o)),
      ),
      child: Text(
        text,
        style: TextStyle(
          color: p.onSurface,
          fontFamily: 'monospace',
          fontSize: 12,
          height: 1.35,
        ),
      ),
    );
  }

  Widget _voiceMetaCard(
    BuildContext context, {
    required String thresholdsLine,
    required String summaryLine,
  }) {
    final p = context.palette;
    final o = _dividerOpacity(context);
    return Container(
      margin: const EdgeInsets.only(bottom: 8),
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
      decoration: BoxDecoration(
        color: p.surfaceVariant,
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: p.divider.withOpacity(o)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            thresholdsLine,
            style: TextStyle(
              color: p.onSurfaceVariant,
              fontSize: 11,
              fontFamily: 'monospace',
              height: 1.35,
            ),
          ),
          const SizedBox(height: 4),
          Text(
            summaryLine,
            style: TextStyle(
              color: p.onSurfaceVariant,
              fontSize: 11,
              fontFamily: 'monospace',
              height: 1.35,
            ),
          ),
        ],
      ),
    );
  }

  Widget _emptyEventsPlaceholder(BuildContext context) {
    final p = context.palette;
    final o = _dividerOpacity(context);
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.symmetric(vertical: 28, horizontal: 16),
      decoration: BoxDecoration(
        color: p.surfaceVariant,
        borderRadius: BorderRadius.circular(_kCardRadius),
        border: Border.all(color: p.divider.withOpacity(o)),
      ),
      child: Text(
        context.l10n.tr('debug_waiting_events'),
        textAlign: TextAlign.center,
        style: TextStyle(
          color: p.onSurfaceVariant,
          fontSize: 14,
          height: 1.35,
        ),
      ),
    );
  }

  Widget _buildTraceRow(BuildContext context, _RelayTraceRow row) {
    final relays = row.relays.isEmpty ? '-' : row.relays.join(' -> ');
    final toShort = row.to == null || row.to!.isEmpty ? '-' : _shortLabel(row.to!);
    return _monoLineCard(
      context,
      'msgId=${row.msgId} to=$toShort relays=$relays outcome=${row.outcome}',
    );
  }

  Widget _buildVoiceDiagRow(BuildContext context, _VoiceRxDebugStats row) {
    final hardLossPercent = row.sessions <= 0 ? 0 : ((row.loss * 100) / row.sessions).round();
    final verdict = _voiceQualityVerdict(row);
    return _monoLineCard(
      context,
      'from=${_shortLabel(row.from)} complete=${row.complete} partial=${row.partial} loss=${row.loss} chunks=${row.chunksSeen} avgLoss=${row.avgLossPercent}% hardLoss=$hardLossPercent% verdict=$verdict',
    );
  }

  @override
  Widget build(BuildContext context) {
    final traceRows = _traceByMsgId.values.toList()
      ..sort((a, b) => b.updatedAt.compareTo(a.updatedAt));
    final voiceRows = _voiceStatsByFrom.values.toList()
      ..sort((a, b) => b.updatedAt.compareTo(a.updatedAt));
    final voiceVerdicts = _buildVoiceVerdictSummary(voiceRows);
    final p = context.palette;

    return Scaffold(
      backgroundColor: p.surface,
      appBar: AppBar(
        backgroundColor: p.surface,
        foregroundColor: p.onSurface,
        elevation: 0,
        iconTheme: IconThemeData(color: p.onSurface),
        title: Text(context.l10n.tr('debug_log_title')),
        actions: [
          IconButton(
            tooltip: context.l10n.tr('copy'),
            onPressed: () async {
              final text = _exportSnapshot();
              await Clipboard.setData(ClipboardData(text: text));
              if (!mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                SnackBar(content: Text(context.l10n.tr('copied'))),
              );
            },
            icon: const Icon(Icons.copy_all_outlined),
          ),
          IconButton(
            tooltip: 'Clear',
            onPressed: () => setState(() {
              _lines.clear();
              _traceByMsgId.clear();
              _voiceAssemblies.clear();
              _voiceStatsByFrom.clear();
            }),
            icon: const Icon(Icons.delete_outline),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(_kScreenPadH, _kScreenPadV, _kScreenPadH, _kScreenPadBottom),
        children: [
          if (traceRows.isNotEmpty) ...[
            _sectionTitle(context, 'Proof-of-Relay table (msgId)'),
            ...traceRows.take(60).map((row) => _buildTraceRow(context, row)),
            const SizedBox(height: _kSectionGap),
          ],
          if (voiceRows.isNotEmpty) ...[
            _sectionTitle(context, 'Voice RX diagnostics'),
            _voiceMetaCard(
              context,
              thresholdsLine:
                  'avg<=${_voiceAcceptMaxAvgLossPercent}% hard<=${_voiceAcceptMaxHardLossPercent}% minSessions=$_voiceAcceptMinSessions',
              summaryLine:
                  'summary PASS=${voiceVerdicts['PASS']} WARN=${voiceVerdicts['WARN']} FAIL=${voiceVerdicts['FAIL']} WARMUP=${voiceVerdicts['WARMUP']}',
            ),
            ...voiceRows.take(40).map((row) => _buildVoiceDiagRow(context, row)),
            const SizedBox(height: _kSectionGap),
          ],
          if (_lines.isEmpty)
            Padding(
              padding: const EdgeInsets.only(top: 20),
              child: _emptyEventsPlaceholder(context),
            )
          else
            ...List<Widget>.generate(_lines.length, (i) {
              final line = _lines[_lines.length - 1 - i];
              return _monoLineCard(context, line);
            }),
        ],
      ),
    );
  }
}

class _RelayTraceRow {
  final int msgId;
  String? to;
  final List<String> relays;
  String outcome;
  DateTime updatedAt;

  _RelayTraceRow({
    required this.msgId,
    this.to,
    List<String>? relays,
    this.outcome = 'pending',
    DateTime? updatedAt,
  })  : relays = relays ?? <String>[],
        updatedAt = updatedAt ?? DateTime.now();
}

class _VoiceRxAssembly {
  final String from;
  int total;
  final DateTime startedAt;
  DateTime lastUpdated;
  final Map<int, String> parts;

  _VoiceRxAssembly({
    required this.from,
    required this.total,
    required this.startedAt,
    required this.lastUpdated,
    Map<int, String>? parts,
  }) : parts = parts ?? <int, String>{};
}

class _VoiceRxDebugStats {
  final String from;
  int complete;
  int partial;
  int loss;
  int chunksSeen;
  int lossPercentSum;
  DateTime updatedAt;

  _VoiceRxDebugStats({
    required this.from,
    this.complete = 0,
    this.partial = 0,
    this.loss = 0,
    this.chunksSeen = 0,
    this.lossPercentSum = 0,
    DateTime? updatedAt,
  }) : updatedAt = updatedAt ?? DateTime.now();

  int get sessions => complete + partial + loss;
  int get avgLossPercent => sessions <= 0 ? 0 : (lossPercentSum / sessions).round();
}
