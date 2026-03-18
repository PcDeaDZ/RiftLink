/**
 * RiftLink PWA — EN / RU
 */
const LOCALE_KEY = 'riftlink_locale';

const strings = {
  en: {
    status_disconnected: 'Disconnected',
    status_connected: 'Connected',
    connect: 'Connect',
    reconnect: 'Reconnect',
    disconnect: 'Disconnect',
    theme: 'Theme',
    voice: 'Voice',
    record: 'Record',
    send_msg: 'Send message',
    msg_placeholder: 'Message text...',
    send: 'Send',
    broadcast: 'Broadcast',
    contact: 'Contact',
    contacts: 'Contacts',
    add_to_contacts: 'Add',
    node_id: 'Node ID (hex8)',
    nickname: 'Nickname',
    nickname_hint: 'Nickname',
    nickname_hint2: 'Up to 16 chars',
    set_nickname: 'Set',
    commands: 'Commands',
    info: 'Info',
    ping: 'Ping',
    selftest: 'Selftest',
    location: 'Location',
    ota: 'OTA',
    region: 'Region',
    channel: 'Channel',
    qr: 'QR & Invite',
    node_id_btn: 'Node ID',
    invite_btn: 'Invite',
    scan_qr: 'Scan QR',
    messages: 'Messages',
    history: 'Command history',
    events: 'Events (raw)',
    lang: 'Language',
    empty: '(empty)',
    delete: 'Delete',
    groups: 'Groups',
  },
  ru: {
    status_disconnected: 'Отключено',
    status_connected: 'Подключено',
    connect: 'Подключиться',
    reconnect: 'Подключиться снова',
    disconnect: 'Отключиться',
    theme: 'Тема',
    voice: 'Голос',
    record: 'Записать',
    send_msg: 'Отправить сообщение',
    msg_placeholder: 'Текст сообщения...',
    send: 'Отправить',
    broadcast: 'Broadcast',
    contact: 'Контакт',
    contacts: 'Контакты',
    add_to_contacts: 'Добавить',
    node_id: 'Node ID (hex8)',
    nickname: 'Никнейм',
    nickname_hint: 'Никнейм',
    nickname_hint2: 'До 16 символов',
    set_nickname: 'Установить',
    commands: 'Команды',
    info: 'Info',
    ping: 'Ping',
    selftest: 'Selftest',
    location: 'Локация',
    ota: 'OTA',
    region: 'Регион',
    channel: 'Канал',
    qr: 'QR и приглашение',
    node_id_btn: 'Node ID',
    invite_btn: 'Приглашение',
    scan_qr: 'Сканировать QR',
    messages: 'Сообщения',
    history: 'История команд',
    events: 'События (raw)',
    lang: 'Язык',
    empty: '(пусто)',
    delete: 'Удалить',
    groups: 'Группы',
  },
};

function getLocale() {
  return localStorage.getItem(LOCALE_KEY) || (navigator.language.startsWith('ru') ? 'ru' : 'en');
}

function setLocale(code) {
  localStorage.setItem(LOCALE_KEY, code);
  applyLocale();
}

function tr(key) {
  const loc = getLocale();
  return strings[loc]?.[key] || strings.en?.[key] || key;
}

function applyLocale() {
  document.querySelectorAll('[data-i18n]').forEach(el => {
    const key = el.getAttribute('data-i18n');
    if (key) el.textContent = tr(key);
  });
  document.querySelectorAll('[data-i18n-placeholder]').forEach(el => {
    const key = el.getAttribute('data-i18n-placeholder');
    if (key) el.placeholder = tr(key);
  });
  document.querySelectorAll('[data-i18n-title]').forEach(el => {
    const key = el.getAttribute('data-i18n-title');
    if (key) el.title = tr(key);
  });
}

function toggleLocale() {
  const next = getLocale() === 'ru' ? 'en' : 'ru';
  setLocale(next);
}
