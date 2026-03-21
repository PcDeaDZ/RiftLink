import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:file_picker/file_picker.dart';
import 'package:crypto/crypto.dart' as crypto_lib;

import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../widgets/mesh_background.dart';

enum _OtaState { idle, picking, starting, uploading, verifying, done, error }

class OtaScreen extends StatefulWidget {
  final RiftLinkBle ble;
  const OtaScreen({super.key, required this.ble});

  @override
  State<OtaScreen> createState() => _OtaScreenState();
}

class _OtaScreenState extends State<OtaScreen> {
  _OtaState _state = _OtaState.idle;
  String? _fileName;
  int _fileSize = 0;
  int _bytesWritten = 0;
  int _chunkSize = 509;
  String? _errorMsg;
  StreamSubscription? _evtSub;
  Timer? _startTimeout;

  @override
  void initState() {
    super.initState();
    _evtSub = widget.ble.events.listen(_onEvent);
  }

  @override
  void dispose() {
    _evtSub?.cancel();
    _startTimeout?.cancel();
    super.dispose();
  }

  void _onEvent(RiftLinkEvent evt) {
    if (!mounted) return;
    if (evt is RiftLinkBleOtaReadyEvent) {
      _startTimeout?.cancel();
      setState(() {
        _chunkSize = evt.chunkSize;
        _state = _OtaState.uploading;
      });
      _doUpload();
    } else if (evt is RiftLinkBleOtaProgressEvent) {
      setState(() => _bytesWritten = evt.written);
    } else if (evt is RiftLinkBleOtaResultEvent) {
      setState(() {
        if (evt.ok) {
          _state = _OtaState.done;
        } else {
          _state = _OtaState.error;
          _errorMsg = evt.reason ?? context.l10n.tr('ota_error_title');
        }
      });
    }
  }

  List<int>? _fileBytes;

  Future<void> _pickAndStart() async {
    setState(() => _state = _OtaState.picking);

    final result = await FilePicker.platform.pickFiles(
      type: FileType.any,
      withData: true,
    );

    if (result == null || result.files.isEmpty) {
      setState(() => _state = _OtaState.idle);
      return;
    }

    final file = result.files.first;
    _fileBytes = file.bytes?.toList();

    if (_fileBytes == null && file.path != null) {
      _fileBytes = await File(file.path!).readAsBytes();
    }

    if (_fileBytes == null || _fileBytes!.isEmpty) {
      setState(() {
        _state = _OtaState.error;
        _errorMsg = context.l10n.tr('ota_file_read_error');
      });
      return;
    }

    _fileName = file.name;
    _fileSize = _fileBytes!.length;
    _bytesWritten = 0;

    // Compute MD5
    final md5 = crypto_lib.md5.convert(_fileBytes!).toString();

    setState(() => _state = _OtaState.starting);
    _startTimeout?.cancel();
    _startTimeout = Timer(const Duration(seconds: 8), () {
      if (!mounted || _state != _OtaState.starting) return;
      setState(() {
        _state = _OtaState.error;
        _errorMsg = context.l10n.tr('ota_start_timeout');
      });
    });
    final started = await widget.ble.startBleOta(size: _fileSize, md5: md5);
    if (!mounted) return;
    if (!started) {
      _startTimeout?.cancel();
      setState(() {
        _state = _OtaState.error;
        _errorMsg = context.l10n.tr('ota_start_failed');
      });
    }
  }

  Future<void> _doUpload() async {
    if (_fileBytes == null) return;

    int offset = 0;
    while (offset < _fileBytes!.length) {
      if (!mounted || _state != _OtaState.uploading) return;

      final end = (offset + _chunkSize).clamp(0, _fileBytes!.length);
      final chunk = _fileBytes!.sublist(offset, end);

      final ok = await widget.ble.sendBleOtaChunk(chunk);
      if (!ok) {
        if (!mounted) return;
        setState(() {
          _state = _OtaState.error;
          _errorMsg = context.l10n.tr('ota_chunk_send_error', {'offset': '$offset'});
        });
        return;
      }

      offset = end;
      if (mounted) setState(() => _bytesWritten = offset);

      // Small pacing between chunks keeps upload stable across BLE/Wi-Fi.
      await Future<void>.delayed(const Duration(milliseconds: 10));
    }

    if (!mounted) return;
    setState(() => _state = _OtaState.verifying);
    await widget.ble.endBleOta();
  }

  Future<void> _abort() async {
    _startTimeout?.cancel();
    await widget.ble.abortBleOta();
    if (!mounted) return;
    setState(() {
      _state = _OtaState.idle;
      _fileBytes = null;
      _bytesWritten = 0;
    });
  }

