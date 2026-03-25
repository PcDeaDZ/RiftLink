/// RiftLink — запись и воспроизведение голосовых сообщений (AAC на Android, Opus на iOS)
import 'dart:async';
import 'dart:io';
import 'package:flutter/foundation.dart';
import 'package:flutter_sound/flutter_sound.dart';
import 'package:path_provider/path_provider.dart';

import '../constants/voice_limits.dart';
import 'package:permission_handler/permission_handler.dart';

enum VoiceMeshProfile { fast, balanced, resilient }

class VoicePlaybackState {
  final bool isPlaying;
  final Duration position;
  final Duration duration;

  const VoicePlaybackState({
    this.isPlaying = false,
    this.position = Duration.zero,
    this.duration = Duration.zero,
  });

  double get progress => duration.inMilliseconds > 0
      ? (position.inMilliseconds / duration.inMilliseconds).clamp(0.0, 1.0)
      : 0.0;
}

class VoiceService {
  static final _recorder = FlutterSoundRecorder();
  static final _player = FlutterSoundPlayer();
  static bool _recorderInited = false;
  static bool _playerInited = false;
  static String? _recordPath;

  static final _playbackController = StreamController<VoicePlaybackState>.broadcast();
  static Stream<VoicePlaybackState> get playbackStream => _playbackController.stream;

