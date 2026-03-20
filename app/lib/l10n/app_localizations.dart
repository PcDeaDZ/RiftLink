/// RiftLink Localization — EN / RU

import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

const _localeKey = 'riftlink_locale';

final Map<String, Map<String, String>> _strings = {
  'app_title': {'en': 'RiftLink', 'ru': 'RiftLink'},
  'find_device': {'en': 'Find device', 'ru': 'Найти устройство'},
  'recent_devices': {'en': 'Recent devices', 'ru': 'Недавние устройства'},
  'search_devices': {'en': 'Search devices', 'ru': 'Поиск устройств'},
  'recent_empty': {'en': 'No recent devices. Search to connect.', 'ru': 'Нет недавних устройств. Нажмите поиск для подключения.'},
  'scan_no_devices': {'en': 'No devices found.', 'ru': 'Устройства не найдены.'},
  'searching_for': {'en': 'Searching for {name}...', 'ru': 'Поиск {name}...'},
  'connecting_to': {'en': 'Connecting to {name}...', 'ru': 'Подключение к {name}...'},
  'device': {'en': 'device', 'ru': 'устройство'},
  'turn_on_heltec': {'en': 'Turn on Heltec LoRa and press the button', 'ru': 'Включите Heltec LoRa и нажмите кнопку'},
  'scanning': {'en': 'Scanning...', 'ru': 'Сканирование...'},
  'stop_scan': {'en': 'Stop', 'ru': 'Остановить'},
  'found': {'en': 'Found:', 'ru': 'Найдено:'},
  'about': {'en': 'About', 'ru': 'О приложении'},
  'about_version': {'en': 'Version', 'ru': 'Версия'},
  'about_build': {'en': 'Build', 'ru': 'Сборка'},
  'about_legal': {'en': 'User must comply with local laws. Manufacturer is not liable for misuse.', 'ru': 'Пользователь обязан соблюдать законы своей страны. Производитель не несёт ответственности за неправомерное использование.'},
  'about_desc': {'en': 'Mesh protocol for LoRa. E2E encryption, BLE, geolocation, telemetry.', 'ru': 'Mesh-протокол для LoRa. E2E-шифрование, BLE, геолокация, телеметрия.'},
  'connected': {'en': 'Connected', 'ru': 'Подключено'},
  'disconnected': {'en': 'Disconnected', 'ru': 'Отключено'},
  'map': {'en': 'Map', 'ru': 'Карта узлов'},
  'contacts': {'en': 'Contacts', 'ru': 'Контакты'},
  'contacts_groups_title': {'en': 'Contacts & groups', 'ru': 'Контакты и группы'},
  'groups_add_sheet_hint': {'en': 'Enter a number from 2 to 4294967295. ID 1 is reserved for mesh broadcast.', 'ru': 'Число от 2 до 4294967295. ID 1 зарезервирован под широковещание mesh.'},
  'ping': {'en': 'Check link', 'ru': 'Проверить связь'},
  'menu_tools': {'en': 'Tools', 'ru': 'Инструменты'},
  'menu_tools_subtitle': {'en': 'Map, location, mesh, ping, self-test', 'ru': 'Карта, геолокация, mesh, ping, самотест'},
  'send': {'en': 'Send', 'ru': 'Отправить'},
  'message_hint': {'en': 'Message...', 'ru': 'Сообщение...'},
  'to': {'en': 'To:', 'ru': 'Кому:'},
  'recipient_title': {'en': 'Recipient', 'ru': 'Получатель'},
  'recipient_search_hint': {'en': 'Search by name or ID', 'ru': 'Поиск по имени или ID'},
  'recipient_no_match': {'en': 'No matches', 'ru': 'Нет совпадений'},
  'saved_contacts': {'en': 'Saved contacts', 'ru': 'Сохранённые контакты'},
  'broadcast': {'en': 'Broadcast', 'ru': 'Broadcast'},
  'neighbors': {'en': 'Neighbors', 'ru': 'Соседи'},
  'no_messages': {'en': 'Start chatting', 'ru': 'Начните переписку'},
  'location': {'en': 'Send location', 'ru': 'Отправить геолокацию'},
  'voice': {'en': 'Voice message', 'ru': 'Голосовое сообщение'},
  'group': {'en': 'Group', 'ru': 'Группа'},
  'nickname': {'en': 'Nickname', 'ru': 'Никнейм'},
  'nickname_hint': {'en': 'Up to 16 characters', 'ru': 'До 16 символов'},
  'region': {'en': 'Region', 'ru': 'Регион'},
  'region_warning': {'en': 'Ensure you are in the selected country. Regulatory violation risks fines.', 'ru': 'Убедитесь, что находитесь в выбранной стране. Нарушение регуляторики — риск штрафов.'},
  'channel_eu': {'en': 'LoRaWAN channel (EU/UK):', 'ru': 'Канал LoRaWAN (EU/UK):'},
  'cancel': {'en': 'Cancel', 'ru': 'Отмена'},
  'ok': {'en': 'OK', 'ru': 'OK'},
  'add_contact': {'en': 'Add contact', 'ru': 'Добавить в контакты'},
  'edit_contact': {'en': 'Edit contact', 'ru': 'Редактировать контакт'},
  'contact_nickname': {'en': 'Nickname', 'ru': 'Никнейм'},
  'contact_name_hint': {'en': 'Contact name', 'ru': 'Имя контакта'},
  'save': {'en': 'Save', 'ru': 'Сохранить'},
  'ping_title': {'en': 'Ping — check link', 'ru': 'Ping — проверка связи'},
  'ping_hint': {'en': 'Node ID (8 hex): A1B2C3D4', 'ru': 'Node ID (8 hex): A1B2C3D4'},
  'ping_invalid': {'en': 'Enter 8 hex characters (e.g. A1B2C3D4)', 'ru': 'Введите 8 hex-символов (например A1B2C3D4)'},
  'ota_title': {'en': 'OTA mode', 'ru': 'OTA режим'},
  'ota_connect': {'en': 'Connect to WiFi:', 'ru': 'Подключитесь к WiFi:'},
  'ota_then': {'en': 'Then upload firmware:', 'ru': 'Затем загрузите прошивку:'},
  'ota_started': {'en': 'OTA started. Connect to WiFi RiftLink-OTA...', 'ru': 'OTA запущен. Подключитесь к WiFi RiftLink-OTA...'},
  'ota_failed': {'en': 'Failed to send OTA command', 'ru': 'Не удалось отправить команду OTA'},
  'ota_start_hint': {'en': 'Sends OTA command — device opens a Wi‑Fi AP for firmware upload.', 'ru': 'Отправить команду OTA — устройство поднимет Wi‑Fi для прошивки.'},
  'no_neighbors_voice': {'en': 'No neighbors for voice message', 'ru': 'Нет соседей для голосового сообщения'},
  'waiting_key': {'en': 'Waiting for key', 'ru': 'Ожидание ключа'},
  'send_voice': {'en': 'Send voice', 'ru': 'Отправить голос'},
  'voice_recording': {'en': 'Recording... Press stop to send', 'ru': 'Запись... Нажмите стоп для отправки'},
  'voice_mic_error': {'en': 'Microphone access error', 'ru': 'Ошибка доступа к микрофону'},
  'voice_send_error': {'en': 'Voice send error', 'ru': 'Ошибка отправки голоса'},
  'contact_added': {'en': 'Contact added', 'ru': 'Контакт добавлен'},
  'link_ok': {'en': 'Link with {from} established', 'ru': 'Связь с {from} установлена'},
  'ping_sent': {'en': 'Ping sent to {id}. Waiting for response...', 'ru': 'Пинг отправлен на {id}. Ожидание ответа...'},
  'ping_timeout': {'en': 'Ping to {id} did not reach — no response', 'ru': 'Пинг до {id} не дошёл — ответа нет'},
  'error': {'en': 'Error', 'ru': 'Ошибка'},
  'loc_timeout': {'en': 'Location timeout — go to open area', 'ru': 'Таймаут геолокации — выйдите на открытое место'},
  'loc_denied': {'en': 'No location access. Enable in app settings.', 'ru': 'Нет доступа к геолокации. Включите в настройках приложения.'},
  'loc_unavailable': {'en': 'Location unavailable on this device', 'ru': 'Геолокация недоступна на этом устройстве'},
  'contacts_empty': {'en': 'No saved contacts', 'ru': 'Нет сохранённых контактов'},
  'contacts_hint': {'en': 'Add from neighbors or manually', 'ru': 'Добавьте контакт из соседей или вручную'},
  'delete_contact': {'en': 'Delete contact?', 'ru': 'Удалить контакт?'},
  'delete': {'en': 'Delete', 'ru': 'Удалить'},
  'add_from_neighbors': {'en': 'Add from neighbors', 'ru': 'Добавить из соседей'},
  'node_id_hex': {'en': 'Node ID (8 hex)', 'ru': 'Node ID (8 hex)'},
  'invalid_hex': {'en': 'Enter 8 hex characters (A1B2C3D4)', 'ru': 'Введите 8 hex-символов (A1B2C3D4)'},
  'ble_timeout': {'en': 'Timeout — device did not respond. Move closer.', 'ru': 'Таймаут — устройство не ответило. Подойдите ближе.'},
  'ble_refused': {'en': 'Connection refused. Reboot the device.', 'ru': 'Подключение отклонено. Перезагрузите устройство.'},
  'ble_disconnect': {'en': 'Device disconnected. Try again.', 'ru': 'Устройство отключилось. Попробуйте снова.'},
  'ble_turn_on': {'en': 'Turn on Bluetooth.', 'ru': 'Включите Bluetooth.'},
  'ble_permission': {'en': 'No Bluetooth permission. Check settings.', 'ru': 'Нет разрешения на Bluetooth. Проверьте настройки.'},
  'open_settings': {'en': 'Open settings', 'ru': 'Открыть настройки'},
  'ble_no_service': {'en': 'RiftLink service not found — possibly different device', 'ru': 'Не найден сервис RiftLink — возможно, другое устройство'},
  'lang': {'en': 'Language', 'ru': 'Язык'},
  'lang_en': {'en': 'English', 'ru': 'English'},
  'lang_ru': {'en': 'Russian', 'ru': 'Русский'},
  'theme': {'en': 'Theme', 'ru': 'Тема'},
  'theme_system': {'en': 'As in system', 'ru': 'Как в системе'},
  'theme_light': {'en': 'Light', 'ru': 'Светлая'},
  'theme_dark': {'en': 'Dark', 'ru': 'Тёмная'},
  'theme_hint': {'en': 'Light, dark, or system default', 'ru': 'Светлая, тёмная или как в системе'},
  'groups': {'en': 'Groups', 'ru': 'Группы'},
  'add_group': {'en': 'Add group', 'ru': 'Добавить группу'},
  'group_id_hint': {'en': 'Group ID (2–4294967295; 1 is reserved)', 'ru': 'ID группы (2–4294967295; 1 зарезервирован)'},
  'group_id_reserved': {'en': 'ID 1 is reserved for mesh broadcast', 'ru': 'ID 1 зарезервирован под широковещание mesh'},
  'add': {'en': 'Add', 'ru': 'Добавить'},
  'invalid_group_id': {'en': 'Invalid group ID', 'ru': 'Неверный ID группы'},
  'added': {'en': 'added', 'ru': 'добавлена'},
  'no_groups': {'en': 'No groups. Add one to receive group messages.', 'ru': 'Нет групп. Добавьте группу для получения сообщений.'},
  'groups_hint': {'en': 'Groups are broadcast channels. Add a group ID to subscribe.', 'ru': 'Группы — каналы рассылки. Добавьте ID группы для подписки.'},
  'refresh': {'en': 'Refresh', 'ru': 'Обновить'},
  'settings': {'en': 'Settings', 'ru': 'Настройки'},
  'connect_first': {'en': 'Connect to device first', 'ru': 'Сначала подключитесь к устройству'},
  'connection': {'en': 'Connection', 'ru': 'Соединение'},
  'disconnect': {'en': 'Disconnect', 'ru': 'Отключиться'},
  'powersave': {'en': 'Power saving', 'ru': 'Энергосбережение'},
  'powersave_hint': {'en': 'Deep sleep when the device is idle and not connected to the app over BLE (board-dependent).', 'ru': 'Глубокий сон в простое, пока нет BLE-связи с приложением (зависит от платы).'},
  'powersave_mode_normal': {'en': 'Normal', 'ru': 'Обычный'},
  'powersave_mode_eco': {'en': 'Eco', 'ru': 'Экономия'},
  'settings_energy_title': {'en': 'Battery & energy', 'ru': 'Батарея и энергия'},
  'settings_energy_hint': {'en': 'Node: deep sleep without BLE. Phone: less GPU work when mesh animation is off.', 'ru': 'Узел: глубокий сон без BLE. Телефон: меньше нагрузка на GPU, если выключить анимацию фона.'},
  'settings_energy_node': {'en': 'Node (device)', 'ru': 'Узел (устройство)'},
  'settings_energy_app': {'en': 'App (this phone)', 'ru': 'Приложение (этот телефон)'},
  'other': {'en': 'Other', 'ru': 'Прочее'},
  'mesh_animation': {'en': 'Mesh background animation', 'ru': 'Анимация фона сети'},
  'mesh_animation_hint': {'en': 'Moving background on chat; uses more battery when on.', 'ru': 'Движущийся фон в чате; при включении больше расход батареи.'},
  'selftest': {'en': 'Selftest', 'ru': 'Самотест'},
  'selftest_summary_ok': {'en': 'All checks passed', 'ru': 'Всё в порядке'},
  'selftest_summary_fail': {'en': 'Some checks failed', 'ru': 'Есть проблемы'},
  'selftest_radio': {'en': 'Radio', 'ru': 'Радио'},
  'selftest_display': {'en': 'Display', 'ru': 'Дисплей'},
  'selftest_voltage': {'en': 'Voltage: {v} V', 'ru': 'Напряжение: {v} В'},
  'selftest_heap': {'en': 'Free heap: {kb} KB', 'ru': 'Свободно памяти: {kb} КБ'},
  'saved': {'en': 'Saved', 'ru': 'Сохранено'},
  'e2e_invite': {'en': 'E2E Invite', 'ru': 'E2E Приглашение'},
  'e2e_invite_hint': {'en': 'End-to-end encrypted contact with another node.', 'ru': 'Сквозное шифрование с другим узлом сети.'},
  'invite_accept_section': {'en': 'Accept invitation', 'ru': 'Принять приглашение'},
  'create_invite': {'en': 'Create invite', 'ru': 'Создать приглашение'},
  'invite_created': {'en': 'Invite created', 'ru': 'Приглашение создано'},
  'inviter_id': {'en': 'Inviter ID', 'ru': 'ID пригласившего'},
  'accept_invite': {'en': 'Accept invite', 'ru': 'Принять приглашение'},
  'invite_accepted': {'en': 'Invite accepted', 'ru': 'Приглашение принято'},
  'copy': {'en': 'Copy', 'ru': 'Копировать'},
  'copied': {'en': 'Copied', 'ru': 'Скопировано'},
  'paste': {'en': 'Paste', 'ru': 'Вставить'},
  'offline_pending': {'en': 'In queue', 'ru': 'В очереди'},
  'gps_enable': {'en': 'Enable GPS', 'ru': 'Включить GPS'},
  'gps_fix_yes': {'en': 'Fix acquired', 'ru': 'Фикс есть'},
  'gps_fix_no': {'en': 'No fix', 'ru': 'Нет фикса'},
  'map_my_location': {'en': 'My location', 'ru': 'Моё местоположение'},
  'map_waiting': {'en': 'Waiting for location events. Send yours from menu → Tools.', 'ru': 'Ожидание координат от узлов. Свою точку — меню → Инструменты → Отправить геолокацию.'},
  'play': {'en': 'Play', 'ru': 'Воспроизвести'},
  'wifi_connect': {'en': 'Connect to WiFi...', 'ru': 'Подключение к WiFi...'},
  'wifi_ota_section': {'en': 'Wi‑Fi & OTA', 'ru': 'Wi‑Fi и OTA'},
  'wifi_ota_section_hint': {'en': 'Home network: credentials are stored on the node. OTA: start AP mode on the device, then upload firmware from your PC (PlatformIO / ArduinoOTA).', 'ru': 'Домашняя сеть: SSID и пароль сохраняются на узле. OTA: команда переводит устройство в режим точки доступа — прошивка с ПК (PlatformIO / ArduinoOTA).'},
  'wifi_station_block': {'en': 'Home network', 'ru': 'Домашняя сеть'},
  'wifi_ota_upload_block': {'en': 'Firmware upload', 'ru': 'Загрузка прошивки'},
  'wifi_ssid': {'en': 'Network name (SSID)', 'ru': 'Имя сети (SSID)'},
  'wifi_password': {'en': 'Password', 'ru': 'Пароль'},
  'settings_device': {'en': 'Device', 'ru': 'Устройство'},
  'settings_node_id': {'en': 'Node ID', 'ru': 'ID узла'},
  'settings_node_id_hint': {'en': 'Short ID comes from BLE name; full 16 hex arrives after the device responds (info).', 'ru': 'Короткий ID — из имени BLE; полные 16 hex приходят после ответа устройства (info).'},
  'settings_region_hint': {'en': 'Must match your country’s regulations.', 'ru': 'Должен соответствовать регуляторике вашей страны.'},
  'settings_sf': {'en': 'Spreading factor (LoRa)', 'ru': 'Коэффициент расширения спектра (SF)'},
  'settings_sf_hint': {'en': 'Loaded from the node after info. SF7 — faster; SF12 — longer range.', 'ru': 'Подгружается с узла после info. SF7 — быстрее; SF12 — дальше.'},
  'settings_sf_legend': {'en': 'Lower SF — shorter air time · Higher SF — longer range', 'ru': 'Меньше SF — короче эфир · больше SF — дальность'},
  'invite_pubkey': {'en': 'Public key (Base64)', 'ru': 'Публичный ключ (Base64)'},
  'invite_channel_key': {'en': 'Channel key (optional, Base64)', 'ru': 'Ключ канала (необязательно, Base64)'},
  'gps_section': {'en': 'GPS', 'ru': 'GPS'},
  'connect': {'en': 'Connect', 'ru': 'Подключить'},
  'region_set': {'en': 'Region: {r}', 'ru': 'Регион: {r}'},
  'channel_set': {'en': 'Channel: {ch} ({freq} MHz)', 'ru': 'Канал: {ch} ({freq} MHz)'},
  'nickname_set': {'en': 'Nickname: {n}', 'ru': 'Никнейм: {n}'},
  'nickname_cleared': {'en': 'Nickname cleared', 'ru': 'Никнейм сброшен'},
  'mesh_topology': {'en': 'Mesh topology', 'ru': 'Mesh топология'},
  'mesh_tab_graph': {'en': 'Graph', 'ru': 'Схема'},
  'mesh_tab_list': {'en': 'List', 'ru': 'Список'},
  'mesh_legend_direct': {'en': 'Direct', 'ru': 'Прямая связь'},
  'mesh_legend_route': {'en': 'Route', 'ru': 'Маршрут'},
  'mesh_list_neighbors': {'en': 'Neighbors (direct)', 'ru': 'Соседи (прямая связь)'},
  'mesh_list_routes': {'en': 'Known routes', 'ru': 'Известные маршруты'},
  'mesh_empty': {'en': 'No data yet. Wait for discovery or refresh.', 'ru': 'Пока нет данных. Дождитесь discovery или обновите.'},
  'mesh_route_hops': {'en': 'hops', 'ru': 'хопов'},
  'mesh_col_next': {'en': 'Next hop', 'ru': 'След. хоп'},
  'ttl_title': {'en': 'Message TTL', 'ru': 'Время жизни сообщения'},
  'ttl_sheet_hint': {'en': 'After TTL expires, the message disappears for recipients.', 'ru': 'По истечении TTL сообщение исчезает у получателей.'},
  'ttl_none': {'en': '∞ No TTL', 'ru': '∞ Без TTL'},
  'voice_swipe_cancel': {'en': '← Swipe left to cancel', 'ru': '← Влево — отмена'},
  'switch_node': {'en': 'Switch node', 'ru': 'Сменить узел'},
  'forget_device': {'en': 'Forget device', 'ru': 'Забыть устройство'},
  'forget_device_confirm': {'en': 'Remove {name} from recent devices?', 'ru': 'Удалить {name} из недавних устройств?'},
  'reconnecting': {'en': 'Connection lost. Reconnecting... attempt {n}/3', 'ru': 'Потеря связи. Переподключение... попытка {n}/3'},
  'reconnect_ok': {'en': 'Reconnected successfully', 'ru': 'Переподключение успешно'},
  'reconnect_failed': {'en': 'Connection lost', 'ru': 'Связь потеряна'},
};

class AppLocalizations {
  final Locale locale;

  AppLocalizations(this.locale);

  String tr(String key, [Map<String, String>? params]) {
    final lang = locale.languageCode == 'ru' ? 'ru' : 'en';
    String s = _strings[key]?[lang] ?? _strings[key]?['en'] ?? key;
    if (params != null) {
      for (final e in params.entries) {
        s = s.replaceAll('{${e.key}}', e.value);
      }
    }
    return s;
  }

  static Locale _cachedLocale = const Locale('ru');

  static Locale get currentLocale => _cachedLocale;

  static Future<void> loadLocale() async {
    final prefs = await SharedPreferences.getInstance();
    final code = prefs.getString(_localeKey) ?? 'ru';
    _cachedLocale = Locale(code);
  }

  static Future<void> setLocale(String languageCode) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_localeKey, languageCode);
    _cachedLocale = Locale(languageCode);
  }

  static Future<void> switchLocale(VoidCallback onChanged) async {
    final next = _cachedLocale.languageCode == 'ru' ? 'en' : 'ru';
    await setLocale(next);
    onChanged();
  }
}

extension AppLocalizationsExtension on BuildContext {
  AppLocalizations get l10n => AppLocalizations(AppLocalizations.currentLocale);
}
