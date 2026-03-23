import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../ble/riftlink_ble.dart';
import '../contacts/contacts_service.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../theme/design_tokens.dart';
import '../widgets/app_primitives.dart';
import '../widgets/mesh_background.dart';

Color _rssiColor(AppPalette palette, int rssi) {
  if (rssi >= -85) return palette.success;
  if (rssi >= -100) return const Color(0xFFFFB300);
  return palette.error;
}

class MeshScreen extends StatefulWidget {
  final RiftLinkBle ble;
  final String nodeId;
  final List<String> neighbors;
  final List<int> neighborsRssi;
  final List<Map<String, dynamic>> routes;

  const MeshScreen({
    super.key,
    required this.ble,
    required this.nodeId,
    required this.neighbors,
    required this.neighborsRssi,
    required this.routes,
  });

  @override
  State<MeshScreen> createState() => _MeshScreenState();
}

class _MeshScreenState extends State<MeshScreen> {
  List<Map<String, dynamic>> _routes = [];
  StreamSubscription<RiftLinkEvent>? _sub;
  Map<String, String> _nickById = const {};
  bool _signalTestRunning = false;
  bool _signalTestAttempted = false;
  final Map<String, int> _signalRssiByNode = <String, int>{};
  Timer? _signalTimer;
  String? _traceTarget;
  String _nodeId = '';
  List<String> _neighbors = const [];
  List<int> _neighborsRssi = const [];
  final Map<String, int> _liveNeighborRssiByNode = <String, int>{};
  final Map<String, int> _liveNeighborRssiAtMs = <String, int>{};
  static const int _liveRssiTtlMs = 30000;

  String _normalizeNodeId(String raw) => raw.trim().toUpperCase();

  void _applyLiveRssiOverrides() {
    final now = DateTime.now().millisecondsSinceEpoch;
    for (var i = 0; i < _neighbors.length; i++) {
      final id = _neighbors[i];
      final ts = _liveNeighborRssiAtMs[id];
      final live = _liveNeighborRssiByNode[id];
      if (ts == null || live == null) continue;
      if (now - ts > _liveRssiTtlMs) continue;
      if (i < _neighborsRssi.length) {
        _neighborsRssi[i] = live;
      }
    }
  }

  /// Сопоставление id из [RiftLinkPongEvent] со списком соседей (полный 16 hex vs префикс).
  String? _matchNeighborKeyForPong(String rawFrom) {
    final n = _normalizeNodeId(rawFrom);
    if (n.isEmpty) return null;
    for (final nb in _neighbors) {
      final b = _normalizeNodeId(nb);
      if (b == n) return b;
      if (n.length >= 8 && b.length >= 8 && (b.startsWith(n) || n.startsWith(b))) return b;
    }
    if (RegExp(r'^[0-9A-F]{16}$').hasMatch(n)) return n;
    return n.length >= 8 ? n : null;
  }

  void _recordLiveRssi(String nodeId, int rssi) {
    final id = _normalizeNodeId(nodeId);
    if (!RegExp(r'^[0-9A-F]{16}$').hasMatch(id)) return;
    if (rssi == 0) return;
    final now = DateTime.now().millisecondsSinceEpoch;
    _liveNeighborRssiByNode[id] = rssi;
    _liveNeighborRssiAtMs[id] = now;
    final idx = _neighbors.indexWhere((n) => n.toUpperCase() == id);
    if (idx >= 0 && idx < _neighborsRssi.length) {
      _neighborsRssi[idx] = rssi;
    }
  }

  void _applySnapshot({
    String? nodeId,
    List<String>? neighbors,
    List<int>? neighborsRssi,
  }) {
    _nodeId = _normalizeNodeId(nodeId ?? _nodeId);
    if (neighbors != null) {
      final nextNeighbors = <String>[];
      final nextRssi = <int>[];
      final seen = <String>{};
      for (var i = 0; i < neighbors.length; i++) {
        final id = _normalizeNodeId(neighbors[i]);
        if (!RegExp(r'^[0-9A-F]{16}$').hasMatch(id)) continue;
        if (!seen.add(id)) continue;
        nextNeighbors.add(id);
        final rssi = (neighborsRssi != null && i < neighborsRssi.length) ? neighborsRssi[i] : 0;
        nextRssi.add(rssi);
      }
      _neighbors = nextNeighbors;
      _neighborsRssi = nextRssi;
    } else if (neighborsRssi != null) {
      final next = List<int>.from(_neighborsRssi);
      final n = neighborsRssi.length < _neighbors.length ? neighborsRssi.length : _neighbors.length;
      for (var i = 0; i < n; i++) {
        if (i < next.length) {
          next[i] = neighborsRssi[i];
        } else {
          next.add(neighborsRssi[i]);
        }
      }
      _neighborsRssi = next;
    }
    _applyLiveRssiOverrides();
  }

