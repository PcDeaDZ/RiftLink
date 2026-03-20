import 'package:flutter/material.dart';
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

    final points = <Offset>[];
    var y = 0.0;
    var row = 0;
    while (y <= size.height * 1.5) {
      final spacing = baseSpacing + (y / size.height) * 24;
      final cols = (size.width / spacing).floor() + 2;
      for (var c = 0; c < cols; c++) {
        final x = c * spacing + (row.isOdd ? spacing * 0.5 : 0);
        if (x <= size.width + spacing) {
          points.add(Offset(x, y));
        }
      }
      y += spacing * 0.85;
      row++;
    }

    for (final p in points) {
      canvas.drawCircle(p, dotRadius, paint);
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
      final animEdges = edges.where((e) => e.a.dy < animLimitY && e.b.dy < animLimitY).toList();
      if (animEdges.isEmpty) return;
      final pulsePaint = Paint()..color = palette.primary.withOpacity(pulseOpacity);
      final maxPulses = animEdges.length.clamp(0, 24);
      for (var k = 0; k < maxPulses; k++) {
        final phase = (k / maxPulses) % 1.0;
        final t = (progress + phase) % 1.0;
        final edge = animEdges[k % animEdges.length];
        final x = edge.a.dx + (edge.b.dx - edge.a.dx) * t;
        final y = edge.a.dy + (edge.b.dy - edge.a.dy) * t;
        canvas.drawCircle(Offset(x, y), pulseRadius, pulsePaint);
      }
    }
  }

  @override
  bool shouldRepaint(covariant MeshBackgroundPainter oldDelegate) =>
      oldDelegate.progress != progress ||
      oldDelegate.animated != animated ||
      oldDelegate.palette != palette;
}
