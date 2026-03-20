import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../widgets/mesh_background.dart';

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
  StreamSubscription? _sub;

  @override
  void initState() {
    super.initState();
    _routes = List.from(widget.routes);
    _sub = widget.ble.events.listen((evt) {
      if (!mounted) return;
      if (evt is RiftLinkRoutesEvent) setState(() => _routes = evt.routes);
    });
    widget.ble.getRoutes();
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
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
        appBar: AppBar(
          backgroundColor: context.palette.surfaceVariant,
          foregroundColor: context.palette.onSurface,
          title: Text(l.tr('mesh_topology')),
          leading: IconButton(
            icon: const Icon(Icons.arrow_back),
            onPressed: () => Navigator.pop(context),
          ),
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
            labelStyle: const TextStyle(fontWeight: FontWeight.w600, fontSize: 13),
            tabs: [
              Tab(icon: const Icon(Icons.account_tree, size: 20), text: l.tr('mesh_tab_graph')),
              Tab(icon: const Icon(Icons.view_list_rounded, size: 20), text: l.tr('mesh_tab_list')),
            ],
          ),
        ),
        body: TabBarView(
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
    );
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
            padding: const EdgeInsets.fromLTRB(16, 12, 16, 8),
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
              decoration: BoxDecoration(
                color: context.palette.card,
                borderRadius: BorderRadius.circular(12),
                border: Border.all(color: context.palette.divider),
              ),
              child: Wrap(
                spacing: 16,
                runSpacing: 8,
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
                  Text(
                    '${l.tr('settings_node_id')}: ${_shortId(nodeId)}',
                    style: TextStyle(fontSize: 12, color: context.palette.onSurfaceVariant.withOpacity(0.95)),
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
            borderRadius: BorderRadius.circular(2),
          ),
        ),
        const SizedBox(width: 8),
        Text(label, style: TextStyle(fontSize: 12, color: context.palette.onSurface)),
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
                  padding: const EdgeInsets.all(32),
                  child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      Icon(Icons.device_hub_outlined, size: 56, color: context.palette.onSurfaceVariant.withOpacity(0.45)),
                      const SizedBox(height: 16),
                      Text(
                        l.tr('mesh_empty'),
                        textAlign: TextAlign.center,
                        style: TextStyle(color: context.palette.onSurfaceVariant.withOpacity(0.95), fontSize: 14, height: 1.4),
                      ),
                    ],
                  ),
                ),
              )
            : ListView(
                padding: const EdgeInsets.fromLTRB(16, 12, 16, 24),
                children: [
                  if (neighbors.isNotEmpty) ...[
                    _SectionTitle(title: l.tr('mesh_list_neighbors')),
                    const SizedBox(height: 8),
                    Card(
                      color: context.palette.card,
                      elevation: 0,
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(12),
                        side: BorderSide(color: context.palette.divider),
                      ),
                      child: Column(
                        children: [
                          for (var i = 0; i < neighbors.length; i++) ...[
                            if (i > 0) Divider(height: 1, color: context.palette.divider),
                            ListTile(
                              leading: CircleAvatar(
                                backgroundColor: context.palette.primary.withOpacity(0.2),
                                child: Icon(Icons.router_outlined, color: context.palette.primary, size: 22),
                              ),
                              title: Text(
                                _idLabel(neighbors[i]),
                                style: TextStyle(fontFamily: 'monospace', fontSize: 14, fontWeight: FontWeight.w600, color: context.palette.onSurface),
                              ),
                              subtitle: Text(
                                '${l.tr('neighbors')} · RSSI ${i < neighborsRssi.length ? neighborsRssi[i] : '—'}',
                                style: TextStyle(fontSize: 12, color: context.palette.onSurfaceVariant.withOpacity(0.95)),
                              ),
                            ),
                          ],
                        ],
                      ),
                    ),
                    const SizedBox(height: 20),
                  ],
                  if (validRoutes.isNotEmpty) ...[
                    _SectionTitle(title: l.tr('mesh_list_routes')),
                    const SizedBox(height: 8),
                    Card(
                      color: context.palette.card,
                      elevation: 0,
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(12),
                        side: BorderSide(color: context.palette.divider),
                      ),
                      child: Column(
                        children: [
                          for (var i = 0; i < validRoutes.length; i++) ...[
                            if (i > 0) Divider(height: 1, color: context.palette.divider),
                            Builder(
                              builder: (_) {
                                final r = validRoutes[i];
                                final dest = (r['dest'] as String?) ?? '';
                                final next = (r['nextHop'] as String?) ?? '';
                                final hops = (r['hops'] as num?)?.toInt() ?? 0;
                                final rssi = (r['rssi'] as num?)?.toInt() ?? 0;
                                return ListTile(
                                  leading: CircleAvatar(
                                    backgroundColor: context.palette.success.withOpacity(0.18),
                                    child: Icon(Icons.alt_route, color: context.palette.success, size: 22),
                                  ),
                                  title: Text(
                                    _idLabel(dest),
                                    style: TextStyle(fontFamily: 'monospace', fontSize: 14, fontWeight: FontWeight.w600, color: context.palette.onSurface),
                                  ),
                                  subtitle: Text(
                                    '${l.tr('mesh_col_next')}: ${next.isNotEmpty ? _idLabel(next) : '—'} · $hops ${l.tr('mesh_route_hops')}${rssi != 0 ? ' · RSSI $rssi' : ''}',
                                    style: TextStyle(fontSize: 12, color: context.palette.onSurfaceVariant.withOpacity(0.95)),
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
          height: 16,
          decoration: BoxDecoration(
            color: context.palette.primary,
            borderRadius: BorderRadius.circular(2),
          ),
        ),
        const SizedBox(width: 10),
        Text(
          title,
          style: TextStyle(
            fontSize: 14,
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
      ..color = palette.onSurfaceVariant.withOpacity(0.45)
      ..strokeWidth = 2
      ..style = PaintingStyle.stroke;

    final routePaint = Paint()
      ..color = palette.primary.withOpacity(0.85)
      ..strokeWidth = 2.5
      ..style = PaintingStyle.stroke;

    for (final e in edges) {
      final from = _pos(e.from);
      final to = _pos(e.to);
      canvas.drawLine(from, to, e.isDirect ? edgePaint : routePaint);
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
      final fill = n.isSelf
          ? palette.success.withOpacity(0.35)
          : (n.hops > 0 ? palette.primary.withOpacity(0.22) : palette.primary.withOpacity(0.14));
      final stroke = n.isSelf ? palette.success : palette.primary.withOpacity(0.8);

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
        _drawText(canvas, '${n.rssi} dBm', Offset(pos.dx, pos.dy + r + 24), 9, palette.onSurfaceVariant);
      }
      if (n.hops > 0) {
        _drawText(
          canvas,
          '${n.hops}h',
          Offset(pos.dx, pos.dy + r + (n.rssi != 0 ? 36 : 24)),
          9,
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
