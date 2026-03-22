import 'dart:io';

import 'package:flutter/services.dart';

class AppLifecycleBridge {
  static const MethodChannel _channel = MethodChannel('riftlink/app_lifecycle');

  static Future<bool> moveToBackground() async {
    if (!Platform.isAndroid) return false;
    try {
      final moved = await _channel.invokeMethod<bool>('moveToBackground');
      return moved ?? false;
    } catch (_) {
      return false;
    }
  }
}
