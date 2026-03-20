import 'dart:async';

import 'package:flutter/material.dart';

import '../ble/riftlink_ble.dart';
import '../ble/riftlink_ble_scope.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';

class DebugScreen extends StatefulWidget {
  const DebugScreen({super.key});

  @override
  State<DebugScreen> createState() => _DebugScreenState();
}

class _DebugScreenState extends State<DebugScreen> {
  StreamSubscription<RiftLinkEvent>? _sub;
  final List<String> _lines = <String>[];

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final ble = RiftLinkBleScope.of(context);
      _sub = ble.events.listen((evt) {
        if (!mounted) return;
        final t = DateTime.now().toIso8601String().substring(11, 19);
        setState(() {
          _lines.add('[$t] ${_formatEvent(evt)}');
          if (_lines.length > 200) {
            _lines.removeRange(0, _lines.length - 200);
          }
        });
      });
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }

  String _formatEvent(RiftLinkEvent evt) {
    if (evt is RiftLinkMsgEvent) return 'msg from=${evt.from} text=${evt.text}';
    if (evt is RiftLinkSentEvent) return 'sent to=${evt.to} msgId=${evt.msgId}';
    if (evt is RiftLinkReadEvent) return 'read from=${evt.from} msgId=${evt.msgId}';
    if (evt is RiftLinkDeliveredEvent) return 'delivered from=${evt.from} msgId=${evt.msgId}';
    if (evt is RiftLinkUndeliveredEvent) return 'undelivered to=${evt.to} msgId=${evt.msgId}';
    if (evt is RiftLinkInfoEvent) return 'info id=${evt.id} neighbors=${evt.neighbors.length} mode=${evt.radioMode}';
    if (evt is RiftLinkPongEvent) return 'pong from=${evt.from} rssi=${evt.rssi ?? 0}';
    if (evt is RiftLinkRoutesEvent) return 'routes count=${evt.routes.length}';
    if (evt is RiftLinkErrorEvent) return 'error code=${evt.code} msg=${evt.msg}';
    return evt.runtimeType.toString();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: context.palette.surface,
      appBar: AppBar(
        title: Text(context.l10n.tr('debug_log_title')),
        actions: [
          IconButton(
            tooltip: 'Clear',
            onPressed: () => setState(_lines.clear),
            icon: const Icon(Icons.delete_outline),
          ),
        ],
      ),
      body: _lines.isEmpty
          ? Center(
              child: Text(
                context.l10n.tr('debug_waiting_events'),
                style: TextStyle(color: context.palette.onSurfaceVariant),
              ),
            )
          : ListView.builder(
              padding: const EdgeInsets.fromLTRB(12, 8, 12, 16),
              itemCount: _lines.length,
              itemBuilder: (context, i) {
                final line = _lines[_lines.length - 1 - i];
                return Container(
                  margin: const EdgeInsets.only(bottom: 6),
                  padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
                  decoration: BoxDecoration(
                    color: context.palette.card,
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(color: context.palette.divider),
                  ),
                  child: Text(
                    line,
                    style: TextStyle(
                      color: context.palette.onSurface,
                      fontFamily: 'monospace',
                      fontSize: 12,
                    ),
                  ),
                );
              },
            ),
    );
  }
}