  double get _progress => _fileSize > 0 ? _bytesWritten / _fileSize : 0;

  @override
  Widget build(BuildContext context) {
    final palette = context.palette;
    final l = context.l10n;
    return Scaffold(
      backgroundColor: palette.surface,
      appBar: AppBar(
        title: Text(l.tr('firmware_update_title')),
        backgroundColor: palette.surface,
        foregroundColor: palette.onSurface,
        elevation: 0,
      ),
      body: MeshBackgroundWrapper(
        child: Center(
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: _buildBody(palette, l),
          ),
        ),
      ),
    );
  }

  Widget _buildBody(AppPalette palette, AppLocalizations l) {
    switch (_state) {
      case _OtaState.idle:
        return Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.system_update_alt, size: 64, color: palette.primary),
            const SizedBox(height: 16),
            Text(
              l.tr('firmware_update_title'),
              style: TextStyle(fontSize: 20, fontWeight: FontWeight.w600, color: palette.onSurface),
            ),
            const SizedBox(height: 8),
            Text(
              l.tr('ota_ble_intro_desc'),
              style: TextStyle(color: palette.onSurfaceVariant),
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 32),
            FilledButton.icon(
              onPressed: _pickAndStart,
              icon: const Icon(Icons.file_open),
              label: Text(l.tr('ota_select_firmware')),
            ),
          ],
        );

      case _OtaState.picking:
        return const CircularProgressIndicator();

      case _OtaState.starting:
        return Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const CircularProgressIndicator(),
            const SizedBox(height: 16),
            Text(l.tr('ota_state_starting'), style: TextStyle(color: palette.onSurface)),
            if (_fileName != null) ...[
              const SizedBox(height: 8),
              Text(
                '$_fileName (${(_fileSize / 1024).toStringAsFixed(0)} KB)',
                style: TextStyle(color: palette.onSurfaceVariant, fontSize: 13),
              ),
            ],
          ],
        );

      case _OtaState.uploading:
        return Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            SizedBox(
              width: 120, height: 120,
              child: Stack(
                alignment: Alignment.center,
                children: [
                  SizedBox(
                    width: 120, height: 120,
                    child: CircularProgressIndicator(
                      value: _progress,
                      strokeWidth: 6,
                      backgroundColor: palette.divider,
                      color: palette.primary,
                    ),
                  ),
                  Text(
                    '${(_progress * 100).toStringAsFixed(0)}%',
                    style: TextStyle(
                      fontSize: 24, fontWeight: FontWeight.bold,
                      color: palette.onSurface,
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 20),
            Text(
              l.tr('ota_state_uploading'),
              style: TextStyle(fontSize: 16, color: palette.onSurface),
            ),
            const SizedBox(height: 4),
            Text(
              '${(_bytesWritten / 1024).toStringAsFixed(0)} / ${(_fileSize / 1024).toStringAsFixed(0)} KB',
              style: TextStyle(color: palette.onSurfaceVariant, fontSize: 13),
            ),
            const SizedBox(height: 24),
            OutlinedButton(
              onPressed: _abort,
              child: Text(l.tr('ota_cancel')),
            ),
          ],
        );

      case _OtaState.verifying:
        return Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const CircularProgressIndicator(),
            const SizedBox(height: 16),
            Text(l.tr('ota_state_verifying'), style: TextStyle(color: palette.onSurface)),
          ],
        );

      case _OtaState.done:
        return Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.check_circle, size: 64, color: palette.success),
            const SizedBox(height: 16),
            Text(
              l.tr('ota_done_title'),
              style: TextStyle(fontSize: 20, fontWeight: FontWeight.w600, color: palette.success),
            ),
            const SizedBox(height: 8),
            Text(
              l.tr('ota_done_desc'),
              style: TextStyle(color: palette.onSurfaceVariant),
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 24),
            FilledButton(
              onPressed: () => Navigator.of(context).pop(),
              child: Text(l.tr('ota_done_button')),
            ),
          ],
        );

      case _OtaState.error:
        return Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.error_outline, size: 64, color: palette.error),
            const SizedBox(height: 16),
            Text(
              l.tr('ota_error_title'),
              style: TextStyle(fontSize: 20, fontWeight: FontWeight.w600, color: palette.error),
            ),
            const SizedBox(height: 8),
            Text(
              _errorMsg ?? l.tr('ota_unknown_error'),
              style: TextStyle(color: palette.onSurfaceVariant),
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 24),
            FilledButton(
              onPressed: () {
                setState(() {
                  _state = _OtaState.idle;
                  _fileBytes = null;
                  _bytesWritten = 0;
                  _errorMsg = null;
                });
              },
              child: Text(l.tr('ota_try_again')),
            ),
          ],
        );
    }
  }
}
