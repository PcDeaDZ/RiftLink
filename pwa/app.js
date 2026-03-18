/**
 * RiftLink Test PWA — Web Bluetooth
 * Подключение к устройству RL-XXXXXXXX по BLE
 */

const SERVICE_UUID = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const TX_UUID = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
const RX_UUID = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';

let device = null;
let txChar = null;
let rxChar = null;
let nodeId = null;
let groups = [];
let neighbors = [];
let neighborsRssi = [];
let routes = [];

const HISTORY_KEY = 'riftlink_cmd_history';
const THEME_KEY = 'riftlink_theme';
const CONTACTS_KEY = 'riftlink_contacts';
const MAX_HISTORY = 20;

function loadContacts() {
  try {
    const raw = localStorage.getItem(CONTACTS_KEY);
    return raw ? JSON.parse(raw) : [];
  } catch (_) { return []; }
}

function saveContacts(arr) {
  localStorage.setItem(CONTACTS_KEY, JSON.stringify(arr));
}

function addContact(id, nickname) {
  const list = loadContacts();
  const idx = list.findIndex(c => c.id === id);
  const c = { id: id.toUpperCase().slice(0, 8), nickname: (nickname || '').trim() };
  if (idx >= 0) list[idx] = c; else list.push(c);
  saveContacts(list);
  renderContacts();
}

function removeContact(id) {
  saveContacts(loadContacts().filter(c => c.id !== id));
  renderContacts();
}

function renderContacts() {
  const listEl = document.getElementById('contactsList');
  const selectEl = document.getElementById('msgToSelect');
  if (!listEl || !selectEl) return;
  const contacts = loadContacts();
  listEl.innerHTML = contacts.map(c =>
    `<span class="contact-chip">${escapeHtml(c.nickname || c.id)} <button class="contact-rm" data-id="${escapeHtml(c.id)}" title="${tr('delete')}">×</button></span>`
  ).join(' ') || tr('empty');
  listEl.querySelectorAll('.contact-rm').forEach(btn => {
    btn.addEventListener('click', () => removeContact(btn.dataset.id));
  });
  const opts = selectEl.querySelectorAll('option');
  for (let i = opts.length - 1; i > 0; i--) opts[i].remove();
  contacts.forEach(c => {
    const opt = document.createElement('option');
    opt.value = c.id;
    opt.textContent = c.nickname ? `${c.nickname} (${c.id})` : c.id;
    selectEl.appendChild(opt);
  });
}

function renderGroupsSelect() {
  const sel = document.getElementById('msgGroupSelect');
  if (!sel) return;
  const opts = sel.querySelectorAll('option');
  for (let i = opts.length - 1; i > 0; i--) opts[i].remove();
  groups.forEach(gid => {
    const opt = document.createElement('option');
    opt.value = gid;
    opt.textContent = `Группа ${gid}`;
    sel.appendChild(opt);
  });
}

function renderMesh() {
  const el = document.getElementById('meshGraph');
  if (!el) return;
  const items = [];
  items.push(`<strong>${nodeId || '—'} (я)</strong>`);
  neighbors.forEach((id, i) => {
    const rssi = neighborsRssi[i];
    items.push(`↔ ${id}${rssi ? ` (${rssi})` : ''}`);
  });
  routes.forEach(r => {
    const dest = r.dest || '?';
    const nh = r.nextHop || '?';
    const hops = r.hops != null ? r.hops : '?';
    const rssi = r.rssi != null ? r.rssi : '';
    items.push(`→ ${dest} via ${nh} ${hops}h${rssi ? ` (${rssi})` : ''}`);
  });
  el.innerHTML = items.join('<br>') || tr('empty');
}

function renderGroupsList() {
  const listEl = document.getElementById('groupsList');
  if (!listEl) return;
  listEl.innerHTML = groups.map(gid =>
    `<span class="contact-chip">Группа ${gid} <button class="contact-rm" data-gid="${gid}" title="${tr('delete')}">×</button></span>`
  ).join(' ') || tr('empty');
  listEl.querySelectorAll('.contact-rm').forEach(btn => {
    btn.addEventListener('click', () => {
      const gid = parseInt(btn.dataset.gid, 10);
      sendCmd({ cmd: 'removeGroup', group: gid });
    });
  });
}

