/// RiftLink — запись и воспроизведение голосовых сообщений (AAC на Android, Opus на iOS)
import 'dart:async';
import 'dart:io';
import 'package:flutter/foundation.dart';
import 'package:flutter_sound/flutter_sound.dart';
import 'package:path_provider/path_provider.dart';
import 'package:permission_handler/permission_handler.dart';

class VoiceService {
  static final _recorder = FlutterSoundRecorder();
  static final _player = FlutterSoundPlayer();
  static bool _recorderInited = false;
  static bool _playerInited = false;
  static String? _recordPath;

  /// AAC поддерживается на всех Android; Opus — на iOS
  static Codec get _recordCodec => defaultTargetPlatform == TargetPlatform.android ? Codec.aacADTS : Codec.opusOGG;
  static String get _recordExt => defaultTargetPlatform == TargetPlatform.android ? 'aac' : 'ogg';

  static Future<bool> _initRecorder() async {
    if (_recorderInited) return true;
    try {
      await _recorder.openRecorder();
      _recorderInited = true;
    } catch (_) {}
    return _recorderInited;
  }

  static Future<bool> _initPlayer() async {
    if (_playerInited) return true;
    try {
      await _player.openPlayer();
      _playerInited = true;
    } catch (_) {}
    return _playerInited;
  }

  static Future<bool> requestPermission() async {
    final status = await Permission.microphone.request();
    return status.isGranted;
  }

  /// Начать запись. Вызвать stopRecord() для завершения.
  static Future<bool> startRecord() async {
    if (!await requestPermission()) return false;
    if (!await _initRecorder()) return false;

    final dir = await getTemporaryDirectory();
    _recordPath = '${dir.path}/riftlink_voice_${DateTime.now().millisecondsSinceEpoch}.$_recordExt';

    try {
      await _recorder.startRecorder(
        toFile: _recordPath!,
        codec: _recordCodec,
        sampleRate: 16000,
        numChannels: 1,
        bitRate: 64000,
      );
      return true;
    } catch (e) {
      rethrow;
    }
  }

  /// Отменить запись (без сохранения)
  static Future<void> cancelRecord() async {
    try {
      await _recorder.stopRecorder();
      final path = _recordPath;
      _recordPath = null;
      if (path != null) {
        final file = File(path);
        if (await file.exists()) await file.delete();
      }
    } catch (_) {}
  }

  /// Остановить запись и вернуть bytes (макс. 30 KB)
  static Future<List<int>?> stopRecord() async {
    try {
      await _recorder.stopRecorder();
      final path = _recordPath;
      _recordPath = null;
      if (path != null) {
        final file = File(path);
        if (await file.exists()) {
          var bytes = await file.readAsBytes();
          await file.delete();
          if (bytes.length > 30720) bytes = bytes.sublist(0, 30720);
          return bytes;
        }
      }
    } catch (_) {}
    return null;
  }

  static bool get isRecording => _recorder.isRecording;

  /// Определить кодек по заголовку (AAC ADTS: 0xFF 0xFx, OGG/Opus: OggS)
  static Codec _detectCodec(List<int> bytes) {
    if (bytes.length >= 4) {
      if (bytes[0] == 0x4F && bytes[1] == 0x67 && bytes[2] == 0x67 && bytes[3] == 0x53) {
        return Codec.opusOGG; // OggS
      }
      if (bytes.length >= 2 && bytes[0] == 0xFF && (bytes[1] & 0xF0) == 0xF0) {
        return Codec.aacADTS; // AAC ADTS sync
      }
    }
    return _recordCodec; // fallback
  }

  /// Воспроизвести голос (автоопределение AAC/Opus по заголовку)
  static Future<void> play(List<int> bytes) async {
    if (!await _initPlayer()) return;

    final codec = _detectCodec(bytes);
    final ext = codec == Codec.opusOGG ? 'ogg' : 'aac';
    final dir = await getTemporaryDirectory();
    final path = '${dir.path}/riftlink_play_${DateTime.now().millisecondsSinceEpoch}.$ext';
    final file = File(path);
    await file.writeAsBytes(bytes);

    try {
      final done = Completer<void>();
      await _player.startPlayer(fromURI: path, codec: codec, whenFinished: () => done.complete());
      await done.future;
    } catch (_) {}
    if (await file.exists()) await file.delete();
  }

  static Future<void> stopPlay() async {
    try {
      await _player.stopPlayer();
    } catch (_) {}
  }

  static bool get isPlaying => _player.isPlaying;
}
