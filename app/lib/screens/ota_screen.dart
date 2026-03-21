import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:file_picker/file_picker.dart';
import 'package:crypto/crypto.dart' as crypto_lib;

import '../app_navigator.dart';
import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../widgets/mesh_background.dart';
import '../widgets/rift_dialogs.dart';

enum _OtaState { idle, picking, starting, uploading, verifying, done, error }

const double _kOtaScreenOuterPadding = 24;

/// Показывает OTA в модальном диалоге (корневая оболочка — [RiftDialogFrame]).
Future<void> showOtaDialog(BuildContext context, RiftLinkBle ble) {
  return showAppDialog<void>(
    context: context,
    barrierDismissible: false,
    builder: (ctx) {
      return RiftDialogFrame(
        maxWidth: 400,
        padding: const EdgeInsets.fromLTRB(12, 8, 12, 12),
        child: SingleChildScrollView(
          child: _OtaFlow(ble: ble, embeddedInDialog: true),
        ),
      );
    },
  );
}

class OtaScreen extends StatefulWidget {
  final RiftLinkBle ble;
  const OtaScreen({super.key, required this.ble});

  @override
  State<OtaScreen> createState() => _OtaScreenState();
}

class _OtaScreenState extends State<OtaScreen> {
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
            padding: const EdgeInsets.all(_kOtaScreenOuterPadding),
            child: _OtaFlow(ble: widget.ble),
          ),
        ),
      ),
    );
  }
}

class _OtaFlow extends StatefulWidget {
  final RiftLinkBle ble;
  final bool embeddedInDialog;

  const _OtaFlow({
    required this.ble,
    this.embeddedInDialog = false,
  });

  @override
  State<_OtaFlow> createState() => _OtaFlowState();
}

class _OtaFlowState extends State<_OtaFlow> {
  static const double _kRadius = 12;
  static const double _kIconSize = 64;
  static const double _kRingSize = 120;
  static const double _kIndeterminateSpinner = 40;
  static const double _kUploadStroke = 6;
  static const double _kIndeterminateStroke = 3;
  static const double _kGapSm = 8;
  static const double _kGapMd = 16;
  static const double _kGapLg = 24;
  static const double _kPanelMaxWidth = 400;
  static const EdgeInsets _kPanelPadding = EdgeInsets.symmetric(horizontal: 24, vertical: 28);

  _OtaState _state = _OtaState.idle;
  String? _fileName;
  int _fileSize = 0;
  int _bytesWritten = 0;
  int _chunkSize = 509;
  String? _errorMsg;
  StreamSubscription? _evtSub;
  Timer? _startTimeout;

  bool get _canDismiss =>
      _state == _OtaState.idle ||
      _state == _OtaState.done ||
      _state == _OtaState.error;

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

  TextStyle _titleStyle(AppPalette p) => TextStyle(
        fontSize: 20,
        fontWeight: FontWeight.w600,
        color: p.onSurface,
      );

  TextStyle _titleStyleAccent(AppPalette p, Color accent) => TextStyle(
        fontSize: 20,
        fontWeight: FontWeight.w600,
        color: accent,
      );

  TextStyle _bodyStyle(AppPalette p) => TextStyle(
        color: p.onSurfaceVariant,
        fontSize: 14,
        height: 1.35,
      );

  TextStyle _captionStyle(AppPalette p) => TextStyle(
        color: p.onSurfaceVariant,
        fontSize: 13,
        height: 1.3,
      );

  TextStyle _statusStyle(AppPalette p) => TextStyle(
        fontSize: 16,
        color: p.onSurface,
        height: 1.3,
      );

  TextStyle _progressPercentStyle(AppPalette p) => TextStyle(
        fontSize: 24,
        fontWeight: FontWeight.bold,
        color: p.onSurface,
      );