const voiceBuffers = {};
function voiceChunksReceived(obj) {
  const key = obj.from || 'unknown';
  if (!voiceBuffers[key]) voiceBuffers[key] = {};
  voiceBuffers[key][obj.chunk] = obj.data;
  const total = obj.total || 1;
  const chunks = voiceBuffers[key];
  if (Object.keys(chunks).length >= total) {
    let b64 = '';
    for (let i = 0; i < total; i++) b64 += (chunks[i] || '');
    delete voiceBuffers[key];
    try {
      const binary = atob(b64);
      const bytes = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
      const blob = new Blob([bytes], { type: bytes[0] === 0x4f && bytes[1] === 0x67 ? 'audio/ogg' : 'audio/webm' });
      const url = URL.createObjectURL(blob);
      const audio = new Audio(url);
      audio.onended = () => URL.revokeObjectURL(url);
      audio.play().catch(() => addEvent('Воспроизведение голоса не удалось', 'error'));
      addMessage(obj.from, '🎤 Голосовое сообщение', true);
    } catch (e) {
      addEvent('Голос: ' + e.message, 'error');
    }
  }
}

const statusEl = document.getElementById('status');
const btnConnect = document.getElementById('btnConnect');
const btnDisconnect = document.getElementById('btnDisconnect');
const btnSend = document.getElementById('btnSend');
const msgText = document.getElementById('msgText');
const msgTo = document.getElementById('msgTo');
const messagesEl = document.getElementById('messages');
const eventsEl = document.getElementById('events');

function setConnected(connected) {
  statusEl.textContent = connected ? tr('status_connected') : tr('status_disconnected');
  statusEl.classList.toggle('connected', connected);
  btnConnect.disabled = connected;
  btnDisconnect.disabled = !connected;
  btnSend.disabled = !connected;
  document.querySelectorAll('.btn.small, #btnRegion, #btnNickname, #btnLocation, #btnChannel, #btnShowQr, #btnInviteQr, #btnVoiceRecord').forEach(b => b && (b.disabled = !connected));
  if (connected) updateShowQrButton();
}

function initTheme() {
  const saved = localStorage.getItem(THEME_KEY) || 'dark';
  document.documentElement.setAttribute('data-theme', saved);
  const btn = document.getElementById('btnTheme');
  if (btn) btn.textContent = saved === 'light' ? '🌙' : '☀️';
}

function toggleTheme() {
  const cur = document.documentElement.getAttribute('data-theme') || 'dark';
  const next = cur === 'dark' ? 'light' : 'dark';
  document.documentElement.setAttribute('data-theme', next);
  localStorage.setItem(THEME_KEY, next);
  document.getElementById('btnTheme').textContent = next === 'light' ? '🌙' : '☀️';
}

function saveToHistory(cmd) {
  try {
    const hist = JSON.parse(localStorage.getItem(HISTORY_KEY) || '[]');
    const json = typeof cmd === 'string' ? cmd : JSON.stringify(cmd);
    const filtered = hist.filter(h => h !== json);
    filtered.unshift(json);
    localStorage.setItem(HISTORY_KEY, JSON.stringify(filtered.slice(0, MAX_HISTORY)));
    renderHistory();
  } catch (_) {}
}

function renderHistory() {
  const el = document.getElementById('history');
  if (!el) return;
  try {
    const hist = JSON.parse(localStorage.getItem(HISTORY_KEY) || '[]');
    el.innerHTML = hist.map(h => `<div class="hist-item" data-cmd="${escapeHtml(h)}">${escapeHtml(h.length > 60 ? h.slice(0, 57) + '...' : h)}</div>`).join('');
    el.querySelectorAll('.hist-item').forEach(item => {
      item.addEventListener('click', () => {
        const cmd = item.dataset.cmd;
        try {
          const obj = JSON.parse(cmd);
          if (obj.cmd) sendCmd(obj);
        } catch (_) {}
      });
    });
  } catch (_) {}
}

