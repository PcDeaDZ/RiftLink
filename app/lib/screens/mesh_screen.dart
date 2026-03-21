import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../ble/riftlink_ble.dart';
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
  bool _signalTestRunning = false;
  final Map<String, int> _signalRssiByNode = <String, int>{};
  Timer? _signalTimer;
  String? _traceTarget;

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
    _routes = List.from(widget.routes);
    debugPrint('[BLE_CHAIN] stage=app_listener action=mesh_subscribe');
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
          _routes = evt.routes;
          _normalizeTraceTarget();
        });
      } else if (evt is RiftLinkPongEvent) {
        final from = evt.from;
        if (from.isEmpty) return;
        setState(() => _signalRssiByNode[from] = evt.rssi ?? 0);
      }
    });
    widget.ble.getRoutes();
  }

  @override
  void dispose() {
    _sub?.cancel();
    _signalTimer?.cancel();
    super.dispose();
  }

  Future<void> _runSignalTest() async {
    if (_signalTestRunning) return;
    final ok = await widget.ble.signalTest();
    if (!ok || !mounted) {
      _snack(context.l10n.tr('mesh_signal_test_failed'));
      return;
    }
    setState(() {
      _signalTestRunning = true;
      _signalRssiByNode.clear();
    });
    _signalTimer?.cancel();
    _signalTimer = Timer(const Duration(seconds: 10), () {
      if (!mounted) return;
      setState(() => _signalTestRunning = false);
    });
  }

  Future<void> _runTraceroute() async {
    final to = _traceTarget;
    if (to == null || to.isEmpty) return;
    final ok = await widget.ble.traceroute(to);
    if (ok) {
      await widget.ble.getRoutes();
    } else {
      _snack(context.l10n.tr('mesh_traceroute_failed'));
    }
  }

  _GraphData _computeGraph() {
    final nodes = <String, _NP>{};
    final edges = <_Edge>[];

    const center = Offset(0, 0);
    nodes[widget.nodeId] = _NP(center, isSelf: true);

    final n = widget.neighbors.length;
    const r1 = 120.0;
    for (var i = 0; i < n; i++) {
      final angle = 2 * math.pi * i / (n > 0 ? n : 1) - math.pi / 2;
      nodes[widget.neighbors[i]] = _NP(
        Offset(r1 * math.cos(angle), r1 * math.sin(angle)),
        rssi: i < widget.neighborsRssi.length ? widget.neighborsRssi[i] : 0,
      );
      edges.add(_Edge(widget.nodeId, widget.neighbors[i], isDirect: true));
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
      edges.add(_Edge(nextHop.isNotEmpty ? nextHop : widget.nodeId, dest, isDirect: false, hops: hops));
      j++;
    }

    return _GraphData(nodes, edges);
  }

  @override
  Widget build(BuildContext context) {
    final l = context.l10n;
    final graph = _computeGraph();
    final hasListData = widget.neighbors.isNotEmpty ||
        _routes.any((r) => ((r['dest'] as String?) ?? '').isNotEmpty);

    return DefaultTabController(
      length: 2,
      child: Scaffold(
        backgroundColor: context.palette.surface,
        appBar: riftAppBar(
          context,
          title: l.tr('mesh_topology'),
          showBack: true,
          actions: [
            IconButton(
              icon: const Icon(Icons.refresh),
              onPressed: () => widget.ble.getRoutes(),
              tooltip: l.tr('refresh'),
            ),
          ],
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
                  if (_signalTestRunning || _signalRssiByNode.isNotEmpty) ...[
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
                                  '${_shortId(e.key)} ${e.value} dBm',
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
                                  child: Text(_shortId(id)),
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
                  _GraphTab(graph: graph, nodeId: widget.nodeId),
                  _ListTab(
                    neighbors: widget.neighbors,
                    neighborsRssi: widget.neighborsRssi,
                    routes: _routes,
                    hasData: hasListData,
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
    final all = <String>{...widget.neighbors};
    for (final r in _routes) {
      final d = (r['dest'] as String?) ?? '';
      if (d.isNotEmpty) all.add(d);
    }
    all.remove(widget.nodeId);
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
    if (route == null) return l.tr('mesh_trace_no_route');
    final hops = (route['hops'] as num?)?.toInt() ?? 0;
    final pathRaw = route['path'];
    final hopRssiRaw = route['hopRssi'];
    final path = <String>[];
    if (pathRaw is List && pathRaw.isNotEmpty) {
      path.addAll(pathRaw.map((e) => e.toString()));
    } else {
      final next = (route['nextHop'] as String?) ?? '';
      path.add(widget.nodeId);
      if (next.isNotEmpty && next.toUpperCase() != widget.nodeId.toUpperCase()) path.add(next);
      if (hops > 2) path.add('...');
      path.add(target);
    }
    final hopRssi = <int>[];
    if (hopRssiRaw is List) {
      hopRssi.addAll(hopRssiRaw.map((e) => (e as num?)?.toInt() ?? 0));
    } else {
      final firstHop = (route['nextHop'] as String?) ?? '';
      if (firstHop.isNotEmpty) {
        final idx = widget.neighbors.indexWhere((n) => n.toUpperCase() == firstHop.toUpperCase());
        hopRssi.add(idx >= 0 && idx < widget.neighborsRssi.length ? widget.neighborsRssi[idx] : 0);
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
      rendered.add(rssi != 0 ? '${_shortId(node)} ($rssi dBm)' : _shortId(node));
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
      if (next.isNotEmpty) '${l.tr('mesh_col_next')}: ${_shortId(next)}',
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

  String _shortId(String id) {
    if (id.length <= 8) return id.toUpperCase();
    return id.substring(0, 8).toUpperCase();
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

  const _GraphTab({required this.graph, required this.nodeId});

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
                    '${l.tr('settings_node_id')}: ${_shortId(nodeId)}',
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
                  ),
                );
              },
            ),
          ),
        ],
      ),
    );
  }

  String _shortId(String id) {
    if (id.length <= 8) return id;
    return '${id.substring(0, 4)}…';
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

  const _ListTab({
    required this.neighbors,
    required this.neighborsRssi,
    required this.routes,
    required this.hasData,
  });

  String _idLabel(String id) => id.length >= 8 ? id.substring(0, 8).toUpperCase() : id.toUpperCase();

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
                                _idLabel(neighbors[i]),
                                style: TextStyle(
                                  fontFamily: 'monospace',
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
                                    _idLabel(dest),
                                    style: TextStyle(
                                      fontFamily: 'monospace',
                                      fontSize: 14,
                                      fontWeight: FontWeight.w600,
                                      color: context.palette.onSurface,
                                    ),
                                  ),
                                  subtitle: Text(
                                    '${l.tr('mesh_col_next')}: ${next.isNotEmpty ? _idLabel(next) : '—'} · $hops ${l.tr('mesh_route_hops')}${rssi != 0 ? ' · RSSI $rssi' : ''}${modem != null ? ' · ${l.tr('mesh_modem')}: $modem' : ''}${trust != null ? ' · trust $trust' : ''}',
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

  _MeshPainter({required this.nodes, required this.edges, required this.center, required this.palette});

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

      final short = id.length >= 8 ? '${id.substring(0, 4)}…' : id;
      _drawText(canvas, short, Offset(pos.dx, pos.dy + r + 10), 11, palette.onSurface);
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
