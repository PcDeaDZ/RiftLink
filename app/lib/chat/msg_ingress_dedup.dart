/// Ключ для дедупликации входящих `evt:msg` в [ChatEventIngestor].
///
/// При [msgId] > 0 ключ стабилен (повтор той же доставки схлопывается).
/// Без валидного id каждое событие получает уникальный суффикс [noIdSequence],
/// иначе разные сообщения с одинаковым текстом ошибочно схлопывались бы.
///
/// Прошивка: в `notifyMsg` поле `msgId` в JSON только при `msgId != 0`; для части путей
/// (например широковещание `OP_MSG` без извлечения id в `main.cpp`) id может быть 0 —
/// тогда дедуп по стабильному id на телефоне невозможен, остаётся уникальный ключ приёма.
String buildMsgIngressDedupKey({
  required String fromNormalized,
  required String scopeTag,
  required int? msgId,
  required int noIdSequence,
}) {
  if (msgId != null && msgId > 0) {
    return 'msg:$fromNormalized:$msgId:$scopeTag';
  }
  return 'msg:$fromNormalized:noid:$noIdSequence:$scopeTag';
}
