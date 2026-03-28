// Сообщения для кодов evt:error на nRF52840 — см. docs/API.md (прошивки без Wi‑Fi).

import '../l10n/app_localizations.dart';

/// Возвращает локализованное пояснение для известных кодов nRF, иначе [firmwareMsg] или [code].
String nrfFirmwareErrorUserMessage(AppLocalizations l, String code, String firmwareMsg) {
  final key = _nrfErrKey(code);
  if (key != null) return l.tr(key);
  final t = firmwareMsg.trim();
  if (t.isNotEmpty) return t;
  return code;
}

String? _nrfErrKey(String code) {
  switch (code) {
    case 'ota_unsupported':
      return 'nrf_err_ota_unsupported';
    case 'ble_ota_unsupported':
      return 'nrf_err_ble_ota_unsupported';
    case 'poweroff_unsupported':
      return 'nrf_err_poweroff_unsupported';
    case 'powersave_unsupported':
      return 'nrf_err_powersave_unsupported';
    case 'espnow_unsupported':
      return 'nrf_err_espnow_unsupported';
    case 'wifi':
      return 'nrf_err_wifi';
    case 'radioMode':
      return 'nrf_err_radio_mode';
    case 'group_legacy_cmd_unsupported':
      return 'nrf_err_group_legacy';
    default:
      return null;
  }
}
