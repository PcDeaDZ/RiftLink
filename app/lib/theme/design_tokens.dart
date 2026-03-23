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

  /// Горизонтальный зазор в сегментированном баре (рядом с индикатором).
  static const double segmentInnerH = 2;
}

/// Радиусы скругления (согласованы с [AppTheme]).
abstract final class AppRadius {
  static const double sm = 8;
  static const double md = 12;
  static const double lg = 14;
  static const double card = 12;
  /// Трек кастомного переключателя [RiftSwitch].
  static const double switchTrack = 14;
  /// Диалоги, snackbar, overlay-панели.
  static const double overlay = 16;
}

/// Размеры иконок (логические px).
abstract final class AppIconSize {
  /// Вторичные иконки в строках (замок, мелкий индикатор).
  static const double compact = 16;
  static const double sm = 18;
  static const double md = 20;
  static const double lg = 22;
  static const double xl = 24;
  /// Крупная иконка в шапке диалога (selftest и т.п.).
  static const double xxl = 26;
  /// Узел на карте.
  static const double mapMarker = 32;
  /// Текущее положение пользователя на карте.
  static const double mapUserPin = 40;
  /// Пустое состояние (карта и т.п.).
  static const double emptyStateHero = 48;
  /// Крупное пустое состояние (mesh и т.п.).
  static const double emptyStateLarge = 56;
  /// Очень крупная иллюстрация пустого состояния (контакты и т.п.).
  static const double emptyStateXLarge = 72;
  /// Средняя иллюстрация (список чатов / соседи).
  static const double emptyStateMedium = 52;
}

/// Высота панели инструментов AppBar.
abstract final class AppBarMetrics {
  static const double toolbarHeight = 48;
}

/// Трек и бегунок [RiftSwitch] (логические px).
abstract final class AppSwitchMetrics {
  static const double trackWidth = 48;
  static const double trackHeight = 28;
  static const double padding = 3;
  static const double knobSize = 22;
}

/// Размеры экрана OTA (прогресс, иконки).
abstract final class AppOtaLayout {
  static const double heroIcon = 64;
  static const double ring = 120;
  static const double indeterminateSpinner = 40;
  static const double ringStroke = 6;
  static const double indeterminateStroke = 3;
}

/// Тени и «высота» для Material-виджетов (логические px).
abstract final class AppElevation {
  static const double none = 0;
  /// [RiftDialogFrame] и подобные Material с тенью.
  static const double dialog = 6;
  static const double snackBar = 8;
  static const double fab = 2;
}

/// Готовые тени (цвет зависит от контекста — методы с параметром).
abstract final class AppShadow {
  static List<BoxShadow> primarySegment(Color primary) => [
        BoxShadow(
          color: primary.withOpacity(0.2),
          blurRadius: 12,
          offset: const Offset(0, 2),
        ),
      ];

  static const List<BoxShadow> switchKnob = [
    BoxShadow(
      color: Color(0x26000000),
      blurRadius: 4,
      offset: Offset(0, 1),
    ),
  ];
}

/// Длительности и кривые анимаций.
abstract final class AppMotion {
  static const Duration shortest = Duration(milliseconds: 150);
  static const Duration standard = Duration(milliseconds: 200);
  static const Duration medium = Duration(milliseconds: 300);
  /// Сегментированный бар: сдвиг индикатора.
  static const Duration segmentSlide = Duration(milliseconds: 320);
  /// Сегментированный бар / переключатель: смена текста и микровзаимодействия.
  static const Duration segmentCross = Duration(milliseconds: 220);
  static const Curve curve = Curves.easeInOut;
  static const Curve easeOutCubic = Curves.easeOutCubic;
}

/// Типографика: размеры и начертания без цвета (цвет — через [context.palette] в виджетах).
abstract final class AppTypography {
  /// Заголовок AppBar и навигации — единая роль (согласовано с [AppTheme.appBarTheme]).
  static const double navTitleSize = 18;
  static const FontWeight navTitleWeight = FontWeight.w600;
  static const double navTitleHeight = 1.2;

  /// Крупный заголовок внутри экрана (модалки, акценты).
  static const double headlineSize = 22;
  /// Заголовок панели (OTA, карточки), не AppBar.
  static const double panelTitleSize = 20;
  /// Крупная цифра статуса (процент OTA и т.п.).
  static const double statDisplaySize = 24;

  /// Обратная совместимость: «экранный» заголовок = навигационный.
  static const double screenTitleSize = navTitleSize;
  static const FontWeight screenTitleWeight = navTitleWeight;
  static const double screenTitleHeight = navTitleHeight;

  static const double bodySize = 15;
  static const FontWeight bodyWeight = FontWeight.w400;
  static const double bodyHeight = 1.35;

  static const double bodyLargeSize = 16;
  static const FontWeight bodyLargeWeight = FontWeight.w400;

  static const double labelSize = 13;
  static const FontWeight labelWeight = FontWeight.w500;

  static const double chipSize = 12;
  static const FontWeight chipWeight = FontWeight.w600;

  /// Вторичный текст, подписи к полям.
  static const double captionSize = 11;
  static const FontWeight captionWeight = FontWeight.w400;
  static const double captionHeight = 1.35;

  /// Компактные списки, метки времени.
  static const double captionDenseSize = 10;

  /// Мелкий текст (бейджи, подсказки).
  static const double microSize = 9;

  /// Метаданные monospace (ID узла и т.п.).
  static const double monoSize = 12;
  static const double monoSmallSize = 11;

  static TextStyle navTitleBase() => const TextStyle(
        fontSize: navTitleSize,
        fontWeight: navTitleWeight,
        height: navTitleHeight,
      );

  static TextStyle screenTitleBase() => navTitleBase();

  static TextStyle headlineBase() => const TextStyle(
        fontSize: headlineSize,
        fontWeight: FontWeight.w600,
        height: 1.25,
      );

  static TextStyle panelTitleBase() => const TextStyle(
        fontSize: panelTitleSize,
        fontWeight: FontWeight.w600,
        height: 1.25,
      );

  static TextStyle statDisplayBase() => const TextStyle(
        fontSize: statDisplaySize,
        fontWeight: FontWeight.bold,
        height: 1.2,
      );

  static TextStyle bodyBase() => const TextStyle(
        fontSize: bodySize,
        fontWeight: bodyWeight,
        height: bodyHeight,
      );

  static TextStyle bodyLargeBase() => const TextStyle(
        fontSize: bodyLargeSize,
        fontWeight: bodyLargeWeight,
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

  static TextStyle captionBase() => const TextStyle(
        fontSize: captionSize,
        fontWeight: captionWeight,
        height: captionHeight,
      );

  static TextStyle captionDenseBase() => const TextStyle(
        fontSize: captionDenseSize,
        fontWeight: captionWeight,
        height: 1.3,
      );

  static TextStyle microBase() => const TextStyle(
        fontSize: microSize,
        fontWeight: FontWeight.w500,
        height: 1.2,
      );

  static TextStyle monoBase() => const TextStyle(
        fontSize: monoSize,
        fontWeight: FontWeight.w400,
        fontFamily: 'monospace',
        height: 1.35,
      );

  static TextStyle monoSmallBase() => const TextStyle(
        fontSize: monoSmallSize,
        fontWeight: FontWeight.w400,
        fontFamily: 'monospace',
        height: 1.35,
      );
}
