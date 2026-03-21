/// Настройки фона mesh (анимация)

import 'package:shared_preferences/shared_preferences.dart';

const _keyMeshAnimation = 'riftlink_mesh_animation';
const _keyVoiceAcceptMaxAvgLossPercent = 'riftlink_voice_accept_max_avg_loss_percent';
const _keyVoiceAcceptMaxHardLossPercent = 'riftlink_voice_accept_max_hard_loss_percent';
const _keyVoiceAcceptMinSessions = 'riftlink_voice_accept_min_sessions';

class MeshPrefs {
  static Future<bool> getAnimationEnabled() async {
    final prefs = await SharedPreferences.getInstance();
    return prefs.getBool(_keyMeshAnimation) ?? true;
  }

  static Future<void> setAnimationEnabled(bool enabled) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(_keyMeshAnimation, enabled);
  }

  static Future<int> getVoiceAcceptMaxAvgLossPercent() async {
    final prefs = await SharedPreferences.getInstance();
    return prefs.getInt(_keyVoiceAcceptMaxAvgLossPercent) ?? 20;
  }

  static Future<void> setVoiceAcceptMaxAvgLossPercent(int value) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setInt(_keyVoiceAcceptMaxAvgLossPercent, value);
  }

  static Future<int> getVoiceAcceptMaxHardLossPercent() async {
    final prefs = await SharedPreferences.getInstance();
    return prefs.getInt(_keyVoiceAcceptMaxHardLossPercent) ?? 15;
  }

  static Future<void> setVoiceAcceptMaxHardLossPercent(int value) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setInt(_keyVoiceAcceptMaxHardLossPercent, value);
  }

  static Future<int> getVoiceAcceptMinSessions() async {
    final prefs = await SharedPreferences.getInstance();
    return prefs.getInt(_keyVoiceAcceptMinSessions) ?? 5;
  }

  static Future<void> setVoiceAcceptMinSessions(int value) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setInt(_keyVoiceAcceptMinSessions, value);
  }
}
