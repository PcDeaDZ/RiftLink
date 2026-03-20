import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:file_picker/file_picker.dart';
import 'package:crypto/crypto.dart' as crypto_lib;

import '../ble/riftlink_ble.dart';
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

  @override
  void initState() {
    super.initState();
    _evtSub = widget.ble.events.listen(_onEvent);
  }

  @override
  void dispose() {
    _evtSub?.cancel();
    super.dispose();
  }

  void _onEvent(RiftLinkEvent evt) {
    if (evt is RiftLinkBleOtaReadyEvent) {
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
          _errorMsg = evt.reason ?? 'OTA failed';
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
        _errorMsg = 'Could not read file';
      });
      return;
    }

    _fileName = file.name;
    _fileSize = _fileBytes!.length;
    _bytesWritten = 0;

    // Compute MD5
    final md5 = crypto_lib.md5.convert(_fileBytes!).toString();

    setState(() => _state = _OtaState.starting);
    await widget.ble.startBleOta(size: _fileSize, md5: md5);
  }

  Future<void> _doUpload() async {
    if (_fileBytes == null) return;

    int offset = 0;
    while (offset < _fileBytes!.length) {
      if (_state != _OtaState.uploading) return;

      final end = (offset + _chunkSize).clamp(0, _fileBytes!.length);
      final chunk = _fileBytes!.sublist(offset, end);

      final ok = await widget.ble.sendBleOtaChunk(chunk);
      if (!ok) {
        setState(() {
          _state = _OtaState.error;
          _errorMsg = 'Failed to send chunk at offset $offset';
        });
        return;
      }

      offset = end;
      setState(() => _bytesWritten = offset);

      // Throttle: BLE throughput ~10-15 KB/s, small delay between chunks
      await Future<void>.delayed(const Duration(milliseconds: 10));
    }

    setState(() => _state = _OtaState.verifying);
    await widget.ble.endBleOta();
  }

  Future<void> _abort() async {
    await widget.ble.abortBleOta();
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
    return Scaffold(
      backgroundColor: palette.surface,
      appBar: AppBar(
        title: const Text('Firmware Update'),
        backgroundColor: palette.surface,
        foregroundColor: palette.onSurface,
        elevation: 0,
      ),
      body: Stack(
        children: [
          const MeshBackground(),
          Center(
            child: Padding(
              padding: const EdgeInsets.all(24),
              child: _buildBody(palette),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildBody(AppPalette palette) {
    switch (_state) {
      case _OtaState.idle:
        return Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.system_update_alt, size: 64, color: palette.primary),
            const SizedBox(height: 16),
            Text(
              'BLE Firmware Update',
              style: TextStyle(fontSize: 20, fontWeight: FontWeight.w600, color: palette.onSurface),
            ),
            const SizedBox(height: 8),
            Text(
              'Select a .bin firmware file to upload over BLE.',
              style: TextStyle(color: palette.onSurfaceVariant),
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 32),
            FilledButton.icon(
              onPressed: _pickAndStart,
              icon: const Icon(Icons.file_open),
              label: const Text('Select Firmware'),
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
            Text('Starting OTA...', style: TextStyle(color: palette.onSurface)),
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
              'Uploading firmware...',
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
              child: const Text('Cancel'),
            ),
          ],
        );

      case _OtaState.verifying:
        return Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const CircularProgressIndicator(),
            const SizedBox(height: 16),
            Text('Verifying & applying...', style: TextStyle(color: palette.onSurface)),
          ],
        );

      case _OtaState.done:
        return Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.check_circle, size: 64, color: palette.success),
            const SizedBox(height: 16),
            Text(
              'Update Successful!',
              style: TextStyle(fontSize: 20, fontWeight: FontWeight.w600, color: palette.success),
            ),
            const SizedBox(height: 8),
            Text(
              'Device is rebooting. Reconnect in a few seconds.',
              style: TextStyle(color: palette.onSurfaceVariant),
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 24),
            FilledButton(
              onPressed: () => Navigator.of(context).pop(),
              child: const Text('Done'),
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
              'Update Failed',
              style: TextStyle(fontSize: 20, fontWeight: FontWeight.w600, color: palette.error),
            ),
            const SizedBox(height: 8),
            Text(
              _errorMsg ?? 'Unknown error',
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
              child: const Text('Try Again'),
            ),
          ],
        );
    }
  }
}
