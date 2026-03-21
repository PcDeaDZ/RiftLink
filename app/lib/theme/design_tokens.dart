import 'package:flutter/material.dart';

/// Единая шкала отступов (4pt grid).
abstract final class AppSpacing {
  static const double xs = 4;
  static const double sm = 8;
  static const double md = 12;
  static const double lg = 16;
  static const double xl = 20;
  static const double xxl = 24;

  /// Как в [AppTheme] для Filled/Elevated primary.
  static const double buttonPrimaryV = 14;
  static const double buttonPrimaryH = 20;
  static const double buttonSecondaryV = 12;
  static const double buttonSecondaryH = 16;
  static const double inputH = 16;
  static const double inputV = 14;
}

/// Радиусы скругления (согласованы с [AppTheme]).
abstract final class AppRadius {
  static const double sm = 8;
  static const double md = 12;
  static const double lg = 14;
  static const double card = 12;
}

/// Длительности и кривые анимаций.
abstract final class AppMotion {
  static const Duration shortest = Duration(milliseconds: 150);
  static const Duration standard = Duration(milliseconds: 200);
  static const Duration medium = Duration(milliseconds: 300);
  static const Curve curve = Curves.easeInOut;
}

/// Типографика: размеры и начертания без цвета (цвет — через [context.palette] в виджетах).
abstract final class AppTypography {
  static const double screenTitleSize = 20;
  static const FontWeight screenTitleWeight = FontWeight.w600;
  static const double screenTitleHeight = 1.2;

  static const double bodySize = 15;
  static const FontWeight bodyWeight = FontWeight.w400;
  static const double bodyHeight = 1.35;

  static const double labelSize = 13;
  static const FontWeight labelWeight = FontWeight.w500;

  static const double chipSize = 12;
  static const FontWeight chipWeight = FontWeight.w600;

  static TextStyle screenTitleBase() => const TextStyle(
        fontSize: screenTitleSize,
        fontWeight: screenTitleWeight,
        height: screenTitleHeight,
      );

  static TextStyle bodyBase() => const TextStyle(
        fontSize: bodySize,
        fontWeight: bodyWeight,
        height: bodyHeight,
      );

  static TextStyle labelBase() => const TextStyle(
        fontSize: labelSize,
        fontWeight: labelWeight,
      );

  static TextStyle chipBase() => const TextStyle(
        fontSize: chipSize,
        fontWeight: chipWeight,
      );
}