  void _snack(String text) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(text), duration: const Duration(seconds: 2)),
    );
  }

  void _normalizeTraceTarget() {
    if (_traceTarget == null) return;
    final candidates = _traceCandidates().map((e) => e.toUpperCase()).toSet();
    if (!candidates.contains(_traceTarget!.toUpperCase())) {
      _traceTarget = null;
    }
  }

  @override
  void initState() {
    super.initState();
    _applySnapshot(
      nodeId: widget.nodeId,
      neighbors: widget.neighbors,
      neighborsRssi: widget.neighborsRssi,
    );
    final li = widget.ble.lastInfo;
    if (li != null) {
      _applySnapshot(
        nodeId: li.id,
        neighbors: li.neighbors,
        neighborsRssi: li.neighborsRssi,
      );
    }
    _routes = List.from(widget.routes);
    _loadNicknames();
    debugPrint('[BLE_CHAIN] stage=app_listener action=mesh_subscribe');
    WidgetsBinding.instance.addPostFrameCallback((_) {
      unawaited(() async {
        try {
          await widget.ble.getInfo();
          await Future<void>.delayed(const Duration(milliseconds: 90));
          if (mounted) await widget.ble.getRoutes();
        } catch (_) {}
      }());
    });
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      debugPrint('[BLE_CHAIN] stage=app_listener action=mesh_event evt=${evt.runtimeType}');
      if (evt is RiftLinkRoutesEvent) {
        setState(() {
          _routes = evt.routes;
          _normalizeTraceTarget();
        });
      } else if (evt is RiftLinkInfoEvent) {
        setState(() {
          _applySnapshot(
            nodeId: evt.id,
            neighbors: evt.neighbors,
            neighborsRssi: evt.neighborsRssi,
          );
          _routes = evt.routes;
          _normalizeTraceTarget();
        });
      } else if (evt is RiftLinkNeighborsEvent) {
        setState(() {
          _applySnapshot(
            neighbors: evt.neighbors,
            neighborsRssi: evt.rssi,
          );
          _normalizeTraceTarget();
        });
      } else if (evt is RiftLinkPongEvent) {
        final from = evt.from;
        if (from.isEmpty) return;
        setState(() {
          final key = _matchNeighborKeyForPong(from);
          if (key == null) return;
          _signalRssiByNode[key] = evt.rssi ?? 0;
          final rssi = evt.rssi;
          if (rssi != null && RegExp(r'^[0-9A-F]{16}$').hasMatch(key)) {
            _recordLiveRssi(key, rssi);
          }
        });
      }
    });
  }

  @override
  void didUpdateWidget(covariant MeshScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.nodeId != widget.nodeId ||
        oldWidget.neighbors != widget.neighbors ||
        oldWidget.neighborsRssi != widget.neighborsRssi) {
      setState(() {
        _applySnapshot(
          nodeId: widget.nodeId,
          neighbors: widget.neighbors,
          neighborsRssi: widget.neighborsRssi,
        );
        _normalizeTraceTarget();
      });
    }
  }

  @override
  void dispose() {
    _sub?.cancel();
    _signalTimer?.cancel();
    super.dispose();
  }

  Future<void> _loadNicknames() async {
    final contacts = await ContactsService.load();
    if (!mounted) return;
    setState(() {
      _nickById = ContactsService.buildNicknameMap(contacts);
    });
  }

  String _labelForNode(String id) {
    return ContactsService.displayNodeLabel(id, _nickById);
  }

  String _shortLabel(String id) {
    final label = _labelForNode(id);
    if (label == id.toUpperCase() && label.length > 12) {
      return '${label.substring(0, 4)}…${label.substring(label.length - 4)}';
    }
    return label;
  }

  Future<void> _runSignalTest() async {
    if (_signalTestRunning) return;
    // До await: signalTest() только подтверждает TX; ответы — отдельные evt:pong.
    // Если сначала await, а потом clear(), быстрые pong успевают попасть в [_signalRssiByNode]
    // и затем затираются — в UI «нет данных».
    setState(() {
      _signalTestRunning = true;
      _signalTestAttempted = true;
      _signalRssiByNode.clear();
    });
    _signalTimer?.cancel();
    _signalTimer = Timer(const Duration(seconds: 10), () {
      if (!mounted) return;
      setState(() => _signalTestRunning = false);
    });
    final ok = await widget.ble.signalTest();
    if (!ok || !mounted) {
      _signalTimer?.cancel();
      setState(() {
        _signalTestRunning = false;
      });
      _snack(context.l10n.tr('mesh_signal_test_failed'));
      return;
    }
  }

  Future<void> _runTraceroute() async {
    final to = _traceTarget;
    if (to == null || to.isEmpty) return;
    final ok = await widget.ble.traceroute(to);
    if (ok) {
      _snack('${context.l10n.tr('mesh_traceroute')}: ${_shortLabel(to)}');
      await widget.ble.getRoutes();
      Future<void>.delayed(const Duration(milliseconds: 900), () {
        if (!mounted) return;
        widget.ble.getRoutes();
      });
    } else {
      _snack(context.l10n.tr('mesh_traceroute_failed'));
    }
  }

  _GraphData _computeGraph() {
    final nodes = <String, _NP>{};
    final edges = <_Edge>[];

    const center = Offset(0, 0);
    nodes[_nodeId] = _NP(center, isSelf: true);

    final n = _neighbors.length;
    const r1 = 120.0;
    for (var i = 0; i < n; i++) {
      final angle = 2 * math.pi * i / (n > 0 ? n : 1) - math.pi / 2;
      nodes[_neighbors[i]] = _NP(
        Offset(r1 * math.cos(angle), r1 * math.sin(angle)),
        rssi: i < _neighborsRssi.length ? _neighborsRssi[i] : 0,
      );
      edges.add(_Edge(_nodeId, _neighbors[i], isDirect: true));
    }

    final routeDests = _routes.map((r) => r['dest'] as String? ?? '').where((s) => s.isNotEmpty).toSet();
    const r2 = 200.0;
    var j = 0;
    for (final dest in routeDests) {
      if (nodes.containsKey(dest)) continue;
      final route = _routes.firstWhere((r) => (r['dest'] as String?) == dest, orElse: () => {});
      final nextHop = route['nextHop'] as String? ?? '';
      final hops = (route['hops'] as num?)?.toInt() ?? 0;
      final rssi = (route['rssi'] as num?)?.toInt() ?? 0;
      final angle = 2 * math.pi * j / (routeDests.isNotEmpty ? routeDests.length : 1) - math.pi / 2 + 0.5;
      nodes[dest] = _NP(Offset(r2 * math.cos(angle), r2 * math.sin(angle)), hops: hops, rssi: rssi);
      edges.add(_Edge(nextHop.isNotEmpty ? nextHop : _nodeId, dest, isDirect: false, hops: hops));
      j++;
    }

    return _GraphData(nodes, edges);
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final graph = _computeGraph();
    final hasListData = _neighbors.isNotEmpty ||
        _routes.any((r) => ((r['dest'] as String?) ?? '').isNotEmpty);

    return DefaultTabController(
      length: 2,
      child: Scaffold(
        backgroundColor: context.palette.surface,
        appBar: riftAppBar(
          context,
          title: l.tr('mesh_topology'),
          showBack: true,
          bottom: TabBar(
            labelColor: context.palette.primary,
            unselectedLabelColor: context.palette.onSurfaceVariant,
            indicatorColor: context.palette.primary,
            indicatorWeight: 2.5,
            labelStyle: AppTypography.labelBase().copyWith(fontWeight: FontWeight.w600),
            tabs: [
              Tab(icon: const Icon(Icons.account_tree, size: 20), text: l.tr('mesh_tab_graph')),
              Tab(icon: const Icon(Icons.view_list_rounded, size: 20), text: l.tr('mesh_tab_list')),
            ],
          ),
        ),
        body: Column(
          children: [
            AppSectionCard(
              margin: EdgeInsets.fromLTRB(AppSpacing.md, AppSpacing.sm + 2, AppSpacing.md, 0),
              padding: const EdgeInsets.all(AppSpacing.md),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  _SectionTitle(title: l.tr('mesh_tools')),
                  SizedBox(height: AppSpacing.sm + 2),
                  Row(
                    children: [
                      Expanded(
                        child: FilledButton.tonalIcon(
                          onPressed: _signalTestRunning ? null : _runSignalTest,
                          icon: const Icon(Icons.network_ping_rounded, size: 18),
                          label: Text(l.tr('mesh_signal_test')),
                        ),
                      ),
                      SizedBox(width: AppSpacing.sm),
                      IconButton(
                        tooltip: l.tr('refresh'),
                        onPressed: () => widget.ble.getRoutes(),
                        icon: const Icon(Icons.refresh),
                      ),
                    ],
                  ),
                  if (_signalTestRunning || _signalTestAttempted || _signalRssiByNode.isNotEmpty) ...[
                    SizedBox(height: AppSpacing.sm + 2),
                    Text(
                      _signalTestRunning
                          ? l.tr('mesh_signal_waiting')
                          : l.tr('mesh_trace_result'),
                      style: AppTypography.chipBase().copyWith(
                        color: context.palette.onSurfaceVariant,
                        fontWeight: FontWeight.w400,
                      ),
                    ),
                    SizedBox(height: AppSpacing.sm - 2),
                    if (_signalRssiByNode.isEmpty)
                      Text(
                        l.tr('mesh_signal_no_data'),
                        style: AppTypography.chipBase().copyWith(
                          color: context.palette.onSurfaceVariant,
                          fontWeight: FontWeight.w400,
                        ),
                      )
                    else
                      Builder(
                        builder: (_) {
                          final sorted = _signalRssiByNode.entries.toList()
                            ..sort((a, b) => b.value.compareTo(a.value));
                          return Wrap(
                            spacing: AppSpacing.sm - 2,
                            runSpacing: AppSpacing.sm - 2,
                            children: sorted.map((e) {
                              final c = _rssiColor(context.palette, e.value);
                              return Container(
                                padding: const EdgeInsets.symmetric(
                                  horizontal: AppSpacing.sm,
                                  vertical: AppSpacing.xs,
                                ),
                                decoration: BoxDecoration(
                                  color: c.withOpacity(0.15),
                                  borderRadius: BorderRadius.circular(AppRadius.sm),
                                  border: Border.all(color: c.withOpacity(0.5)),
                                ),
                                child: Text(
                                  '${_shortLabel(e.key)} ${e.value} dBm',
                                  style: AppTypography.chipBase().copyWith(
                                    color: context.palette.onSurface,
                                    fontWeight: FontWeight.w500,
                                  ),
                                ),
                              );
                            }).toList(),
                          );
                        },
                      ),
                  ],
                  SizedBox(height: AppSpacing.md),
                  Row(
                    children: [
                      Expanded(
                        child: DropdownButtonFormField<String>(
                          value: _traceTarget,
                          decoration: InputDecoration(
                            labelText: l.tr('mesh_select_node'),
                            isDense: true,
                          ),
                          items: _traceCandidates()
                              .map(
                                (id) => DropdownMenuItem<String>(
                                  value: id,
                                  child: Text(_shortLabel(id)),
                                ),
                              )
                              .toList(),
                          onChanged: (v) => setState(() => _traceTarget = v),
                        ),
                      ),
                      SizedBox(width: AppSpacing.sm),
                      FilledButton(
                        onPressed: _traceTarget == null ? null : _runTraceroute,
                        child: Text(l.tr('mesh_traceroute')),
                      ),
                    ],
                  ),
                  if (_traceTarget != null) ...[
                    SizedBox(height: AppSpacing.sm),
                    Text(
                      _traceSummary(context, _traceTarget!),
                      style: AppTypography.chipBase().copyWith(
                        color: context.palette.onSurfaceVariant,
                        fontWeight: FontWeight.w400,
                      ),
                    ),
                  ],
                ],
              ),
            ),
            Expanded(
              child: TabBarView(
                children: [
                  _GraphTab(
                    graph: graph,
                    nodeId: _nodeId,
                    nodeLabel: _labelForNode,
                  ),
                  _ListTab(
                    neighbors: _neighbors,
                    neighborsRssi: _neighborsRssi,
                    routes: _routes,
                    hasData: hasListData,
                    nodeLabel: _labelForNode,
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  List<String> _traceCandidates() {
    final all = <String>{..._neighbors};
    for (final r in _routes) {
      final d = (r['dest'] as String?) ?? '';
      if (d.isNotEmpty) all.add(d);
    }
    all.remove(_nodeId);
    return all.toList()..sort();
  }

  String _traceSummary(BuildContext context, String target) {
    final l = context.l10n;
    Map<String, dynamic>? route;
    for (final r in _routes) {
      if ((r['dest'] as String?) == target) {
        route = r;
        break;
      }
    }
    if (route == null) {
      final idx = _neighbors.indexWhere((n) => n.toUpperCase() == target.toUpperCase());
      if (idx >= 0) {
        final hopText = '${l.tr('mesh_trace_hops')}: ${_shortLabel(_nodeId)} -> ${_shortLabel(target)}';
        final rssi = idx < _neighborsRssi.length ? _neighborsRssi[idx] : 0;
        final rssiText = rssi != 0 ? ' · RSSI $rssi dBm' : '';
        return '$hopText$rssiText · ${l.tr('mesh_route_hops')}: 1';
      }
      return l.tr('mesh_trace_no_route');
    }
    final hops = (route['hops'] as num?)?.toInt() ?? 0;
    final pathRaw = route['path'];
    final hopRssiRaw = route['hopRssi'];
    final path = <String>[];
    if (pathRaw is List && pathRaw.isNotEmpty) {
      path.addAll(pathRaw.map((e) => e.toString()));
    } else {
      final next = (route['nextHop'] as String?) ?? '';
      path.add(_nodeId);
      if (next.isNotEmpty && next.toUpperCase() != _nodeId.toUpperCase()) path.add(next);
      if (hops > 2) path.add('...');
      path.add(target);
    }
    final hopRssi = <int>[];
    if (hopRssiRaw is List) {
      hopRssi.addAll(hopRssiRaw.map((e) => (e as num?)?.toInt() ?? 0));
    } else {
      final firstHop = (route['nextHop'] as String?) ?? '';
      if (firstHop.isNotEmpty) {
        final idx = _neighbors.indexWhere((n) => n.toUpperCase() == firstHop.toUpperCase());
        hopRssi.add(idx >= 0 && idx < _neighborsRssi.length ? _neighborsRssi[idx] : 0);
      }
      hopRssi.add((route['rssi'] as num?)?.toInt() ?? 0);
    }
    final rendered = <String>[];
    for (var i = 0; i < path.length; i++) {
      final node = path[i];
      if (node == '...') {
        rendered.add('...');
        continue;
      }
      final rssi = i > 0 && (i - 1) < hopRssi.length ? hopRssi[i - 1] : 0;
      final label = _shortLabel(node);
      rendered.add(rssi != 0 ? '$label ($rssi dBm)' : label);
    }
    final next = (route['nextHop'] as String?) ?? '';
    final rssi = (route['rssi'] as num?)?.toInt() ?? 0;
    final modemPreset = (route['modemPreset'] as num?)?.toInt();
    final sf = (route['sf'] as num?)?.toInt();
    final bw = (route['bw'] as num?)?.toDouble();
    final cr = (route['cr'] as num?)?.toInt();
    final trust = (route['trustScore'] as num?)?.toInt();
    String? modem;
    if (modemPreset != null) {
      modem = _modemPresetLabel(l, modemPreset);
    } else if (sf != null || bw != null || cr != null) {
      modem = 'SF${sf ?? '?'} / BW${bw?.toStringAsFixed(1) ?? '?'} / CR${cr ?? '?'}';
    }
    final base = '${l.tr('mesh_trace_hops')}: ${rendered.join(' -> ')}';
    final details = <String>[
      if (next.isNotEmpty) '${l.tr('mesh_col_next')}: ${_shortLabel(next)}',
      '$hops ${l.tr('mesh_route_hops')}',
      if (rssi != 0) 'RSSI $rssi dBm',
      if (modem != null) '${l.tr('mesh_modem')}: $modem',
      if (trust != null) 'trust $trust',
    ];
    return '$base\n${details.join(' · ')}';
  }

  String _modemPresetLabel(AppLocalizations l, int preset) {
    return switch (preset) {
      0 => l.tr('modem_preset_speed'),
      1 => l.tr('modem_preset_normal'),
      2 => l.tr('modem_preset_range'),
      3 => l.tr('modem_preset_maxrange'),
      _ => l.tr('modem_preset_custom'),
    };
  }

}

class _GraphData {
  final Map<String, _NP> nodes;
  final List<_Edge> edges;
  _GraphData(this.nodes, this.edges);
}

class _GraphTab extends StatelessWidget {
  final _GraphData graph;
  final String nodeId;
  final String Function(String) nodeLabel;

  const _GraphTab({
    required this.graph,
    required this.nodeId,
    required this.nodeLabel,
  });

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    return MeshBackgroundWrapper(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Padding(
            padding: EdgeInsets.fromLTRB(AppSpacing.lg, AppSpacing.md, AppSpacing.lg, AppSpacing.sm),
            child: AppSectionCard(
              padding: EdgeInsets.symmetric(
                horizontal: AppSpacing.lg - 2,
                vertical: AppSpacing.sm + 2,
              ),
              child: Wrap(
                spacing: AppSpacing.lg,
                runSpacing: AppSpacing.sm,
                crossAxisAlignment: WrapCrossAlignment.center,
                children: [
                  _LegendItem(
                    color: context.palette.onSurfaceVariant,
                    label: l.tr('mesh_legend_direct'),
                  ),
                  _LegendItem(
                    color: context.palette.primary,
                    label: l.tr('mesh_legend_route'),
                  ),
                  _LegendItem(
                    color: _rssiColor(context.palette, -80),
                    label: l.tr('mesh_rssi_strong'),
                  ),
                  _LegendItem(
                    color: _rssiColor(context.palette, -95),
                    label: l.tr('mesh_rssi_medium'),
                  ),
                  _LegendItem(
                    color: _rssiColor(context.palette, -110),
                    label: l.tr('mesh_rssi_weak'),
                  ),
                  Text(
                    '${l.tr('settings_node_id')}: ${nodeLabel(nodeId)}',
                    style: AppTypography.chipBase().copyWith(
                      color: context.palette.onSurfaceVariant.withOpacity(0.95),
                      fontWeight: FontWeight.w400,
                    ),
                  ),
                ],
              ),
            ),
          ),
          Expanded(
            child: LayoutBuilder(
              builder: (_, constraints) {
                final size = Size(constraints.maxWidth, constraints.maxHeight);
                final c = Offset(size.width / 2, size.height / 2);
                return CustomPaint(
                  size: size,
                  painter: _MeshPainter(
                    nodes: graph.nodes,
                    edges: graph.edges,
                    center: c,
                    palette: context.palette,
                    nodeLabel: nodeLabel,
                  ),
                );
              },
            ),
          ),
        ],
      ),
    );
  }

}

class _LegendItem extends StatelessWidget {
  final Color color;
  final String label;

  const _LegendItem({required this.color, required this.label});

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Container(
          width: 22,
          height: 3,
          decoration: BoxDecoration(
            color: color,
            borderRadius: BorderRadius.circular(AppRadius.sm / 4),
          ),
        ),
        SizedBox(width: AppSpacing.sm),
        Text(
          label,
          style: AppTypography.chipBase().copyWith(
            color: context.palette.onSurface,
            fontWeight: FontWeight.w400,
          ),
        ),
      ],
    );
  }
}