function updateShowQrButton() {
  const btn = document.getElementById('btnShowQr');
  const btnInv = document.getElementById('btnInviteQr');
  if (btn) btn.disabled = !nodeId;
  if (btnInv) btnInv.disabled = !nodeId;
}

function showQr() {
  if (!nodeId || typeof QRCode === 'undefined') return;
  const container = document.getElementById('qrContainer');
  const canvas = document.getElementById('qrCanvas');
  const nodeEl = document.getElementById('qrNodeId');
  if (!container || !canvas) return;
  container.style.display = 'block';
  nodeEl.textContent = nodeId;
  QRCode.toCanvas(canvas, 'riftlink:' + nodeId, { width: 160, margin: 1 }, () => {});
}

function showInviteQr(id, pubKey) {
  if (!id || !pubKey || typeof QRCode === 'undefined') return;
  const container = document.getElementById('qrContainer');
  const canvas = document.getElementById('qrCanvas');
  const nodeEl = document.getElementById('qrNodeId');
  if (!container || !canvas) return;
  container.style.display = 'block';
  nodeEl.textContent = 'Приглашение: ' + id;
  const data = 'riftlink:' + id + ':' + pubKey;
  QRCode.toCanvas(canvas, data, { width: 200, margin: 1 }, () => {});
}

function hideQr() {
  const container = document.getElementById('qrContainer');
  if (container) container.style.display = 'none';
}

function scanQrFromFile(file) {
  if (typeof jsQR === 'undefined') return;
  const img = new Image();
  img.onload = () => {
    const canvas = document.createElement('canvas');
    canvas.width = img.width;
    canvas.height = img.height;
    const ctx = canvas.getContext('2d');
    ctx.drawImage(img, 0, 0);
    const data = ctx.getImageData(0, 0, canvas.width, canvas.height);
    const code = jsQR(data.data, data.width, data.height);
    if (code && code.data) {
      const raw = code.data.replace(/^riftlink:/i, '').trim();
      const parts = raw.split(':');
      if (parts.length >= 2 && /^[0-9A-Fa-f]{8}$/.test(parts[0]) && parts[1].length > 40) {
        const id = parts[0].toUpperCase();
        const pubKey = parts[1];
        sendCmd({ cmd: 'acceptInvite', id, pubKey });
        addEvent('Приглашение принято: ' + id, '');
      } else if (/^[0-9A-Fa-f]{8}$/.test(raw)) {
        msgTo.value = raw.toUpperCase();
        addEvent('QR: ' + raw, '');
      } else {
        addEvent('QR: ' + raw, '');
      }
    } else {
      addEvent('QR не найден на изображении', 'error');
    }
  };
  img.src = URL.createObjectURL(file);
}

function addEvent(text, type = '') {
  const div = document.createElement('div');
  div.className = 'evt' + (type ? ' ' + type : '');
  div.textContent = new Date().toLocaleTimeString() + ' ' + text;
  eventsEl.insertBefore(div, eventsEl.firstChild);
  while (eventsEl.children.length > 50) eventsEl.removeChild(eventsEl.lastChild);
}

function addMessage(from, text, incoming = true) {
  const div = document.createElement('div');
  div.className = 'msg' + (incoming ? ' incoming' : '');
  div.innerHTML = `<span class="from">${from || '—'}</span> ${escapeHtml(text)} <span class="time">${new Date().toLocaleTimeString()}</span>`;
  messagesEl.insertBefore(div, messagesEl.firstChild);
  while (messagesEl.children.length > 100) messagesEl.removeChild(messagesEl.lastChild);
}

function escapeHtml(s) {
  const d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}

async function ensureConnected() {
  if (!device) return false;
  if (device.gatt?.connected && txChar) return true;
  try {
    const server = await device.gatt.connect();
    if (!server.connected) return false;
    const service = await server.getPrimaryService(SERVICE_UUID);
    txChar = await service.getCharacteristic(TX_UUID);
    rxChar = await service.getCharacteristic(RX_UUID);
    await rxChar.startNotifications();
    rxChar.addEventListener('characteristicvaluechanged', (e) => {
      handleEvent(new TextDecoder().decode(e.target.value));
    });
    return true;
  } catch (e) {
    if (e?.message?.includes('disconnect') || e?.message?.includes('GATT')) handleDisconnect();
    return false;
  }
}

