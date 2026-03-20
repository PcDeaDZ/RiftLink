import 'package:flutter/material.dart';

/// Всплывающее меню сверху справа (как «три точки» в чате).
/// [toolbarHeight] — фактическая высота AppBar (в чате 44, на скане обычно [kToolbarHeight]).
class AppPopoverMenuRoute<T> extends PageRouteBuilder<T> {
  AppPopoverMenuRoute({
    required Widget child,
    this.toolbarHeight = kToolbarHeight,
  }) : super(
          opaque: false,
          barrierColor: Colors.black38,
          barrierDismissible: true,
          pageBuilder: (context, animation, secondaryAnimation) => child,
          transitionsBuilder: (context, animation, secondaryAnimation, widget) {
            final top = MediaQuery.paddingOf(context).top + toolbarHeight;
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
                    padding: EdgeInsets.only(top: top, right: 12),
                    child: FadeTransition(
                      opacity: CurvedAnimation(parent: animation, curve: Curves.easeOut),
                      child: ScaleTransition(
                        scale: Tween<double>(begin: 0.9, end: 1.0).animate(
                          CurvedAnimation(parent: animation, curve: Curves.easeOut),
                        ),
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
