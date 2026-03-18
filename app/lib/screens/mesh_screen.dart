import 'dart:async';
import 'dart:math' as math;
import 'package:flutter/material.dart';
import '../widgets/mesh_background.dart';
import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';

class MeshScreen extends StatefulWidget {
  final RiftLinkBle ble;
  final String nodeId;
  final List<String> neighbors;
  final List<int> neighborsRssi;
  final List<Map<String, dynamic>> routes;
  const MeshScreen({super.key, required this.ble, required this.nodeId, required this.neighbors, required this.neighborsRssi, required this.routes});
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
    _sub = widget.ble.events.listen((evt) { if (!mounted) return; if (evt is RiftLinkRoutesEvent) setState(() => _routes = evt.routes); });
    widget.ble.getRoutes();
  }

  @override
  void dispose() { _sub?.cancel(); super.dispose(); }

  @override
  Widget build(BuildContext context) {
    final nodes = <String, _NP>{};
    final edges = <_Edge>[];

    const center = Offset(0, 0);
    nodes[widget.nodeId] = _NP(center, isSelf: true);

    final n = widget.neighbors.length;
    const r1 = 120.0;
    for (var i = 0; i < n; i++) {
      final angle = 2 * math.pi * i / (n > 0 ? n : 1) - math.pi / 2;
      nodes[widget.neighbors[i]] = _NP(Offset(r1 * math.cos(angle), r1 * math.sin(angle)), rssi: i < widget.neighborsRssi.length ? widget.neighborsRssi[i] : 0);
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
      final angle = 2 * math.pi * j / (routeDests.length > 0 ? routeDests.length : 1) - math.pi / 2 + 0.5;
      nodes[dest] = _NP(Offset(r2 * math.cos(angle), r2 * math.sin(angle)), hops: hops, rssi: rssi);
      edges.add(_Edge(nextHop.isNotEmpty ? nextHop : widget.nodeId, dest, isDirect: false, hops: hops));
      j++;
    }

    return Scaffold(
      backgroundColor: Colors.white,
      appBar: AppBar(
        title: Text(context.l10n.tr('mesh_topology')),
        leading: IconButton(icon: const Icon(Icons.arrow_back), onPressed: () => Navigator.pop(context)),
        actions: [IconButton(icon: const Icon(Icons.refresh), onPressed: () => widget.ble.getRoutes(), tooltip: context.l10n.tr('refresh'))],
      ),
      body: Material(
        color: Colors.white,
        child: LayoutBuilder(builder: (_, constraints) {
        final size = Size(constraints.maxWidth, constraints.maxHeight);
        final c = Offset(size.width / 2, size.height / 2);
        return CustomPaint(size: size, painter: _MeshPainter(nodes: nodes, edges: edges, center: c));
      }),
        ),
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
  _MeshPainter({required this.nodes, required this.edges, required this.center});

  Offset _pos(String id) { final n = nodes[id]; return n == null ? center : Offset(center.dx + n.pos.dx, center.dy + n.pos.dy); }

  @override
  void paint(Canvas canvas, Size size) {
    final edgePaint = Paint()..color = Colors.grey.shade400..strokeWidth = 2..style = PaintingStyle.stroke;
    final routePaint = Paint()..color = Colors.blue.shade400..strokeWidth = 2..style = PaintingStyle.stroke;

    for (final e in edges) {
      final from = _pos(e.from); final to = _pos(e.to);
      canvas.drawLine(from, to, e.isDirect ? edgePaint : routePaint);
      if (!e.isDirect && e.hops > 0) _drawText(canvas, '${e.hops}h', Offset((from.dx + to.dx) / 2, (from.dy + to.dy) / 2), 10, Colors.blue);
    }

    for (final e in nodes.entries) {
      final id = e.key; final n = e.value; final pos = _pos(id);
      canvas.drawCircle(pos, n.isSelf ? 24 : 18, Paint()..color = n.isSelf ? Colors.green : (n.hops > 0 ? Colors.blue.shade100 : Colors.orange.shade100)..style = PaintingStyle.fill);
      canvas.drawCircle(pos, n.isSelf ? 24 : 18, Paint()..color = n.isSelf ? Colors.green.shade700 : Colors.grey.shade700..style = PaintingStyle.stroke..strokeWidth = 2);
      _drawText(canvas, id.length >= 8 ? '${id.substring(0, 4)}…' : id, Offset(pos.dx, pos.dy + 28), 11, Colors.black87);
      if (n.rssi != 0) _drawText(canvas, '${n.rssi}', Offset(pos.dx, pos.dy + 40), 9, Colors.grey);
      if (n.hops > 0) _drawText(canvas, '${n.hops}h', Offset(pos.dx, pos.dy + (n.rssi != 0 ? 50 : 40)), 9, Colors.blue);
    }
  }

  void _drawText(Canvas canvas, String text, Offset pos, double fontSize, Color color) {
    final tp = TextPainter(text: TextSpan(text: text, style: TextStyle(fontSize: fontSize, color: color)), textDirection: TextDirection.ltr)..layout();
    tp.paint(canvas, Offset(pos.dx - tp.width / 2, pos.dy));
  }

  @override
  bool shouldRepaint(covariant _MeshPainter old) => old.nodes != nodes || old.edges != edges;
}