async function sendCmd(cmd) {
  if (!device) return false;
  const ok = await ensureConnected();
  if (!ok) return false;
  try {
    const json = JSON.stringify(cmd);
    await txChar.writeValue(new TextEncoder().encode(json));
    addEvent('→ ' + json, '');
    saveToHistory(cmd);
    return true;
  } catch (e) {
    addEvent(formatBleError(e), 'error');
    if (e?.message?.includes('disconnect') || e?.message?.includes('GATT')) handleDisconnect();
    return false;
  }
}

function handleEvent(data) {
  try {
    const obj = JSON.parse(data);
    const evt = obj.evt;
    addEvent('← ' + data, evt === 'msg' ? 'msg' : '');

    if (evt === 'msg') {
      addMessage(obj.from, obj.text || '', true);
    } else if (evt === 'info') {
      const sfStr = obj.sf != null ? ` SF${obj.sf}` : '';
      addEvent(`Info: id=${obj.id} region=${obj.region} freq=${obj.freq}${sfStr}`, '');
      if (obj.id) { nodeId = obj.id; updateShowQrButton(); }
      if (Array.isArray(obj.groups)) { groups = obj.groups; renderGroupsSelect(); renderGroupsList(); }
      if (Array.isArray(obj.neighbors)) { neighbors = obj.neighbors; neighborsRssi = obj.neighborsRssi || []; renderMesh(); }
      if (Array.isArray(obj.routes)) { routes = obj.routes; renderMesh(); }
    } else if (evt === 'routes') {
      if (Array.isArray(obj.routes)) { routes = obj.routes; renderMesh(); }
    } else if (evt === 'groups') {
      if (Array.isArray(obj.groups)) { groups = obj.groups; renderGroupsSelect(); renderGroupsList(); }
    } else if (evt === 'invite') {
      if (obj.id && obj.pubKey) showInviteQr(obj.id, obj.pubKey);
      if (obj.nickname) {
        const nickEl = document.getElementById('nickname');
        if (nickEl) nickEl.placeholder = 'Ник: ' + obj.nickname;
      }
    } else if (evt === 'pong') {
      addEvent(`Pong от ${obj.from}${obj.rssi ? ` (RSSI ${obj.rssi} dBm)` : ''}`, '');
    } else if (evt === 'selftest') {
      addEvent(`Selftest: radio=${obj.radioOk} display=${obj.displayOk} bat=${obj.batteryMv}mV heap=${obj.heapFree}`, '');
    } else if (evt === 'ota') {
      addEvent(`OTA: ${obj.ssid} / ${obj.password} @ ${obj.ip}`, '');
    } else if (evt === 'delivered') {
      addEvent(`Доставлено: ${obj.from} msgId=${obj.msgId}${obj.rssi ? ` RSSI ${obj.rssi}` : ''}`, '');
    } else if (evt === 'read') {
      addEvent(`Прочитано: ${obj.from} msgId=${obj.msgId}${obj.rssi ? ` RSSI ${obj.rssi}` : ''}`, '');
    } else if (evt === 'location') {
      const rssiSuffix = obj.rssi ? ` [${obj.rssi} dBm]` : '';
      addMessage(obj.from, `📍 ${(obj.lat || 0).toFixed(5)}, ${(obj.lon || 0).toFixed(5)}${rssiSuffix}`, true);
    } else if (evt === 'error') {
      const code = obj.code || 'unknown';
      const msg = obj.msg || 'Ошибка устройства';
      addEvent(`Ошибка [${code}]: ${msg}`, 'error');
    } else if (evt === 'voice') {
      voiceChunksReceived(obj);
    }
  } catch (e) {
    addEvent('Parse error: ' + data, 'error');
  }
}

function getBleSupportMessage() {
  if (navigator.bluetooth) return null;
  const ua = navigator.userAgent;
  const isIOS = /iPad|iPhone|iPod/.test(ua) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
  const isAndroid = /Android/.test(ua);
  if (isIOS) return 'Web Bluetooth не поддерживается в Safari на iOS. Используйте Android (Chrome, Яндекс.Браузер, Samsung Internet) или ПК (Chrome, Edge).';
  if (isAndroid) return 'Установите Chrome, Яндекс.Браузер или Samsung Internet для работы с Bluetooth.';
  return 'Web Bluetooth не поддерживается. Используйте Chrome, Edge или Яндекс.Браузер (в т.ч. на Android).';
}

