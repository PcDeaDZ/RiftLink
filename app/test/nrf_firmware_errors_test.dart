import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/material.dart';
import 'package:riftlink_app/support/nrf_firmware_errors.dart';
import 'package:riftlink_app/l10n/app_localizations.dart';

void main() {
  test('nrfFirmwareErrorUserMessage maps API codes to l10n', () {
    final l = AppLocalizations(const Locale('en'));
    expect(
      nrfFirmwareErrorUserMessage(l, 'ota_unsupported', 'raw'),
      contains('Wi‑Fi OTA'),
    );
    expect(
      nrfFirmwareErrorUserMessage(l, 'ble_ota_unsupported', ''),
      contains('BLE'),
    );
    expect(
      nrfFirmwareErrorUserMessage(l, 'radioMode', ''),
      contains('BLE'),
    );
    expect(nrfFirmwareErrorUserMessage(l, 'unknown_code', 'device said'), 'device said');
    expect(nrfFirmwareErrorUserMessage(l, 'unknown_code', ''), 'unknown_code');
  });

  test('Russian locale uses nrf_err strings', () {
    final l = AppLocalizations(const Locale('ru'));
    final m = nrfFirmwareErrorUserMessage(l, 'wifi', '');
    expect(m.isNotEmpty, true);
    expect(m.contains('nRF'), true);
  });
}
