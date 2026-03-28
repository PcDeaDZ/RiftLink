import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:file_picker/file_picker.dart';
import 'package:crypto/crypto.dart' as crypto_lib;

import '../app_navigator.dart';
import '../ble/riftlink_ble.dart';
import '../l10n/app_localizations.dart';
import '../theme/app_theme.dart';
import '../theme/design_tokens.dart';
import '../widgets/app_primitives.dart';
import '../widgets/mesh_background.dart';
import '../widgets/rift_dialogs.dart';
import '../support/nrf_firmware_errors.dart';

enum _OtaState { idle, picking, starting, uploading, verifying, done, error }

/// Показывает OTA в модальном диалоге (корневая оболочка — [RiftDialogFrame]).
Future<void> showOtaDialog(BuildContext context, RiftLinkBle ble) {
  return showAppDialog<void>(
    context: context,
    barrierDismissible: false,
    builder: (ctx) {
      return RiftDialogFrame(
        maxWidth: 400,
        padding: const EdgeInsets.fromLTRB(AppSpacing.md, AppSpacing.sm, AppSpacing.md, AppSpacing.md),
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
      appBar: riftAppBar(
        context,
        title: l.tr('firmware_update_title'),
        showBack: Navigator.canPop(context),
      ),
      body: MeshBackgroundWrapper(
        child: Center(
          child: Padding(
            padding: const EdgeInsets.all(AppSpacing.xxl),
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
  static const double _kPanelMaxWidth = 400;
  static final EdgeInsets _kPanelPadding = EdgeInsets.symmetric(
    horizontal: AppSpacing.xxl,
    vertical: AppSpacing.xxl + AppSpacing.xs,
  );

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
    if (evt is RiftLinkErrorEvent) {
      if (_state == _OtaState.starting &&
          (evt.code == 'ble_ota_unsupported' || evt.code == 'ota_unsupported')) {
        _startTimeout?.cancel();
        final l = context.l10n;
        setState(() {
          _state = _OtaState.error;
          _errorMsg = nrfFirmwareErrorUserMessage(l, evt.code, evt.msg);
        });
        return;
      }
    }
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

  TextStyle _titleStyle(AppPalette p) => AppTypography.panelTitleBase().copyWith(color: p.onSurface);

  TextStyle _titleStyleAccent(AppPalette p, Color accent) => AppTypography.panelTitleBase().copyWith(color: accent);

  TextStyle _bodyStyle(AppPalette p) => AppTypography.bodyLargeBase().copyWith(color: p.onSurfaceVariant);

  TextStyle _captionStyle(AppPalette p) => AppTypography.labelBase().copyWith(color: p.onSurfaceVariant, height: 1.3);

  TextStyle _statusStyle(AppPalette p) => AppTypography.bodyLargeBase().copyWith(color: p.onSurface, height: 1.3);

  TextStyle _progressPercentStyle(AppPalette p) => AppTypography.statDisplayBase().copyWith(color: p.onSurface);

  Widget _otaPanel(AppPalette p, {required Widget child}) {
    return ConstrainedBox(
      constraints: const BoxConstraints(maxWidth: _kPanelMaxWidth),
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: p.card,
          borderRadius: BorderRadius.circular(AppRadius.card),
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
      width: AppOtaLayout.indeterminateSpinner,
      height: AppOtaLayout.indeterminateSpinner,
      child: CircularProgressIndicator(
        strokeWidth: AppOtaLayout.indeterminateStroke,
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
                icon: Icon(Icons.close, color: palette.onSurfaceVariant, size: AppIconSize.md),
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
                child: Icon(Icons.system_update_alt, size: AppOtaLayout.heroIcon, color: palette.primary),
              ),
              SizedBox(height: AppSpacing.lg),
              Text(
                l.tr('firmware_update_title'),
                style: _titleStyle(palette),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: AppSpacing.sm),
              Text(
                l.tr('ota_ble_intro_desc'),
                style: _bodyStyle(palette),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: AppSpacing.xxl),
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
              SizedBox(height: AppSpacing.lg),
              Text(
                l.tr('ota_state_starting'),
                style: _statusStyle(palette),
                textAlign: TextAlign.center,
              ),
              if (_fileName != null) ...[
                SizedBox(height: AppSpacing.sm),
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
                  width: AppOtaLayout.ring,
                  height: AppOtaLayout.ring,
                  child: Stack(
                    alignment: Alignment.center,
                    children: [
                      SizedBox(
                        width: AppOtaLayout.ring,
                        height: AppOtaLayout.ring,
                        child: CircularProgressIndicator(
                          value: _progress,
                          strokeWidth: AppOtaLayout.ringStroke,
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
              SizedBox(height: AppSpacing.lg),
              Text(
                l.tr('ota_state_uploading'),
                style: _statusStyle(palette),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: AppSpacing.sm),
              Text(
                '${(_bytesWritten / 1024).toStringAsFixed(0)} / ${(_fileSize / 1024).toStringAsFixed(0)} KB',
                style: _captionStyle(palette),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: AppSpacing.xxl),
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
              SizedBox(height: AppSpacing.lg),
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
                child: Icon(Icons.check_circle, size: AppOtaLayout.heroIcon, color: palette.success),
              ),
              SizedBox(height: AppSpacing.lg),
              Text(
                l.tr('ota_done_title'),
                style: _titleStyleAccent(palette, palette.success),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: AppSpacing.sm),
              Text(
                l.tr('ota_done_desc'),
                style: _bodyStyle(palette),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: AppSpacing.xxl),
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
                child: Icon(Icons.error_outline, size: AppOtaLayout.heroIcon, color: palette.error),
              ),
              SizedBox(height: AppSpacing.lg),
              Text(
                l.tr('ota_error_title'),
                style: _titleStyleAccent(palette, palette.error),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: AppSpacing.sm),
              Text(
                _errorMsg ?? l.tr('ota_unknown_error'),
                style: _bodyStyle(palette),
                textAlign: TextAlign.center,
              ),
              SizedBox(height: AppSpacing.xxl),
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
