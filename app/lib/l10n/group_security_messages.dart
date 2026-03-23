import 'app_localizations.dart';
import '../ble/riftlink_ble.dart';

/// Человекочитаемое сообщение для [RiftLinkGroupSecurityErrorEvent] (RU/EN через [AppLocalizations]).
String localizedGroupSecurityError(AppLocalizations l, RiftLinkGroupSecurityErrorEvent evt) {
  final code = evt.code;
  final msg = evt.msg;
  if (code == 'group_v3_store_failed' || code == 'group_v2_store_failed') {
    if (msg.contains('owner signing') || msg.contains('persist')) {
      return l.tr('group_security_error_store_owner_key');
    }
    return l.tr('group_security_error_store_group');
  }
  if (code == 'group_v3_bad') {
    if (msg.contains('Missing')) {
      return l.tr('group_security_error_missing_fields');
    }
    if (msg.contains('invalid separator') || msg.contains('separator')) {
      return l.tr('group_security_error_bad_name');
    }
  }
  if (code == 'group_v2_invite_bad' || code == 'group_v2_invite_expired' || code.startsWith('group_v31_invite')) {
    return _localizedGroupInviteSecurity(l, code, msg);
  }
  if (msg.trim().isEmpty) {
    return l.tr('group_security_error_code', {'code': code});
  }
  return '${l.tr('error')}: $msg';
}

String _localizedGroupInviteSecurity(AppLocalizations l, String code, String msg) {
  if (code == 'group_v2_invite_expired' || msg.contains('expired')) {
    return l.tr('group_invite_error_expired');
  }
  if (msg.contains('Bad invite base64') || msg.contains('Missing invite')) {
    return l.tr('group_invite_error_bad_base64');
  }
  if (msg.contains('Malformed')) {
    return l.tr('group_invite_error_malformed');
  }
  if (msg.contains('Invalid key/channel')) {
    return l.tr('group_invite_error_invalid_key');
  }
  if (msg.contains('signature') || msg.contains('Signature')) {
    return l.tr('group_invite_error_signature');
  }
  if (msg.trim().isEmpty) {
    return l.tr('group_security_error_code', {'code': code});
  }
  return '${l.tr('error')}: $msg';
}
