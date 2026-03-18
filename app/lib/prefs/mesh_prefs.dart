/// Настройки фона mesh (анимация)

import 'package:shared_preferences/shared_preferences.dart';

const _keyMeshAnimation = 'riftlink_mesh_animation';

class MeshPrefs {
  static Future<bool> getAnimationEnabled() async {
    final prefs = await SharedPreferences.getInstance();
    return prefs.getBool(_keyMeshAnimation) ?? true;
  }

  static Future<void> setAnimationEnabled(bool enabled) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(_keyMeshAnimation, enabled);
  }
}