class _ListTab extends StatelessWidget {
  final List<String> neighbors;
  final List<int> neighborsRssi;
  final List<Map<String, dynamic>> routes;
  final bool hasData;
  final String Function(String) nodeLabel;

  const _ListTab({
    required this.neighbors,
    required this.neighborsRssi,
    required this.routes,
    required this.hasData,
    required this.nodeLabel,
  });

  String _modemPresetLabel(AppLocalizations l, int preset) {
    return switch (preset) {
      0 => l.tr('modem_preset_speed'),
      1 => l.tr('modem_preset_normal'),
      2 => l.tr('modem_preset_range'),
      3 => l.tr('modem_preset_maxrange'),
      _ => l.tr('modem_preset_custom'),
    };
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final validRoutes = routes.where((r) => ((r['dest'] as String?) ?? '').isNotEmpty).toList();

    return MeshBackgroundWrapper(
      child: Material(
        color: Colors.transparent,
        child: !hasData
            ? Center(
                child: Padding(
                  padding: const EdgeInsets.all(AppSpacing.xxl + AppSpacing.sm),
                  child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      Icon(
                        Icons.device_hub_outlined,
                        size: 56,
                        color: context.palette.onSurfaceVariant.withOpacity(0.45),
                      ),
                      SizedBox(height: AppSpacing.lg),
                      Text(
                        l.tr('mesh_empty'),
                        textAlign: TextAlign.center,
                        style: AppTypography.bodyBase().copyWith(
                          fontSize: 14,
                          height: 1.4,
                          color: context.palette.onSurfaceVariant.withOpacity(0.95),
                        ),
                      ),
                    ],
                  ),
                ),
              )
            : ListView(
                padding: EdgeInsets.fromLTRB(AppSpacing.lg, AppSpacing.md, AppSpacing.lg, AppSpacing.xxl),
                children: [
                  if (neighbors.isNotEmpty) ...[
                    _SectionTitle(title: l.tr('mesh_list_neighbors')),
                    SizedBox(height: AppSpacing.sm),
                    AppSectionCard(
                      padding: EdgeInsets.zero,
                      child: Column(
                        children: [
                          for (var i = 0; i < neighbors.length; i++) ...[
                            if (i > 0) Divider(height: 1, thickness: 1, color: context.palette.divider),
                            ListTile(
                              leading: CircleAvatar(
                                backgroundColor: context.palette.primary.withOpacity(0.2),
                                child: Icon(Icons.router_outlined, color: context.palette.primary, size: 22),
                              ),
                              title: Text(
                                nodeLabel(neighbors[i]),
                                style: TextStyle(
                                  fontSize: 14,
                                  fontWeight: FontWeight.w600,
                                  color: context.palette.onSurface,
                                ),
                              ),
                              subtitle: Text(
                                '${l.tr('neighbors')} · RSSI ${i < neighborsRssi.length ? neighborsRssi[i] : '—'}',
                                style: AppTypography.chipBase().copyWith(
                                  color: context.palette.onSurfaceVariant.withOpacity(0.95),
                                  fontWeight: FontWeight.w400,
                                ),
                              ),
                            ),
                          ],
                        ],
                      ),
                    ),
                    SizedBox(height: AppSpacing.xl),
                  ],
                  if (validRoutes.isNotEmpty) ...[
                    _SectionTitle(title: l.tr('mesh_list_routes')),
                    SizedBox(height: AppSpacing.sm),
                    AppSectionCard(
                      padding: EdgeInsets.zero,
                      child: Column(
                        children: [
                          for (var i = 0; i < validRoutes.length; i++) ...[
                            if (i > 0) Divider(height: 1, thickness: 1, color: context.palette.divider),
                            Builder(
                              builder: (_) {
                                final r = validRoutes[i];
                                final dest = (r['dest'] as String?) ?? '';
                                final next = (r['nextHop'] as String?) ?? '';
                                final hops = (r['hops'] as num?)?.toInt() ?? 0;
                                final rssi = (r['rssi'] as num?)?.toInt() ?? 0;
                                final modemPreset = (r['modemPreset'] as num?)?.toInt();
                                final sf = (r['sf'] as num?)?.toInt();
                                final bw = (r['bw'] as num?)?.toDouble();
                                final cr = (r['cr'] as num?)?.toInt();
                                final trust = (r['trustScore'] as num?)?.toInt();
                                String? modem;
                                if (modemPreset != null) {
                                  modem = _modemPresetLabel(l, modemPreset);
                                } else if (sf != null || bw != null || cr != null) {
                                  modem = 'SF${sf ?? '?'} / BW${bw?.toStringAsFixed(1) ?? '?'} / CR${cr ?? '?'}';
                                }
                                return ListTile(
                                  leading: CircleAvatar(
                                    backgroundColor: context.palette.success.withOpacity(0.18),
                                    child: Icon(Icons.alt_route, color: context.palette.success, size: 22),
                                  ),
                                  title: Text(
                                    nodeLabel(dest),
                                    style: TextStyle(
                                      fontSize: 14,
                                      fontWeight: FontWeight.w600,
                                      color: context.palette.onSurface,
                                    ),
                                  ),
                                  subtitle: Text(
                                    '${l.tr('mesh_col_next')}: ${next.isNotEmpty ? nodeLabel(next) : '—'} · $hops ${l.tr('mesh_route_hops')}${rssi != 0 ? ' · RSSI $rssi' : ''}${modem != null ? ' · ${l.tr('mesh_modem')}: $modem' : ''}${trust != null ? ' · trust $trust' : ''}',
                                    style: AppTypography.chipBase().copyWith(
                                      color: context.palette.onSurfaceVariant.withOpacity(0.95),
                                      fontWeight: FontWeight.w400,
                                    ),
                                  ),
                                );
                              },
                            ),
                          ],
                        ],
                      ),
                    ),
                  ],
                ],
              ),
      ),
    );
  }
}

