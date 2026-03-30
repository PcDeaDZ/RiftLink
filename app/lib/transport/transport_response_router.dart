import 'dart:async';

/// Ошибка групповой безопасности с узла (`evt: groupSecurityError`), с тем же `cmdId`, что и tracked-команда.
class GroupSecurityResponseError implements Exception {
  GroupSecurityResponseError(this.cmd, this.cmdId, this.code, this.msg);
  final String cmd;
  final int cmdId;
  final String code;
  final String msg;

  @override
  String toString() => 'GroupSecurityResponseError($cmd, cmdId=$cmdId, $code: $msg)';
}

/// Сопоставление ответов по полю `cmdId` в JSON (> 0). Прошивка обязана повторять `cmdId` из запроса.
typedef RouterTrace = void Function(String message);
typedef RouterSendCommand = Future<bool> Function(Map<String, dynamic> payload);
typedef RouterCommandIdFactory = int Function();

class TransportRequestTicket {
  final int cmdId;
  final Future<Map<String, dynamic>> response;

  const TransportRequestTicket({
    required this.cmdId,
    required this.response,
  });
}

class TransportResponseRouter {
  final RouterSendCommand _sendCommand;
  final RouterCommandIdFactory? _commandIdFactory;
  final RouterTrace? _trace;
  final Map<int, _PendingRequest> _pending = <int, _PendingRequest>{};
  final Map<int, DateTime> _recentlyCompleted = <int, DateTime>{};
  final StreamSubscription<Map<String, dynamic>> _rxSub;
  int _nextCmdId = 1;

  TransportResponseRouter({
    required RouterSendCommand sendCommand,
    required Stream<Map<String, dynamic>> responses,
    RouterCommandIdFactory? commandIdFactory,
    RouterTrace? trace,
  })  : _sendCommand = sendCommand,
        _commandIdFactory = commandIdFactory,
        _trace = trace,
        _rxSub = responses.listen((_) {}) {
    _rxSub.onData(_onResponse);
  }

  int nextCmdId() {
    final id = _nextCmdId;
    _nextCmdId = (id >= 0x7FFFFFFF) ? 1 : (id + 1);
    return id;
  }

  Future<Map<String, dynamic>> sendRequest({
    required String cmd,
    Map<String, dynamic>? payload,
    required Set<String> expectedEvents,
    Duration timeout = const Duration(seconds: 4),
    int retries = 0,
    int sendAttempts = 1,
  }) {
    final ticket = sendTrackedRequest(
      cmd: cmd,
      payload: payload,
      expectedEvents: expectedEvents,
      timeout: timeout,
      retries: retries,
      sendAttempts: sendAttempts,
    );
    return ticket.response;
  }

  TransportRequestTicket sendTrackedRequest({
    required String cmd,
    Map<String, dynamic>? payload,
    required Set<String> expectedEvents,
    Duration timeout = const Duration(seconds: 4),
    int retries = 0,
    int sendAttempts = 1,
  }) {
    // Прошивка: один s_pendingInfoCmdId — волна node/neighbors отдаётся только с последним cmd:info.
    // Иначе старые tracked info висят до таймаута, пока по эфиру идут evt с новым cmdId.
    if (cmd == 'info') {
      _supersedePendingInfoRequests();
    }
    final body = <String, dynamic>{...(payload ?? const <String, dynamic>{})};
    body['cmd'] = cmd;
    final raw = body['cmdId'];
    // Не только int: при merge/JSON иногда num (double); иначе генерировался новый id и ломался матч с узлом.
    final int cmdId;
    if (raw is int && raw > 0) {
      cmdId = raw;
    } else if (raw is num && raw > 0) {
      cmdId = raw.round();
    } else {
      cmdId = _commandIdFactory?.call() ?? nextCmdId();
    }
    body['cmdId'] = cmdId;

    final completer = Completer<Map<String, dynamic>>();
    final req = _PendingRequest(
      cmd: cmd,
      cmdId: cmdId,
      expectedEvents: expectedEvents,
      payload: body,
      timeout: timeout,
      retriesLeft: retries,
      sendAttempts: sendAttempts < 1 ? 1 : sendAttempts,
      completer: completer,
      startedAt: DateTime.now(),
    );
    _pending[cmdId] = req;
    _trace?.call(
        'stage=router action=request_sent cmd=$cmd cmdId=$cmdId retries=$retries send_attempts=${req.sendAttempts}');
    _sendPending(req);
    return TransportRequestTicket(cmdId: cmdId, response: completer.future);
  }