function initBleNotice() {
  const el = document.getElementById('bleNotice');
  if (!el) return;
  const msg = getBleSupportMessage();
  if (msg) {
    el.textContent = msg;
    el.style.display = 'block';
  }
}

async function connect() {
  if (!navigator.bluetooth) {
    addEvent(getBleSupportMessage() || 'Web Bluetooth не поддерживается.', 'error');
    return;
  }

  try {
    addEvent('Поиск устройства...', '');
    try {
      device = await navigator.bluetooth.requestDevice({
        filters: [
          { namePrefix: 'RL-' },
          { services: [SERVICE_UUID] }
        ],
        optionalServices: [SERVICE_UUID]
      });
    } catch (e) {
      if (e?.name === 'NotFoundError') {
        addEvent('RiftLink не найден по фильтру. Показываю все устройства...', '');
        device = await navigator.bluetooth.requestDevice({
          acceptAllDevices: true,
          optionalServices: [SERVICE_UUID]
        });
      } else throw e;
    }

    addEvent('Подключение к ' + (device.name || 'устройство') + '...', '');
    const server = await device.gatt.connect();

    const service = await server.getPrimaryService(SERVICE_UUID);
    txChar = await service.getCharacteristic(TX_UUID);
    rxChar = await service.getCharacteristic(RX_UUID);

    await rxChar.startNotifications();
    rxChar.addEventListener('characteristicvaluechanged', (e) => {
      const val = e.target.value;
      const str = new TextDecoder().decode(val);
      handleEvent(str);
    });

    setConnected(true);
    addEvent('Подключено', '');

    device.addEventListener('gattserverdisconnected', handleDisconnect);
  } catch (e) {
    device = null;
    txChar = null;
    rxChar = null;
    addEvent(formatBleError(e), 'error');
  }
}

function handleDisconnect() {
  txChar = null;
  rxChar = null;
  setConnected(false);
  updateConnectButton();
  addEvent('Устройство отключилось. Нажмите «Подключиться снова».', 'error');
}

function updateConnectButton() {
  if (!btnConnect) return;
  btnConnect.textContent = device ? tr('reconnect') || 'Подключиться снова' : tr('connect') || 'Подключиться';
}

async function reconnect() {
  if (!device) return connect();
  try {
    addEvent('Переподключение...', '');
    const server = await device.gatt.connect();
    if (!server.connected) throw new Error('GATT disconnected');
    const service = await server.getPrimaryService(SERVICE_UUID);
    txChar = await service.getCharacteristic(TX_UUID);
    rxChar = await service.getCharacteristic(RX_UUID);
    await rxChar.startNotifications();
    rxChar.addEventListener('characteristicvaluechanged', (e) => {
      const val = e.target.value;
      handleEvent(new TextDecoder().decode(val));
    });
    setConnected(true);
    addEvent('Подключено', '');
    device.addEventListener('gattserverdisconnected', handleDisconnect);
  } catch (e) {
    if (e?.message?.includes('disconnect') || e?.message?.includes('GATT') || e?.name === 'NotFoundError') {
      device = null;
    }
    updateConnectButton();
    addEvent(formatBleError(e), 'error');
  }
}

function formatBleError(e) {
  const name = (e.name || '').toLowerCase();
  const msg = (e.message || '').toLowerCase();
  if (name === 'notfounderror') return 'Устройство не найдено. Убедитесь, что RiftLink включён и в радиусе.';
  if (name === 'securityerror') return 'Нет разрешения на Bluetooth. Проверьте настройки браузера.';
  if (name === 'networkerror') return 'Сеть Bluetooth недоступна. Включите Bluetooth.';
  if (msg.includes('timeout') || msg.includes('timed out')) return 'Таймаут — устройство не ответило. Подойдите ближе.';
  if (msg.includes('disconnect') || msg.includes('gatt')) return 'Устройство отключилось. Подключитесь снова.';
  return 'Ошибка: ' + (e.message || e.toString());
}

