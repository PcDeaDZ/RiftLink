import 'package:flutter/material.dart';

import 'design_tokens.dart';

/// Палитра приложения (дневная / ночная) — [ThemeExtension] для доступа через [BuildContext].
@immutable
class AppPalette extends ThemeExtension<AppPalette> {
  final Color primary;
  final Color surface;
  final Color surfaceVariant;
  final Color card;
  final Color onSurface;
  final Color onSurfaceVariant;
  final Color divider;
  final Color error;
  final Color success;

  const AppPalette({
    required this.primary,
    required this.surface,
    required this.surfaceVariant,
    required this.card,
    required this.onSurface,
    required this.onSurfaceVariant,
    required this.divider,
    required this.error,
    required this.success,
  });

  static const AppPalette dark = AppPalette(
    primary: Color(0xFF42A5F5),
    surface: Color(0xFF121212),
    surfaceVariant: Color(0xFF1E1E1E),
    card: Color(0xFF2D2D2D),
    onSurface: Color(0xFFE0E0E0),
    onSurfaceVariant: Color(0xFFB0B0B0),
    divider: Color(0xFF3D3D3D),
    error: Color(0xFFCF6679),
    success: Color(0xFF66BB6A),
  );

  static const AppPalette light = AppPalette(
    primary: Color(0xFF1565C0),
    surface: Color(0xFFF5F7FA),
    surfaceVariant: Color(0xFFE8ECEF),
    card: Color(0xFFFFFFFF),
    onSurface: Color(0xFF1C1B1F),
    onSurfaceVariant: Color(0xFF49454F),
    divider: Color(0xFFCAC4D0),
    error: Color(0xFFB3261E),
    success: Color(0xFF2E7D32),
  );

  @override
  AppPalette copyWith({
    Color? primary,
    Color? surface,
    Color? surfaceVariant,
    Color? card,
    Color? onSurface,
    Color? onSurfaceVariant,
    Color? divider,
    Color? error,
    Color? success,
  }) {
    return AppPalette(
      primary: primary ?? this.primary,
      surface: surface ?? this.surface,
      surfaceVariant: surfaceVariant ?? this.surfaceVariant,
      card: card ?? this.card,
      onSurface: onSurface ?? this.onSurface,
      onSurfaceVariant: onSurfaceVariant ?? this.onSurfaceVariant,
      divider: divider ?? this.divider,
      error: error ?? this.error,
      success: success ?? this.success,
    );
  }

  @override
  AppPalette lerp(ThemeExtension<AppPalette>? other, double t) {
    if (other is! AppPalette) return this;
    return AppPalette(
      primary: Color.lerp(primary, other.primary, t)!,
      surface: Color.lerp(surface, other.surface, t)!,
      surfaceVariant: Color.lerp(surfaceVariant, other.surfaceVariant, t)!,
      card: Color.lerp(card, other.card, t)!,
      onSurface: Color.lerp(onSurface, other.onSurface, t)!,
      onSurfaceVariant: Color.lerp(onSurfaceVariant, other.onSurfaceVariant, t)!,
      divider: Color.lerp(divider, other.divider, t)!,
      error: Color.lerp(error, other.error, t)!,
      success: Color.lerp(success, other.success, t)!,
    );
  }
}

extension AppPaletteContext on BuildContext {
  AppPalette get palette => Theme.of(this).extension<AppPalette>() ?? AppPalette.dark;
}

class AppTheme {
  static final BorderRadius _buttonRadius = BorderRadius.circular(AppRadius.sm);

  static ButtonStyle _primaryFilledStyle(AppPalette p) {
    return FilledButton.styleFrom(
      backgroundColor: p.primary,
      foregroundColor: Colors.white,
      disabledBackgroundColor: p.primary.withOpacity(0.38),
      disabledForegroundColor: Colors.white70,
      padding: const EdgeInsets.symmetric(
        vertical: AppSpacing.buttonPrimaryV,
        horizontal: AppSpacing.buttonPrimaryH,
      ),
      minimumSize: const Size(0, 48),
      shape: RoundedRectangleBorder(borderRadius: _buttonRadius),
    );
  }

  static ButtonStyle _primaryElevatedStyle(AppPalette p) {
    return ElevatedButton.styleFrom(
      backgroundColor: p.primary,
      foregroundColor: Colors.white,
      disabledBackgroundColor: p.primary.withOpacity(0.38),
      disabledForegroundColor: Colors.white70,
      padding: const EdgeInsets.symmetric(
        vertical: AppSpacing.buttonSecondaryV,
        horizontal: AppSpacing.buttonPrimaryH,
      ),
      minimumSize: const Size(0, 44),
      elevation: 0,
      shape: RoundedRectangleBorder(borderRadius: _buttonRadius),
    );
  }