  /// Снять ожидание старых `cmd:info`: на узле остаётся один актуальный cmdId на волну.
  void _supersedePendingInfoRequests() {
    final staleIds = <int>[];
    for (final e in _pending.entries) {
      if (e.value.cmd == 'info') {
        staleIds.add(e.key);
      }
    }
    for (final id in staleIds) {
      final req = _pending.remove(id);
      req?.timer?.cancel();
      if (req != null && !req.completer.isCompleted) {
        _trace?.call('stage=router action=request_superseded cmd=info cmdId=${req.cmdId} reason=new_info_request');
        req.completer.completeError(StateError('info_superseded:${req.cmdId}'));
      }
    }
  }

  void _sendPending(_PendingRequest req) {
    req.timer?.cancel();
    req.timer = Timer(req.timeout, () => _onTimeout(req.cmdId));
    unawaited(() async {
      var left = req.sendAttempts;
      while (left > 0) {
        left--;
        final ok = await _sendCommand(req.payload);
        if (ok) return;
        if (left > 0) {
          _trace?.call(
              'stage=router action=send_retry cmd=${req.cmd} cmdId=${req.cmdId} attempts_left=$left');
          await Future<void>.delayed(const Duration(milliseconds: 90));
        }
      }
      req.timer?.cancel();
      if (_pending.remove(req.cmdId) != null && !req.completer.isCompleted) {
        req.completer.completeError(StateError('request_send_failed:${req.cmd}:${req.cmdId}'));
      }
    }());
  }

  void _onTimeout(int cmdId) {
    final req = _pending[cmdId];
    if (req == null) return;
    if (req.retriesLeft > 0) {
      req.retriesLeft--;
      _trace?.call('stage=router action=request_retry cmd=${req.cmd} cmdId=$cmdId retries_left=${req.retriesLeft}');
      _sendPending(req);
      return;
    }
    _pending.remove(cmdId);
    if (!req.completer.isCompleted) {
      _trace?.call('stage=router action=response_timeout cmd=${req.cmd} cmdId=$cmdId');
      req.completer.completeError(TimeoutException('response_timeout:${req.cmd}:$cmdId', req.timeout));
    }
  }

  static const Set<String> _kInfoWaveEvts = {'node', 'neighbors', 'routes', 'groups'};
  /// Предел «скользящего» продления таймера по частям волны (после — только обработка evt, без нового Timer).
  static const Duration _kInfoWaveSlidingCap = Duration(seconds: 35);

  void _onResponse(Map<String, dynamic> json) {
    final evt = json['evt']?.toString() ?? 'unknown';
    final cmdId = _readCmdId(json);
    if (cmdId == null || cmdId <= 0) return;
    final req = _pending[cmdId];
    if (req == null) {
      final completedAt = _recentlyCompleted[cmdId];
      if (completedAt != null && DateTime.now().difference(completedAt) <= const Duration(seconds: 30)) {
        _trace?.call('stage=router action=response_late cmdId=$cmdId evt=$evt');
      } else {
        _trace?.call('stage=router action=response_orphan cmdId=$cmdId evt=$evt');
      }
      return;
    }
    // Волна cmd:info — несколько notify подряд; node может прийти после routes/groups (очередь BLE / main loop).
    // Продлеваем окно ожидания «якоря» (node|neighbors в expectedEvents), пока идут части одной волны.
    if (req.cmd == 'info' && _kInfoWaveEvts.contains(evt)) {
      final sinceStart = DateTime.now().difference(req.startedAt);
      final anchor = evt == 'node' || evt == 'neighbors';
      if (anchor || sinceStart < _kInfoWaveSlidingCap) {
        req.timer?.cancel();
        req.timer = Timer(req.timeout, () => _onTimeout(req.cmdId));
        _trace?.call(
          'stage=router action=info_wave_timer_reset cmdId=$cmdId evt=$evt since_start_ms=${sinceStart.inMilliseconds}',
        );
      }
    }
    _dispatchMatchedRequest(req, json, evt, cmdId);
  }