async function disconnect() {
  if (device?.gatt?.connected) device.gatt.disconnect();
  device = null;
  txChar = null;
  rxChar = null;
  setConnected(false);
  updateConnectButton();
  addEvent('Отключено', '');
}

async function sendMessage() {
  const text = msgText.value.trim();
  if (!text) return;

  const groupSel = document.getElementById('msgGroupSelect');
  const groupId = groupSel && groupSel.value ? parseInt(groupSel.value, 10) : 0;

  let to = msgTo.value.trim().toUpperCase();
  const sel = document.getElementById('msgToSelect');
  if (sel && sel.value) to = sel.value;
  const isUnicast = !groupId && to.length >= 8 && /^[0-9A-F]+$/.test(to);

  let cmd;
  if (groupId > 0) {
    cmd = { cmd: 'send', group: groupId, text };
  } else if (isUnicast) {
    cmd = { cmd: 'send', to: to.slice(0, 8), text };
  } else {
    cmd = { cmd: 'send', text };
  }

  const ok = await sendCmd(cmd);
  if (ok) {
    const target = groupId > 0 ? `Группа ${groupId}` : (isUnicast ? to : 'Broadcast');
    addMessage(target, text, false);
    msgText.value = '';
  }
}

initBleNotice();
btnConnect.addEventListener('click', () => (device ? reconnect() : connect()));
btnDisconnect.addEventListener('click', disconnect);
btnSend.addEventListener('click', sendMessage);

msgText.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') sendMessage();
});

document.getElementById('btnInfo').addEventListener('click', () => sendCmd({ cmd: 'info' }));
document.getElementById('btnRoutes').addEventListener('click', () => sendCmd({ cmd: 'routes' }));
document.getElementById('btnPing').addEventListener('click', async () => {
  const to = msgTo.value.trim() || prompt('Node ID (hex8):', '');
  if (to && to.length >= 8) sendCmd({ cmd: 'ping', to: to.slice(0, 8) });
});
document.getElementById('btnSelftest').addEventListener('click', () => sendCmd({ cmd: 'selftest' }));
document.getElementById('btnOta').addEventListener('click', () => sendCmd({ cmd: 'ota' }));

document.getElementById('btnRegion').addEventListener('click', () => {
  const r = document.getElementById('region').value;
  sendCmd({ cmd: 'region', region: r });
});

document.getElementById('btnNickname').addEventListener('click', () => {
  const nick = document.getElementById('nickname').value.trim();
  sendCmd({ cmd: 'nickname', nickname: nick });
  if (nick) addEvent('Никнейм: ' + nick, '');
});

document.getElementById('btnChannel').addEventListener('click', () => {
  const ch = parseInt(document.getElementById('channel').value, 10);
  sendCmd({ cmd: 'channel', channel: ch });
  addEvent('Канал: ' + ch, '');
});

document.getElementById('btnTheme').addEventListener('click', toggleTheme);
const btnLang = document.getElementById('btnLang');
if (btnLang) btnLang.addEventListener('click', () => {
  toggleLocale();
  addEvent(tr('lang') + ': ' + (getLocale() === 'ru' ? 'Русский' : 'English'), '');
});
document.getElementById('btnAddGroup').addEventListener('click', async () => {
  const inp = document.getElementById('groupId');
  const gid = parseInt(inp?.value, 10);
  if (!gid || gid <= 0) {
    addEvent('Введите ID группы (1–4294967295)', 'error');
    return;
  }
  const ok = await sendCmd({ cmd: 'addGroup', group: gid });
  if (ok) { inp.value = ''; addEvent('Группа ' + gid + ' добавлена', ''); }
});

document.getElementById('btnAddContact').addEventListener('click', () => {
  const id = document.getElementById('contactId').value.trim().toUpperCase();
  const nick = document.getElementById('contactNick').value.trim();
  if (id.length !== 8 || !/^[0-9A-F]+$/.test(id)) {
    addEvent('Введите 8 hex-символов (A1B2C3D4)', 'error');
    return;
  }
  addContact(id, nick);
  document.getElementById('contactId').value = '';
  document.getElementById('contactNick').value = '';
  addEvent('Контакт добавлен: ' + (nick || id), '');
});
document.getElementById('msgToSelect').addEventListener('change', function() {
  if (this.value) document.getElementById('msgTo').value = this.value;
});