  static Object? _currentPlaybackKey;
  static StreamSubscription? _progressSub;

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
  static Future<bool> startRecord({int bitRate = 64000}) async {
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
        bitRate: bitRate,
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

  /// Остановить запись и вернуть bytes (обрезает до maxBytes).
  static Future<List<int>?> stopRecord({int maxBytes = kMaxVoicePlainBytes}) async {
    try {
      await _recorder.stopRecorder();
      final path = _recordPath;
      _recordPath = null;
      if (path != null) {
        final file = File(path);
        if (await file.exists()) {
          var bytes = await file.readAsBytes();
          await file.delete();
          if (bytes.length > maxBytes) bytes = bytes.sublist(0, maxBytes);
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

  /// Оценка длительности по размеру и битрейту (грубая, без декодирования).
  /// bitRate: бит/с (по умолчанию 64000 для AAC), voiceProfileCode подсказывает.
  static Duration estimateDuration(List<int> bytes, {int? voiceProfileCode}) {
    if (bytes.isEmpty) return Duration.zero;
    int effectiveBitRate;
    switch (voiceProfileCode) {
      case 1:
        effectiveBitRate = 64000;
        break;
      case 2:
        effectiveBitRate = 32000;
        break;
      case 3:
        effectiveBitRate = 16000;
        break;
      default:
        effectiveBitRate = 64000;
    }
    final seconds = (bytes.length * 8) / effectiveBitRate;
    return Duration(milliseconds: (seconds * 1000).round());
  }

  /// Ключ текущего воспроизведения (hashCode voiceData). null если ничего не играет.
  static Object? get currentPlaybackKey => _currentPlaybackKey;

  /// Воспроизвести голос с прогрессом. key — уникальный идентификатор (например hashCode данных).
  static Future<void> playWithProgress(List<int> bytes, {Object? key, int? voiceProfileCode}) async {
    if (!await _initPlayer()) return;
    if (_player.isPlaying) {
      try { await _player.stopPlayer(); } catch (_) {}
    }
    _progressSub?.cancel();
    _currentPlaybackKey = key;
    _playbackController.add(const VoicePlaybackState());

    final detected = _detectCodec(bytes);
    final fallback = detected == Codec.opusOGG ? Codec.aacADTS : Codec.opusOGG;
    for (final codec in [detected, fallback]) {
      final ok = await _playWithCodecProgress(bytes, codec, voiceProfileCode: voiceProfileCode);
      if (ok) {
        // Дать UI время доанимировать полоску до 100% до сброса ключа (иначе бар обнуляется раньше конца).
        await Future<void>.delayed(const Duration(milliseconds: 340));
        _currentPlaybackKey = null;
        _playbackController.add(const VoicePlaybackState());
        return;
      }
    }
    _currentPlaybackKey = null;
    _playbackController.add(const VoicePlaybackState());
  }

  static Future<bool> _playWithCodecProgress(List<int> bytes, Codec codec, {int? voiceProfileCode}) async {
    final ext = codec == Codec.opusOGG ? 'ogg' : 'aac';
    final dir = await getTemporaryDirectory();
    final path = '${dir.path}/riftlink_play_${DateTime.now().millisecondsSinceEpoch}.$ext';
    final file = File(path);
    await file.writeAsBytes(bytes);
    try {
      final done = Completer<void>();
      final estimated = estimateDuration(bytes, voiceProfileCode: voiceProfileCode);
      var progressEventIndex = 0;
      var lastReportedDur = estimated > Duration.zero ? estimated : Duration.zero;

      await _player.setSubscriptionDuration(const Duration(milliseconds: 100));
      _progressSub = _player.onProgress?.listen((e) {
        progressEventIndex++;
        final dur = e.duration > Duration.zero ? e.duration : estimated;
        if (dur > Duration.zero) lastReportedDur = dur;
        var pos = e.position;
        if (pos < Duration.zero) pos = Duration.zero;
        if (dur > Duration.zero) {
          if (pos > dur) pos = dur;
          // Артефакт декодера: первые кадры «уже в конце». Не шлём в UI — иначе рывок вперёд/назад.
          if (progressEventIndex <= 3 &&
              dur.inMilliseconds > 200 &&
              pos.inMilliseconds >= dur.inMilliseconds - 100) {
            return;
          }
        }
        _playbackController.add(VoicePlaybackState(
          isPlaying: true,
          position: pos,
          duration: dur,
        ));
      });

      // Сначала фиксируем 0 до старта декодера — иначе onProgress может опередить и дать «вперёд→назад».
      _playbackController.add(VoicePlaybackState(
        isPlaying: true,
        position: Duration.zero,
        duration: estimated > Duration.zero ? estimated : Duration.zero,
      ));

      await _player.startPlayer(
        fromURI: path,
        codec: codec,
        whenFinished: () {
          _progressSub?.cancel();
          _progressSub = null;
          if (!done.isCompleted) done.complete();
        },
      );
      await done.future.timeout(const Duration(seconds: 30));
      // Финальный кадр 100% — иначе UI часто останавливается на ~0.9 до сброса ключа.
      final d = lastReportedDur > Duration.zero
          ? lastReportedDur
          : (estimated > Duration.zero ? estimated : const Duration(milliseconds: 1));
      _playbackController.add(VoicePlaybackState(
        isPlaying: true,
        position: d,
        duration: d,
      ));
      return true;
    } catch (_) {
      return false;
    } finally {
      _progressSub?.cancel();
      _progressSub = null;
      // Ключ и UI сбрасывает только playWithProgress после успеха или после обеих неудачных попыток.
      // Раньше здесь обнуляли ключ — вторая попытка кодека шла без currentPlaybackKey (артефакты UI / «чужое» воспроизведение).
      try {
        await _player.stopPlayer();
      } catch (_) {}
      try {
        if (await file.exists()) await file.delete();
      } catch (_) {}
    }
  }

  /// Воспроизвести голос (автоопределение AAC/Opus по заголовку) — простой вариант без прогресса
  static Future<void> play(List<int> bytes) async {
    if (!await _initPlayer()) return;
    if (_player.isPlaying) {
      try {
        await _player.stopPlayer();
      } catch (_) {}
    }
    final detected = _detectCodec(bytes);
    final fallback = detected == Codec.opusOGG ? Codec.aacADTS : Codec.opusOGG;
    final tried = <Codec>[detected, fallback];
    for (final codec in tried) {
      final ok = await _playWithCodec(bytes, codec);
      if (ok) return;
    }
  }

  static Future<bool> _playWithCodec(List<int> bytes, Codec codec) async {
    final ext = codec == Codec.opusOGG ? 'ogg' : 'aac';
    final dir = await getTemporaryDirectory();
    final path = '${dir.path}/riftlink_play_${DateTime.now().millisecondsSinceEpoch}.$ext';
    final file = File(path);
    await file.writeAsBytes(bytes);
    try {
      final done = Completer<void>();
      await _player.startPlayer(
        fromURI: path,
        codec: codec,
        whenFinished: () {
          if (!done.isCompleted) done.complete();
        },
      );
      await done.future.timeout(const Duration(seconds: 20));
      return true;
    } catch (_) {
      return false;
    } finally {
      if (await file.exists()) await file.delete();
    }
  }

  static Future<void> stopPlay() async {
    try {
      _progressSub?.cancel();
      _progressSub = null;
      _currentPlaybackKey = null;
      _playbackController.add(const VoicePlaybackState());
      await _player.stopPlayer();
    } catch (_) {}
  }

  static bool get isPlaying => _player.isPlaying;
}
