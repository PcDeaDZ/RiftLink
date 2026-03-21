import 'package:flutter/material.dart';

// Согласовано с showAppDialog (app_navigator): тот же scrim и длительность overlay.
const Duration _kOverlayMotionDuration = Duration(milliseconds: 200);

/// Всплывающее меню сверху справа (как «три точки» в чате).
/// [toolbarHeight] — фактическая высота AppBar (в чате 44, на скане обычно [kToolbarHeight]).
class AppPopoverMenuRoute<T> extends PageRouteBuilder<T> {
  AppPopoverMenuRoute({
    required Widget child,
    this.toolbarHeight = kToolbarHeight,
  }) : super(
          opaque: false,
          barrierColor: Colors.black54,
          barrierDismissible: true,
          transitionDuration: _kOverlayMotionDuration,
          reverseTransitionDuration: _kOverlayMotionDuration,
          pageBuilder: (context, animation, secondaryAnimation) => child,
          transitionsBuilder: (context, animation, secondaryAnimation, widget) {
            final top = MediaQuery.paddingOf(context).top + toolbarHeight;
            final curved = CurvedAnimation(
              parent: animation,
              curve: Curves.easeOutCubic,
              reverseCurve: Curves.easeInCubic,
            );
            return Stack(
              children: [
                Positioned.fill(
                  child: GestureDetector(
                    onTap: () => Navigator.pop(context),
                    behavior: HitTestBehavior.opaque,
                    child: Container(color: Colors.transparent),
                  ),
                ),
                Align(
                  alignment: Alignment.topRight,
                  child: Padding(
                    padding: EdgeInsets.only(top: top, right: 16),
                    child: FadeTransition(
                      opacity: curved,
                      child: ScaleTransition(
                        scale: Tween<double>(begin: 0.94, end: 1.0).animate(curved),
                        alignment: Alignment.topRight,
                        child: widget,
                      ),
                    ),
                  ),
                ),
              ],
            );
          },
        );

  final double toolbarHeight;
}
