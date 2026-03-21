import 'package:flutter_test/flutter_test.dart';
import 'package:riftlink_app/prefs/mesh_prefs.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  setUp(() async {
    SharedPreferences.setMockInitialValues({});
  });

  group('MeshPrefs voice acceptance', () {
    test('defaults are returned when prefs are empty', () async {
      final avg = await MeshPrefs.getVoiceAcceptMaxAvgLossPercent();
      final hard = await MeshPrefs.getVoiceAcceptMaxHardLossPercent();
      final minSessions = await MeshPrefs.getVoiceAcceptMinSessions();

      expect(avg, 20);
      expect(hard, 15);
      expect(minSessions, 5);
    });

    test('set/get persists values', () async {
      await MeshPrefs.setVoiceAcceptMaxAvgLossPercent(33);
      await MeshPrefs.setVoiceAcceptMaxHardLossPercent(21);
      await MeshPrefs.setVoiceAcceptMinSessions(9);

      final avg = await MeshPrefs.getVoiceAcceptMaxAvgLossPercent();
      final hard = await MeshPrefs.getVoiceAcceptMaxHardLossPercent();
      final minSessions = await MeshPrefs.getVoiceAcceptMinSessions();

      expect(avg, 33);
      expect(hard, 21);
      expect(minSessions, 9);
    });
  });
}