class _SectionTitle extends StatelessWidget {
  final String title;
  const _SectionTitle({required this.title});

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Container(
          width: 3,
          height: AppSpacing.lg,
          decoration: BoxDecoration(
            color: context.palette.primary,
            borderRadius: BorderRadius.circular(AppRadius.sm / 4),
          ),
        ),
        SizedBox(width: AppSpacing.sm + 2),
        Text(
          title,
          style: AppTypography.labelBase().copyWith(
            fontWeight: FontWeight.w600,
            color: context.palette.onSurface,
            letterSpacing: 0.2,
          ),
        ),
      ],
    );
  }
}

class _NP {
  final Offset pos;
  final bool isSelf;
  final int rssi, hops;

  _NP(this.pos, {this.isSelf = false, this.rssi = 0, this.hops = 0});
}

class _Edge {
  final String from, to;
  final bool isDirect;
  final int hops;

  _Edge(this.from, this.to, {this.isDirect = true, this.hops = 0});
}

class _MeshPainter extends CustomPainter {
  final Map<String, _NP> nodes;
  final List<_Edge> edges;
  final Offset center;
  final AppPalette palette;
  final String Function(String) nodeLabel;

  _MeshPainter({
    required this.nodes,
    required this.edges,
    required this.center,
    required this.palette,
    required this.nodeLabel,
  });

