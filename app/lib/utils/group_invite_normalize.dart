/// Подготовка строки инвайта группы перед отправкой на узел.
///
/// Мессенджеры и почта часто вставляют переносы строк / пробелы внутри BASE64 — прошивка
/// (`mbedtls_base64_decode`) без этого даёт «Bad invite base64».
String normalizeGroupInvitePayload(String raw) {
  var s = raw.trim();
  if (s.isEmpty) return '';
  s = s.replaceAll(RegExp(r'[\uFEFF\u200B\u200C\u200D]'), '');
  if ((s.startsWith('"') && s.endsWith('"')) || (s.startsWith("'") && s.endsWith("'"))) {
    s = s.substring(1, s.length - 1).trim();
  }
  s = s.replaceAll(RegExp(r'\s+'), '');
  if (s.isEmpty) return '';
  // URL-safe base64 (RFC 4648 §5) → обычный
  s = s.replaceAll('-', '+').replaceAll('_', '/');
  final mod = s.length % 4;
  if (mod == 1) return '';
  if (mod == 2) {
    s += '==';
  } else if (mod == 3) {
    s += '=';
  }
  return s;
}
