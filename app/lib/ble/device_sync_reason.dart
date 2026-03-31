/// Причина запроса снимка узла (`cmd:info`) — единая точка дедупликации и трассировки.
enum DeviceSyncReason {
  /// BLE/WebSocket только что поднят (connect / connectWifi).
  onTransportUp,

  /// Явное действие пользователя (pull-to-refresh, кнопка).
  userRefresh,

  /// Экран стал видимым / resumed.
  screenVisible,

  /// После изменения настроек на устройстве через приложение.
  postSettingsChange,

  /// Долгая тишина транспорта — проба живости.
  watchdogIdle,

  /// После успешного восстановления сессии (без дублирования с onTransportUp).
  reconnect,

  /// Прочие вызовы (совместимость).
  generic,
}
