import '../screens/chat/chat_ui_models.dart';

class VoiceAssemblyService {
  VoiceAssemblyService({this.inactiveTimeout = const Duration(seconds: 20), this.maxAge = const Duration(seconds: 45)});

  final Duration inactiveTimeout;
  final Duration maxAge;

  List<String> collectExpiredSenders(Map<String, ChatUiVoiceRxAssembly> assemblies, DateTime now) {
    if (assemblies.isEmpty) return const [];
    final expired = <String>[];
    assemblies.forEach((from, assembly) {
      final inactiveSec = now.difference(assembly.lastUpdated).inSeconds;
      final ageSec = now.difference(assembly.startedAt).inSeconds;
      if (inactiveSec > inactiveTimeout.inSeconds || ageSec > maxAge.inSeconds) {
        expired.add(from);
      }
    });
    return expired;
  }

  ChatUiDecodedVoicePayload decodePayload(List<int> rawBytes) {
    var bytes = rawBytes;
    int? voiceProfileCode;
    if (bytes.length >= 2 && bytes[0] == 0xFE && bytes[1] >= 1 && bytes[1] <= 3) {
      voiceProfileCode = bytes[1];
      bytes = bytes.sublist(2);
    }
    DateTime? deleteAt;
    if (bytes.length >= 2 && bytes[0] == 0xFF && bytes[1] >= 1 && bytes[1] <= 255) {
      final ttl = bytes[1];
      bytes = bytes.sublist(2);
      deleteAt = DateTime.now().add(Duration(minutes: ttl));
    }
    return ChatUiDecodedVoicePayload(
      bytes: bytes,
      voiceProfileCode: voiceProfileCode,
      deleteAt: deleteAt,
    );
  }
}
