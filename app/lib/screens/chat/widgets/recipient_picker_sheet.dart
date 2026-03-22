import 'package:flutter/material.dart';

import '../../../app_navigator.dart';
import '../../../theme/app_theme.dart';
import '../../../theme/design_tokens.dart';

class RecipientPickerSheet extends StatelessWidget {
  const RecipientPickerSheet({super.key, required this.child});

  final Widget child;

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      top: false,
      child: Container(
        decoration: BoxDecoration(
          color: context.palette.card,
          borderRadius: const BorderRadius.vertical(
            top: Radius.circular(AppSpacing.xl),
          ),
        ),
        child: child,
      ),
    );
  }

  static Future<T?> show<T>({
    required BuildContext context,
    required WidgetBuilder builder,
  }) {
    return showAppModalBottomSheet<T>(
      context: context,
      isScrollControlled: true,
      backgroundColor: context.palette.card,
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(AppSpacing.xl)),
      ),
      builder: (ctx) => RecipientPickerSheet(child: builder(ctx)),
    );
  }
}