  void _dispatchMatchedRequest(_PendingRequest req, Map<String, dynamic> json, String evt, int cmdId) {
    if (evt == 'groupSecurityError') {
      _pending.remove(cmdId);
      req.timer?.cancel();
      _recentlyCompleted[cmdId] = DateTime.now();
      _pruneCompleted();
      final elapsed = DateTime.now().difference(req.startedAt).inMilliseconds;
      _trace?.call(
        'stage=router action=response_group_security_error cmd=${req.cmd} cmdId=$cmdId elapsed_ms=$elapsed',
      );
      if (!req.completer.isCompleted) {
        req.completer.completeError(
          GroupSecurityResponseError(
            req.cmd,
            cmdId,
            json['code']?.toString() ?? 'unknown',
            json['msg']?.toString() ?? '',
          ),
        );
      }
      return;
    }
    final isError = evt == 'error';
    final matched = isError || req.expectedEvents.contains(evt);
    if (!matched) return;

    _pending.remove(cmdId);
    req.timer?.cancel();
    _recentlyCompleted[cmdId] = DateTime.now();
    _pruneCompleted();
    final elapsed = DateTime.now().difference(req.startedAt).inMilliseconds;
    if (isError) {
      _trace?.call('stage=router action=response_error cmd=${req.cmd} cmdId=$cmdId evt=$evt elapsed_ms=$elapsed');
      if (!req.completer.isCompleted) {
        req.completer.completeError(StateError('response_error:${req.cmd}:$cmdId:${json['code'] ?? 'unknown'}'));
      }
      return;
    }
    _trace?.call('stage=router action=response_matched cmd=${req.cmd} cmdId=$cmdId evt=$evt elapsed_ms=$elapsed');
    if (!req.completer.isCompleted) req.completer.complete(json);
  }

  int? _readCmdId(Map<String, dynamic> json) {
    final raw = json['cmdId'];
    if (raw is int) return raw;
    if (raw is num) return raw.toInt();
    if (raw == null) return null;
    return int.tryParse(raw.toString());
  }

  void _pruneCompleted() {
    final now = DateTime.now();
    final stale = <int>[];
    for (final e in _recentlyCompleted.entries) {
      if (now.difference(e.value) > const Duration(seconds: 30)) stale.add(e.key);
    }
    for (final key in stale) {
      _recentlyCompleted.remove(key);
    }
  }

  void cancelAll() {
    for (final req in _pending.values) {
      req.timer?.cancel();
      if (!req.completer.isCompleted) {
        _trace?.call('stage=router action=cancel_all cmd=${req.cmd} cmdId=${req.cmdId}');
        req.completer.completeError(StateError('router_cancelled:${req.cmd}:${req.cmdId}'));
      }
    }
    _pending.clear();
    _recentlyCompleted.clear();
  }

  Future<void> dispose() async {
    await _rxSub.cancel();
    cancelAll();
  }
}

class _PendingRequest {
  final String cmd;
  final int cmdId;
  final Set<String> expectedEvents;
  final Map<String, dynamic> payload;
  final Duration timeout;
  int retriesLeft;
  /// Сколько раз подряд вызывать [_sendCommand] при ошибке BLE/WiFi записи (до ожидания ответа).
  final int sendAttempts;
  final Completer<Map<String, dynamic>> completer;
  final DateTime startedAt;
  Timer? timer;

  _PendingRequest({
    required this.cmd,
    required this.cmdId,
    required this.expectedEvents,
    required this.payload,
    required this.timeout,
    required this.retriesLeft,
    required this.sendAttempts,
    required this.completer,
    required this.startedAt,
  });
}
