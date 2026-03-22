import 'package:flutter/material.dart';
import 'dart:math' as math;
import '../prefs/mesh_prefs.dart';
import '../theme/app_theme.dart';

/// Обёртка с mesh-фоном для любого экрана
class MeshBackgroundWrapper extends StatefulWidget {
  final Widget child;

  const MeshBackgroundWrapper({super.key, required this.child});

  @override
  State<MeshBackgroundWrapper> createState() => _MeshBackgroundWrapperState();
}

class _MeshBackgroundWrapperState extends State<MeshBackgroundWrapper> with SingleTickerProviderStateMixin {
  bool _animated = true;
  AnimationController? _controller;

  @override
  void initState() {
    super.initState();
    MeshPrefs.getAnimationEnabled().then((v) {
      if (mounted) {
        setState(() => _animated = v);
        if (v) {
          _controller ??= AnimationController(vsync: this, duration: const Duration(seconds: 4))..repeat();
        }
      }
    });
  }

  @override
  void dispose() {
    _controller?.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final p = context.palette;
    return Stack(
      fit: StackFit.expand,
      children: [
        Positioned.fill(
          child: IgnorePointer(
            child: ColoredBox(
              color: p.surface,
              child: ListenableBuilder(
                listenable: _animated && _controller != null ? _controller! : const AlwaysStoppedAnimation(0),
                builder: (_, __) => CustomPaint(
                  painter: MeshBackgroundPainter(
                    progress: _controller?.value ?? 0,
                    animated: _animated,
                    palette: p,
                  ),
                ),
              ),
            ),
          ),
        ),
        widget.child,
      ],
    );
  }
}

/// Абстрактный фон: узлы сети и тонкие линии связи с опциональной анимацией импульсов
class MeshBackgroundPainter extends CustomPainter {
  MeshBackgroundPainter({this.progress = 0, this.animated = false, required this.palette});

  final double progress;
  final bool animated;
  final AppPalette palette;

  double _densityForY(double y, double height) {
    final yNorm = (y / height).clamp(0.0, 1.0);
    const fadeStart = 0.92;
    const minDensity = 0.86;
    if (yNorm <= fadeStart) return 1.0;
    final t = (yNorm - fadeStart) / (1.0 - fadeStart);
    return 1.0 - (1.0 - minDensity) * math.pow(t, 1.35);
  }

  double _stableNoise(Offset p) {
    final v = math.sin(p.dx * 12.9898 + p.dy * 78.233) * 43758.5453;
    return v - v.floorToDouble();
  }

  double _pingPong01(double x) {
    final f = x - x.floorToDouble();
    return f < 0.5 ? f * 2.0 : (1.0 - f) * 2.0;
  }