applyLocale();
initTheme();
renderHistory();
renderContacts();
renderGroupsList();

document.getElementById('btnShowQr').addEventListener('click', () => {
  const container = document.getElementById('qrContainer');
  if (container.style.display === 'none') showQr();
});
document.getElementById('btnInviteQr').addEventListener('click', () => sendCmd({ cmd: 'invite' }));

document.getElementById('btnScanQr').addEventListener('click', () => {
  document.getElementById('qrFile').click();
});

document.getElementById('qrFile').addEventListener('change', (e) => {
  const f = e.target.files?.[0];
  if (f) scanQrFromFile(f);
  e.target.value = '';
});

// Голос: запись и отправка
let voiceRecorder = null;
let voiceChunks = [];

document.getElementById('btnVoiceRecord').addEventListener('click', async () => {
  const btn = document.getElementById('btnVoiceRecord');
  const statusEl = document.getElementById('voiceStatus');
  if (voiceRecorder && voiceRecorder.state === 'recording') {
    voiceRecorder.stop();
    btn.textContent = '🎤 Записать';
    statusEl.textContent = 'Обработка...';
    return;
  }
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
    voiceRecorder = new MediaRecorder(stream, { mimeType: 'audio/webm;codecs=opus', audioBitsPerSecond: 8000 });
    voiceChunks = [];
    voiceRecorder.ondataavailable = (e) => { if (e.data.size > 0) voiceChunks.push(e.data); };
    voiceRecorder.onstop = async () => {
      stream.getTracks().forEach(t => t.stop());
      const blob = new Blob(voiceChunks, { type: 'audio/webm' });
      const buf = await blob.arrayBuffer();
      const bytes = new Uint8Array(buf);
      if (bytes.length > 30720) {
        addEvent('Голос слишком длинный (макс 30 KB)', 'error');
        statusEl.textContent = '';
        return;
      }
      const to = msgTo.value.trim().toUpperCase();
      if (!to || to.length < 8) {
        addEvent('Укажите Node ID для голоса', 'error');
        statusEl.textContent = '';
        return;
      }
      const b64 = btoa(String.fromCharCode.apply(null, bytes));
      const chunkSize = 200;
      const total = Math.ceil(b64.length / chunkSize);
      for (let i = 0; i < total; i++) {
        const chunk = b64.slice(i * chunkSize, (i + 1) * chunkSize);
        await sendCmd({ cmd: 'voice', to: to.slice(0, 8), chunk: i, total, data: chunk });
      }
      addEvent('Голос отправлен', '');
      statusEl.textContent = '';
    };
    voiceRecorder.start();
    btn.textContent = '⏹ Стоп';
    statusEl.textContent = 'Запись...';
  } catch (e) {
    addEvent('Микрофон: ' + e.message, 'error');
    statusEl.textContent = '';
  }
});

document.getElementById('btnLocation').addEventListener('click', async () => {
  if (!navigator.geolocation) {
    addEvent('Геолокация не поддерживается', 'error');
    return;
  }
  addEvent('Получение координат...', '');
  navigator.geolocation.getCurrentPosition(
    async (pos) => {
      const cmd = { cmd: 'location', lat: pos.coords.latitude, lon: pos.coords.longitude, alt: Math.round(pos.coords.altitude || 0) };
      const ok = await sendCmd(cmd);
      if (ok) addMessage('Я', `📍 ${pos.coords.latitude.toFixed(5)}, ${pos.coords.longitude.toFixed(5)}`, false);
    },
    (err) => {
      let msg = err.message || '';
      if (err.code === 1) msg = 'Доступ к геолокации запрещён';
      else if (err.code === 2) msg = 'Позиция недоступна';
      else if (err.code === 3) msg = 'Таймаут геолокации — выйдите на открытое место';
      addEvent('Геолокация: ' + msg, 'error');
    },
    { enableHighAccuracy: true, timeout: 10000 }
  );
});
