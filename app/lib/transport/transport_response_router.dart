import 'dart:async';

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
  }) {
    final ticket = sendTrackedRequest(
      cmd: cmd,
      payload: payload,
      expectedEvents: expectedEvents,
      timeout: timeout,
      retries: retries,
    );
    return ticket.response;
  }

  TransportRequestTicket sendTrackedRequest({
    required String cmd,
    Map<String, dynamic>? payload,
    required Set<String> expectedEvents,
    Duration timeout = const Duration(seconds: 4),
    int retries = 0,
  }) {
    final body = <String, dynamic>{...(payload ?? const <String, dynamic>{})};
    body['cmd'] = cmd;
    final raw = body['cmdId'];
    final cmdId = (raw is int && raw > 0) ? raw : (_commandIdFactory?.call() ?? nextCmdId());
    body['cmdId'] = cmdId;

    final completer = Completer<Map<String, dynamic>>();
    final req = _PendingRequest(
      cmd: cmd,
      cmdId: cmdId,
      expectedEvents: expectedEvents,
      payload: body,
      timeout: timeout,
      retriesLeft: retries,
      completer: completer,
      startedAt: DateTime.now(),
    );
    _pending[cmdId] = req;
    _trace?.call('stage=router action=request_sent cmd=$cmd cmdId=$cmdId retries=$retries');
    _sendPending(req);
    return TransportRequestTicket(cmdId: cmdId, response: completer.future);
  }

  void _sendPending(_PendingRequest req) {
    req.timer?.cancel();
    req.timer = Timer(req.timeout, () => _onTimeout(req.cmdId));
    unawaited(() async {
      final ok = await _sendCommand(req.payload);
      if (!ok) {
        req.timer?.cancel();
        if (_pending.remove(req.cmdId) != null && !req.completer.isCompleted) {
          req.completer.completeError(StateError('request_send_failed:${req.cmd}:${req.cmdId}'));
        }
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

  void _onResponse(Map<String, dynamic> json) {
    final cmdId = _readCmdId(json);
    if (cmdId == null || cmdId <= 0) return;
    final evt = json['evt']?.toString() ?? 'unknown';
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
    required this.completer,
    required this.startedAt,
  });
}