  Offset _pos(String id) {
    final n = nodes[id];
    if (n == null) return center;
    return Offset(center.dx + n.pos.dx, center.dy + n.pos.dy);
  }

  @override
  void paint(Canvas canvas, Size size) {
    final edgePaint = Paint()
      ..color = palette.onSurfaceVariant.withOpacity(0.62)
      ..strokeWidth = 2
      ..style = PaintingStyle.stroke;

    final routePaint = Paint()
      ..color = palette.primary.withOpacity(0.85)
      ..strokeWidth = 2.5
      ..style = PaintingStyle.stroke;

    for (final e in edges) {
      final from = _pos(e.from);
      final to = _pos(e.to);
      final targetRssi = nodes[e.to]?.rssi ?? 0;
      final linePaint = Paint()
        ..color = (targetRssi != 0 ? _rssiColor(palette, targetRssi) : (e.isDirect ? edgePaint.color : routePaint.color)).withOpacity(0.85)
        ..strokeWidth = e.isDirect ? 2 : 2.5
        ..style = PaintingStyle.stroke;
      canvas.drawLine(from, to, linePaint);
      if (!e.isDirect && e.hops > 0) {
        _drawText(
          canvas,
          '${e.hops}h',
          Offset((from.dx + to.dx) / 2, (from.dy + to.dy) / 2),
          10,
          palette.primary,
        );
      }
    }

    for (final e in nodes.entries) {
      final id = e.key;
      final n = e.value;
      final pos = _pos(id);
      final r = n.isSelf ? 22.0 : 17.0;
      final qualityColor = n.rssi != 0 ? _rssiColor(palette, n.rssi) : palette.primary;
      final fill = n.isSelf
          ? palette.success.withOpacity(0.35)
          : qualityColor.withOpacity(n.hops > 0 ? 0.22 : 0.14);
      final stroke = n.isSelf ? palette.success : qualityColor.withOpacity(0.9);

      canvas.drawCircle(pos, r, Paint()..color = fill);
      canvas.drawCircle(
        pos,
        r,
        Paint()
          ..color = stroke
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2,
      );

      final label = nodeLabel(id);
      final compact = label.length > 14 ? '${label.substring(0, 13)}…' : label;
      _drawText(canvas, compact, Offset(pos.dx, pos.dy + r + 10), 11, palette.onSurface);
      if (n.rssi != 0) {
        _drawText(canvas, '${n.rssi} dBm', Offset(pos.dx, pos.dy + r + 24), 10, palette.onSurfaceVariant);
      }
      if (n.hops > 0) {
        _drawText(
          canvas,
          '${n.hops}h',
          Offset(pos.dx, pos.dy + r + (n.rssi != 0 ? 36 : 24)),
          10,
          palette.primary,
        );
      }
    }
  }

  void _drawText(Canvas canvas, String text, Offset pos, double fontSize, Color color) {
    final tp = TextPainter(
      text: TextSpan(
        text: text,
        style: TextStyle(fontSize: fontSize, color: color, fontWeight: FontWeight.w500),
      ),
      textDirection: TextDirection.ltr,
    )..layout();
    tp.paint(canvas, Offset(pos.dx - tp.width / 2, pos.dy));
  }

  @override
  bool shouldRepaint(covariant _MeshPainter old) =>
      old.nodes != nodes || old.edges != edges || old.palette != palette;
}