  Widget _otaPanel(AppPalette p, {required Widget child}) {
    return ConstrainedBox(
      constraints: const BoxConstraints(maxWidth: _kPanelMaxWidth),
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: p.card,
          borderRadius: BorderRadius.circular(_kRadius),
          border: Border.all(color: p.divider.withOpacity(0.55)),
        ),
        child: Padding(
          padding: _kPanelPadding,
          child: child,
        ),
      ),
    );
  }

  Widget _indeterminateProgress(AppPalette p) {
    return SizedBox(
      width: _kIndeterminateSpinner,
      height: _kIndeterminateSpinner,
      child: CircularProgressIndicator(
        strokeWidth: _kIndeterminateStroke,
        color: p.primary,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final palette = context.palette;
    final l = context.l10n;
    final body = _buildBody(context, palette, l);
    return PopScope(
      canPop: _canDismiss,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (widget.embeddedInDialog && _state == _OtaState.idle)
            Align(
              alignment: Alignment.centerRight,
              child: IconButton(
                icon: Icon(Icons.close, color: palette.onSurfaceVariant),
                tooltip: MaterialLocalizations.of(context).closeButtonTooltip,
                onPressed: () => Navigator.of(context).pop(),
              ),
            ),
          body,
        ],
      ),
    );
  }

  Widget _buildBody(BuildContext context, AppPalette palette, AppLocalizations l) {
    switch (_state) {
      case _OtaState.idle:
        return _otaPanel(
          palette,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Align(
                alignment: Alignment.center,
                child: Icon(Icons.system_update_alt, size: _kIconSize, color: palette.primary),
              ),
              SizedBox(height: _kGapMd),
              Text(
                l.tr('firmware_update_title'),
                style: _titleStyle(palette),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: _kGapSm),
              Text(
                l.tr('ota_ble_intro_desc'),
                style: _bodyStyle(palette),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: _kGapLg),
              FilledButton.icon(
                onPressed: _pickAndStart,
                icon: const Icon(Icons.file_open),
                label: Text(l.tr('ota_select_firmware')),
              ),
            ],
          ),
        );

      case _OtaState.picking:
        return _otaPanel(
          palette,
          child: Align(
            alignment: Alignment.center,
            child: _indeterminateProgress(palette),
          ),
        );

      case _OtaState.starting:
        return _otaPanel(
          palette,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Align(alignment: Alignment.center, child: _indeterminateProgress(palette)),
              SizedBox(height: _kGapMd),
              Text(
                l.tr('ota_state_starting'),
                style: _statusStyle(palette),
                textAlign: TextAlign.center,
              ),
              if (_fileName != null) ...[
                SizedBox(height: _kGapSm),
                Text(
                  '$_fileName (${(_fileSize / 1024).toStringAsFixed(0)} KB)',
                  style: _captionStyle(palette),
                  textAlign: TextAlign.center,
                ),
              ],
            ],
          ),
        );

      case _OtaState.uploading:
        return _otaPanel(
          palette,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Align(
                alignment: Alignment.center,
                child: SizedBox(
                  width: _kRingSize,
                  height: _kRingSize,
                  child: Stack(
                    alignment: Alignment.center,
                    children: [
                      SizedBox(
                        width: _kRingSize,
                        height: _kRingSize,
                        child: CircularProgressIndicator(
                          value: _progress,
                          strokeWidth: _kUploadStroke,
                          backgroundColor: palette.divider,
                          color: palette.primary,
                        ),
                      ),
                      Text(
                        '${(_progress * 100).toStringAsFixed(0)}%',
                        style: _progressPercentStyle(palette),
                      ),
                    ],
                  ),
                ),
              ),
              SizedBox(height: _kGapMd),
              Text(
                l.tr('ota_state_uploading'),
                style: _statusStyle(palette),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: _kGapSm),
              Text(
                '${(_bytesWritten / 1024).toStringAsFixed(0)} / ${(_fileSize / 1024).toStringAsFixed(0)} KB',
                style: _captionStyle(palette),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: _kGapLg),
              OutlinedButton(
                onPressed: _abort,
                child: Text(l.tr('ota_cancel')),
              ),
            ],
          ),
        );

      case _OtaState.verifying:
        return _otaPanel(
          palette,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Align(alignment: Alignment.center, child: _indeterminateProgress(palette)),
              SizedBox(height: _kGapMd),
              Text(
                l.tr('ota_state_verifying'),
                style: _statusStyle(palette),
                textAlign: TextAlign.center,
              ),
            ],
          ),
        );

      case _OtaState.done:
        return _otaPanel(
          palette,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Align(
                alignment: Alignment.center,
                child: Icon(Icons.check_circle, size: _kIconSize, color: palette.success),
              ),
              SizedBox(height: _kGapMd),
              Text(
                l.tr('ota_done_title'),
                style: _titleStyleAccent(palette, palette.success),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: _kGapSm),
              Text(
                l.tr('ota_done_desc'),
                style: _bodyStyle(palette),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: _kGapLg),
              FilledButton(
                onPressed: () => Navigator.of(context).pop(),
                child: Text(l.tr('ota_done_button')),
              ),
            ],
          ),
        );

      case _OtaState.error:
        return _otaPanel(
          palette,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Align(
                alignment: Alignment.center,
                child: Icon(Icons.error_outline, size: _kIconSize, color: palette.error),
              ),
              SizedBox(height: _kGapMd),
              Text(
                l.tr('ota_error_title'),
                style: _titleStyleAccent(palette, palette.error),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: _kGapSm),
              Text(
                _errorMsg ?? l.tr('ota_unknown_error'),
                style: _bodyStyle(palette),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: _kGapLg),
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
          ),
        );
    }
  }
}