  @override
  void paint(Canvas canvas, Size size) {
    const baseSpacing = 48.0;
    const dotRadius = 1.2;
    const lineOpacity = 0.04;
    const dotOpacity = 0.06;
    const pulseRadius = 2.0;
    const pulseOpacity = 0.12;
    const animZoneFraction = 1.0;

    final paint = Paint()..color = palette.primary.withOpacity(dotOpacity);
    final linePaint = Paint()
      ..color = palette.primary.withOpacity(lineOpacity)
      ..strokeWidth = 0.8
      ..style = PaintingStyle.stroke;

    final allPoints = <Offset>[];
    var y = 0.0;
    var row = 0;
    while (y <= size.height * 2.2) {
      final spacing = baseSpacing + (y / size.height) * 24;
      final cols = (size.width / spacing).floor() + 2;
      for (var c = 0; c < cols; c++) {
        final x = c * spacing + (row.isOdd ? spacing * 0.5 : 0);
        if (x <= size.width + spacing) {
          allPoints.add(Offset(x, y));
        }
      }
      y += spacing * 0.85;
      row++;
    }

    final points = <Offset>[];
    for (final p in allPoints) {
      final density = _densityForY(p.dy, size.height);
      if (_stableNoise(p) <= density) {
        points.add(p);
      }
    }

    for (final p in points) {
      canvas.drawCircle(p, dotRadius, paint);
    }

    // Bottom tail: keep sparse but visible points close to the lower edge.
    final tailPaint = Paint()..color = palette.primary.withOpacity(0.06);
    for (final p in allPoints) {
      if (p.dy < size.height * 0.68 || p.dy > size.height * 1.1) continue;
      final h = _stableNoise(Offset(p.dx + 19.0, p.dy + 47.0));
      if (h <= 0.34) {
        canvas.drawCircle(p, 1.05, tailPaint);
      }
    }

    final edges = <({Offset a, Offset b})>[];
    for (var i = 0; i < points.length; i++) {
      for (var j = i + 1; j < points.length; j++) {
        final dist = (points[i] - points[j]).distance;
        final maxDist = (baseSpacing + 24) * 1.5;
        if (dist < maxDist) {
          final a = points[i].dy < points[j].dy ? points[i] : points[j];
          final b = points[i].dy < points[j].dy ? points[j] : points[i];
          edges.add((a: a, b: b));
          canvas.drawLine(points[i], points[j], linePaint);
        }
      }
    }

    if (animated && edges.isNotEmpty) {
      final animLimitY = size.height * animZoneFraction;
      final animEdges = edges
          .where((e) => e.a.dy < animLimitY && e.b.dy < animLimitY)
          .where((e) =>
              e.a.dx >= -8 &&
              e.a.dx <= size.width + 8 &&
              e.b.dx >= -8 &&
              e.b.dx <= size.width + 8 &&
              e.a.dy >= -8 &&
              e.b.dy >= -8 &&
              e.a.dy <= size.height + 8 &&
              e.b.dy <= size.height + 8)
          .where((e) {
            final yMid = (e.a.dy + e.b.dy) * 0.5;
            final density = _densityForY(yMid, size.height);
            final boostedDensity = yMid > size.height * 0.76
                ? math.max(density, 0.82)
                : density;
            final edgeSeed = Offset(e.a.dx + e.b.dx, e.a.dy + e.b.dy);
            return _stableNoise(edgeSeed) <= boostedDensity;
          })
          .toList();
      if (animEdges.isEmpty) return;
      final pulsePaintSoft = Paint()..color = palette.primary.withOpacity(pulseOpacity * 0.85);
      final pulsePaintBright = Paint()..color = palette.primary.withOpacity(pulseOpacity * 1.25);
      String pointKey(Offset p) => '${p.dx.toStringAsFixed(2)}:${p.dy.toStringAsFixed(2)}';
      final byPoint = <String, List<Offset>>{};
      for (final e in animEdges) {
        final ka = pointKey(e.a);
        final kb = pointKey(e.b);
        (byPoint[ka] ??= <Offset>[]).add(e.b);
        (byPoint[kb] ??= <Offset>[]).add(e.a);
      }
      Offset pickNextHop(Offset prev, Offset cur, double selector, {required bool preferDown}) {
        final curKey = pointKey(cur);
        final prevKey = pointKey(prev);
        final all = (byPoint[curKey] ?? const <Offset>[])
            .where((n) => pointKey(n) != prevKey)
            .toList();
        if (all.isEmpty) return prev;
        final directed = preferDown
            ? all.where((n) => n.dy >= cur.dy + 0.5).toList()
            : all.where((n) => n.dy <= cur.dy - 0.5).toList();
        final pool = directed.isNotEmpty ? directed : all;
        if (preferDown && pool.length > 1) {
          // Push particles deeper: prefer lower candidates in the pool.
          pool.sort((a, b) => b.dy.compareTo(a.dy));
          final topCount = math.max(1, (pool.length * 0.85).ceil());
          final top = pool.take(topCount).toList();
          final idx = (selector * top.length).floor().clamp(0, top.length - 1);
          return top[idx];
        }
        final idx = (selector * pool.length).floor().clamp(0, pool.length - 1);
        return pool[idx];
      }
      // Use real time to avoid cycle seams from 0..1 progress reset.
      final timeSec = DateTime.now().microsecondsSinceEpoch / 1000000.0;
      var drawn = 0;
      for (final edge in animEdges) {
        final seed = Offset(
          edge.a.dx * 0.71 + edge.b.dx * 1.31,
          edge.a.dy * 1.91 + edge.b.dy * 0.47,
        );
        final h = _stableNoise(seed);
        // Deterministic sparse subset: slightly calmer density.
        if (h > 0.22) continue;

        // Some edges can carry two pulses, but avoid excessive clutter.
        final laneCount = h < 0.10 ? 2 : 1;
        for (var lane = 0; lane < laneCount; lane++) {
          final laneSeed = Offset(seed.dx + lane * 13.7, seed.dy + lane * 9.1);
          final lh = _stableNoise(laneSeed);
          // Tighter speed range to avoid very fast jumps.
          final speed = 0.018 + lh * 0.05;
          final phase = h * 7.0 + lh * 13.0 + lane * 0.61;
          final v = timeSec * speed + phase;
          final turn = v.floorToDouble();
          final cycle = (v - turn) * 14.0; // 0..14
          final seg = cycle.floor();
          final u = cycle - seg;

          final a = edge.a;
          final b = edge.b;
          double selectorFor(int salt) {
            return _stableNoise(
              Offset(
                seed.dx * 0.17 + lane * 11.0 + turn * 0.73 + salt * 7.0,
                seed.dy * 0.23 + lane * 13.0 + turn * 1.11 + salt * 5.0,
              ),
            );
          }
          final c = pickNextHop(a, b, selectorFor(1), preferDown: true);
          final d = pickNextHop(b, c, selectorFor(2), preferDown: true);
          final e = pickNextHop(c, d, selectorFor(3), preferDown: true);
          final f = pickNextHop(d, e, selectorFor(4), preferDown: true);
          final g = pickNextHop(e, f, selectorFor(5), preferDown: true);
          final h2 = pickNextHop(f, g, selectorFor(6), preferDown: true);

          late Offset from;
          late Offset to;
          switch (seg) {
            case 0:
              from = a;
              to = b;
              break; // A -> B
            case 1:
              from = b;
              to = c;
              break; // B -> C
            case 2:
              from = c;
              to = d;
              break; // C -> D
            case 3:
              from = d;
              to = e;
              break; // D -> E
            case 4:
              from = e;
              to = f;
              break; // E -> F
            case 5:
              from = f;
              to = g;
              break; // F -> G
            case 6:
              from = g;
              to = h2;
              break; // G -> H
            case 7:
              from = h2;
              to = g;
              break; // H -> G
            case 8:
              from = g;
              to = f;
              break; // G -> F
            case 9:
              from = f;
              to = e;
              break; // F -> E
            case 10:
              from = e;
              to = d;
              break; // E -> D
            case 11:
              from = d;
              to = c;
              break; // D -> C
            case 12:
              from = c;
              to = b;
              break; // C -> B
            default:
              from = b;
              to = a;
              break; // B -> A
          }

          final xRaw = from.dx + (to.dx - from.dx) * u;
          final yRaw = from.dy + (to.dy - from.dy) * u;
          final y = yRaw.clamp(-8.0, size.height + 8.0);

          final paint = (lane % 2 == 0) ? pulsePaintBright : pulsePaintSoft;
          final radius = pulseRadius * (0.9 + lh * 0.45);
          canvas.drawCircle(Offset(xRaw, y), radius, paint);

          drawn++;
          if (drawn >= 48) break; // cap cost and visual clutter
        }
        if (drawn >= 48) break;
      }
    }
  }

  @override
  bool shouldRepaint(covariant MeshBackgroundPainter oldDelegate) =>
      oldDelegate.progress != progress ||
      oldDelegate.animated != animated ||
      oldDelegate.palette != palette;
}