  static ButtonStyle _outlinedSecondaryStyle(AppPalette p) {
    return OutlinedButton.styleFrom(
      foregroundColor: p.primary,
      side: BorderSide(color: p.divider),
      padding: const EdgeInsets.symmetric(
        vertical: AppSpacing.buttonSecondaryV,
        horizontal: AppSpacing.lg,
      ),
      minimumSize: const Size(0, 44),
      shape: RoundedRectangleBorder(borderRadius: _buttonRadius),
    );
  }

  /// Разрушающее действие — те же отступы, что у filled.
  static ButtonStyle destructiveFilledStyle(AppPalette p) {
    return FilledButton.styleFrom(
      backgroundColor: p.error.withOpacity(0.85),
      foregroundColor: Colors.white,
      disabledBackgroundColor: p.error.withOpacity(0.35),
      disabledForegroundColor: Colors.white70,
      padding: const EdgeInsets.symmetric(
        vertical: AppSpacing.buttonPrimaryV,
        horizontal: AppSpacing.buttonPrimaryH,
      ),
      minimumSize: const Size(0, 48),
      shape: RoundedRectangleBorder(borderRadius: _buttonRadius),
    );
  }

  static ThemeData _build(AppPalette p, Brightness brightness) {
    final isDark = brightness == Brightness.dark;
    return ThemeData(
      useMaterial3: false,
      brightness: brightness,
      extensions: <ThemeExtension<dynamic>>[p],
      primaryColor: p.primary,
      scaffoldBackgroundColor: p.surface,
      canvasColor: p.surface,
      cardColor: p.card,
      dividerColor: p.divider,
      filledButtonTheme: FilledButtonThemeData(style: _primaryFilledStyle(p)),
      elevatedButtonTheme: ElevatedButtonThemeData(style: _primaryElevatedStyle(p)),
      outlinedButtonTheme: OutlinedButtonThemeData(style: _outlinedSecondaryStyle(p)),
      appBarTheme: AppBarTheme(
        backgroundColor: p.surface,
        foregroundColor: p.onSurface,
        elevation: 0,
        centerTitle: false,
        iconTheme: IconThemeData(color: p.onSurface),
        titleTextStyle: TextStyle(
          color: p.onSurface,
          fontSize: AppTypography.screenTitleSize,
          fontWeight: AppTypography.screenTitleWeight,
        ),
      ),
      cardTheme: CardThemeData(
        color: p.card,
        elevation: 0,
        margin: const EdgeInsets.symmetric(horizontal: AppSpacing.lg, vertical: AppSpacing.xs),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(AppRadius.card)),
      ),
      inputDecorationTheme: InputDecorationTheme(
        filled: true,
        fillColor: p.surfaceVariant,
        contentPadding: const EdgeInsets.symmetric(horizontal: AppSpacing.inputH, vertical: AppSpacing.inputV),
        border: OutlineInputBorder(borderRadius: BorderRadius.circular(AppRadius.sm), borderSide: BorderSide.none),
        enabledBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(AppRadius.sm), borderSide: BorderSide(color: p.divider)),
        focusedBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(AppRadius.sm), borderSide: BorderSide(color: p.primary, width: 2)),
        hintStyle: TextStyle(color: p.onSurfaceVariant),
        labelStyle: TextStyle(color: p.onSurfaceVariant),
      ),
      snackBarTheme: SnackBarThemeData(
        behavior: SnackBarBehavior.floating,
        backgroundColor: p.card,
        contentTextStyle: TextStyle(
          color: p.onSurface,
          fontSize: AppTypography.bodySize,
          height: AppTypography.bodyHeight,
        ),
        elevation: 8,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(AppRadius.lg)),
        insetPadding: const EdgeInsets.fromLTRB(AppSpacing.lg, 0, AppSpacing.lg, AppSpacing.xl),
      ),
      dialogTheme: DialogThemeData(backgroundColor: p.card),
      colorScheme: isDark
          ? ColorScheme.dark(
              primary: p.primary,
              onPrimary: Colors.white,
              surface: p.surface,
              onSurface: p.onSurface,
              surfaceContainerHighest: p.card,
              error: p.error,
              onError: Colors.white,
            )
          : ColorScheme.light(
              primary: p.primary,
              onPrimary: Colors.white,
              surface: p.surface,
              onSurface: p.onSurface,
              surfaceContainerHighest: p.card,
              error: p.error,
              onError: Colors.white,
            ),
    );
  }

  static ThemeData get light => _build(AppPalette.light, Brightness.light);
  static ThemeData get dark => _build(AppPalette.dark, Brightness.dark);
}
