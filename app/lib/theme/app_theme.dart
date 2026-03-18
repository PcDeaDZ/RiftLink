import 'package:flutter/material.dart';

/// Цвета в стиле MeshCore — тёмная тема
class AppColors {
  static const primary = Color(0xFF42A5F5);
  static const surface = Color(0xFF121212);
  static const surfaceVariant = Color(0xFF1E1E1E);
  static const card = Color(0xFF2D2D2D);
  static const onSurface = Color(0xFFE0E0E0);
  static const onSurfaceVariant = Color(0xFFB0B0B0);
  static const divider = Color(0xFF3D3D3D);
  static const error = Color(0xFFCF6679);
  static const success = Color(0xFF66BB6A);
}

class AppTheme {
  static ThemeData get dark {
    return ThemeData(
      useMaterial3: false,
      brightness: Brightness.dark,
      primaryColor: AppColors.primary,
      scaffoldBackgroundColor: AppColors.surface,
      canvasColor: AppColors.surface,
      cardColor: AppColors.card,
      dividerColor: AppColors.divider,
      appBarTheme: const AppBarTheme(
        backgroundColor: AppColors.surface,
        foregroundColor: AppColors.onSurface,
        elevation: 0,
        centerTitle: false,
        iconTheme: IconThemeData(color: AppColors.onSurface),
        titleTextStyle: TextStyle(color: AppColors.onSurface, fontSize: 20, fontWeight: FontWeight.w600),
      ),
      cardTheme: CardThemeData(
        color: AppColors.card,
        elevation: 0,
        margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 4),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
      ),
      inputDecorationTheme: InputDecorationTheme(
        filled: true,
        fillColor: AppColors.surfaceVariant,
        contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
        border: OutlineInputBorder(borderRadius: BorderRadius.circular(8), borderSide: BorderSide.none),
        enabledBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(8), borderSide: const BorderSide(color: AppColors.divider)),
        focusedBorder: OutlineInputBorder(borderRadius: BorderRadius.circular(8), borderSide: const BorderSide(color: AppColors.primary, width: 2)),
        hintStyle: const TextStyle(color: AppColors.onSurfaceVariant),
        labelStyle: const TextStyle(color: AppColors.onSurfaceVariant),
      ),
      snackBarTheme: const SnackBarThemeData(
        behavior: SnackBarBehavior.floating,
        backgroundColor: AppColors.card,
        contentTextStyle: TextStyle(color: AppColors.onSurface),
      ),
      dialogTheme: const DialogThemeData(backgroundColor: AppColors.card),
      colorScheme: const ColorScheme.dark(
        primary: AppColors.primary,
        onPrimary: Colors.white,
        surface: AppColors.surface,
        onSurface: AppColors.onSurface,
        surfaceContainerHighest: AppColors.card,
        error: AppColors.error,
        onError: Colors.white,
      ),
    );
  }
}
