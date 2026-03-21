/**
 * RiftLink App — mesh-мессенджер (PWA)
 */

const SERVICE_UUID = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const TX_UUID = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
const RX_UUID = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';
const CONTACTS_KEY = 'riftlink_contacts';

let device = null;
let txChar = null;
let rxChar = null;
let nodeId = '';
let nickname = '';
let groups = [];
let neighbors = [];
let neighborsRssi = [];
let routes = [];
let locations = {};
let voiceBuffers = {};
let currentRegion = '';
let currentChannel = -1;
let powersaveEnabled = false;
let batteryMv = 0;
let heapKb = 0;
let offlinePending = 0;
let telemetry = {};  // from -> { battery, heapKb }
let gpsPresent = false;
let gpsEnabled = false;
let gpsFix = false;
let intentionalDisconnect = false;
let reconnectingPromise = null;

function normalizeNodeId(id) {
  return String(id || '').replace(/[^0-9A-Fa-f]/g, '').toUpperCase();
}

function isFullNodeId(id) {
  return /^[0-9A-F]{16}$/.test(normalizeNodeId(id));
}

function loadContacts() {
  try {
    const raw = JSON.parse(localStorage.getItem(CONTACTS_KEY) || '[]');
    if (!Array.isArray(raw)) return [];
    return raw
      .map(c => ({ id: normalizeNodeId(c?.id), nickname: String(c?.nickname || '') }))
      .filter(c => c.id.length === 16 || c.id.length === 8);
  } catch (_) { return []; }
}

function getContactName(id) {
  const fullId = normalizeNodeId(id);
  const contacts = loadContacts();
  const exact = contacts.find(x => x.id === fullId);
  if (exact?.nickname) return exact.nickname;
  const legacy = contacts.find(x => x.id.length === 8 && fullId.startsWith(x.id));
  return legacy?.nickname || fullId;
}

function saveContact(id, nickname) {
  const fullId = normalizeNodeId(id);
  if (!isFullNodeId(fullId)) return false;
  const contacts = loadContacts();
  const idx = contacts.findIndex(x => x.id === fullId);
  if (idx >= 0) contacts[idx].nickname = nickname;
  else contacts.push({ id: fullId, nickname });
  localStorage.setItem(CONTACTS_KEY, JSON.stringify(contacts));
  return true;
}

function removeContact(id) {
  const fullId = normalizeNodeId(id);
  const contacts = loadContacts().filter(x => x.id !== fullId);
  localStorage.setItem(CONTACTS_KEY, JSON.stringify(contacts));
}

function migrateLegacyContactsWithKnownIds(knownIds) {
  const contacts = loadContacts();
  let changed = false;
  const normalizedKnown = (knownIds || []).map(normalizeNodeId).filter(isFullNodeId);
  for (const c of contacts) {
    if (c.id.length !== 8) continue;
    const matches = normalizedKnown.filter(id => id.startsWith(c.id));
    if (matches.length === 1) {
      c.id = matches[0];
      changed = true;
    }
  }
  if (changed) {
    localStorage.setItem(CONTACTS_KEY, JSON.stringify(contacts));
  }
}

function addRxListener() {
  if (!rxChar) return;
  rxChar.addEventListener('characteristicvaluechanged', (e) => {
    handleEvent(new TextDecoder().decode(e.target.value));
  });
}

async function ensureConnected() {
  if (!device) return false;
  if (device.gatt?.connected && txChar) return true;
  if (reconnectingPromise) return reconnectingPromise;
  reconnectingPromise = (async () => {
    try {
      for (let attempt = 1; attempt <= 2; attempt++) {
        try {
          const server = await device.gatt.connect();
          if (!server.connected) continue;
          const service = await server.getPrimaryService(SERVICE_UUID);
          txChar = await service.getCharacteristic(TX_UUID);
          rxChar = await service.getCharacteristic(RX_UUID);
          await rxChar.startNotifications();
          addRxListener();
          return true;
        } catch (e) {
          if (attempt === 2 || e?.message?.includes('disconnect') || e?.message?.includes('GATT')) {
            if (e?.message?.includes('disconnect') || e?.message?.includes('GATT')) handleDisconnect();
            return false;
          }
          await new Promise(r => setTimeout(r, 400));
        }
      }
      return false;
    } finally {
      reconnectingPromise = null;
    }
  })();
  return reconnectingPromise;
}

function sendCmd(cmd) {
  if (!device) return Promise.resolve(false);
  return ensureConnected()
    .then((ok) => ok ? txChar.writeValue(new TextEncoder().encode(JSON.stringify(cmd))) : Promise.reject())
    .then(() => true)
    .catch((e) => {
      if (e?.message?.includes('disconnect') || e?.message?.includes('GATT')) handleDisconnect();
      return false;
    });
}

function showToast(msg, type = '') {
  const el = document.createElement('div');
  el.className = 'toast' + (type ? ' ' + type : '');
  el.textContent = msg;
  document.body.appendChild(el);
  setTimeout(() => el.remove(), 3000);
}

function escapeHtml(s) {
  const d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}

function addMessage(from, text, incoming = true, msgId = null, recipient = null) {
  const div = document.createElement('div');
  div.className = 'msg' + (incoming ? ' incoming' : ' outgoing');
  const name = incoming ? getContactName(from) : (from || 'Я');
  const statusSpan = incoming ? '' : '<span class="status" data-status=""></span>';
  div.innerHTML = `<span class="from">${escapeHtml(name)}</span><br>${escapeHtml(text)}<span class="time">${new Date().toLocaleTimeString()}</span>${statusSpan}`;
  if (msgId) div.dataset.msgid = String(msgId);
  if (recipient) div.dataset.recipient = recipient;
  document.getElementById('messages').insertBefore(div, document.getElementById('messages').firstChild);
  while (document.getElementById('messages').children.length > 100) {
    document.getElementById('messages').removeChild(document.getElementById('messages').lastChild);
  }
}

function updateMessageStatus(msgId, status) {
  const el = document.querySelector(`.msg.outgoing[data-msgid="${msgId}"] .status`);
  if (el) el.textContent = status;
}

function findLastOutgoingWithoutMsgId(recipient) {
  const msgs = document.querySelectorAll('.msg.outgoing');
  for (const m of msgs) {
    if (m.dataset.recipient === recipient && !m.dataset.msgid) return m;
  }
  return null;
}

function renderRecipient() {
  const sel = document.getElementById('recipient');
  if (!sel) return;
  const opts = sel.querySelectorAll('option');
  for (let i = opts.length - 1; i >= 0; i--) opts[i].remove();
  const opt0 = document.createElement('option');
  opt0.value = '';
  opt0.textContent = 'Broadcast';
  sel.appendChild(opt0);
  groups.forEach(gid => {
    const o = document.createElement('option');
    o.value = 'g' + gid;
    o.textContent = `Группа ${gid}`;
    sel.appendChild(o);
  });
  neighbors.forEach((id, i) => {
    const o = document.createElement('option');
    o.value = id;
    o.textContent = getContactName(id) + (neighborsRssi[i] ? ` (${neighborsRssi[i]})` : '');
    sel.appendChild(o);
  });
  updateMsgInputLimit();
}

function updateMsgInputLimit() {
  const r = document.getElementById('recipient')?.value || '';
  const inp = document.getElementById('msgInput');
  if (inp) inp.maxLength = r.startsWith('g') ? 160 : 200;
}

function renderNeighbors() {
  const el = document.getElementById('neighborsList');
  if (!el) return;
  if (neighbors.length === 0) {
    el.innerHTML = '<span class="empty">Нет соседей</span>';
    return;
  }
  el.innerHTML = neighbors.map((id, i) => {
    const rssi = neighborsRssi[i];
    const nid = normalizeNodeId(id);
    const hasContact = loadContacts().some(c => c.id === nid);
    const telem = telemetry[nid];
    const batStr = telem?.battery ? ` ${(telem.battery / 1000).toFixed(1)}V` : '';
    return `<span class="chip">${getContactName(id)}${batStr} ${rssi ? `(${rssi})` : ''} <button class="chip-ping" data-ping="${nid}">Ping</button>${hasContact ? '' : `<button class="chip-add" data-add="${nid}" title="В контакты">+</button>`}</span>`;
  }).join('');
  el.querySelectorAll('.chip-ping').forEach(btn => {
    btn.addEventListener('click', () => doPing(btn.dataset.ping, btn));
  });
  el.querySelectorAll('.chip-add').forEach(btn => {
    btn.addEventListener('click', () => {
      const nid = btn.dataset.add;
      saveContact(nid, nid);
      renderContacts();
      renderRecipient();
      renderNeighbors();
      showToast('Добавлено в контакты');
    });
  });
}

function renderContacts() {
  const el = document.getElementById('contactsList');
  if (!el) return;
  const contacts = loadContacts();
  el.innerHTML = contacts.length === 0
    ? '<span class="empty">Нет контактов</span>'
    : contacts.map(c =>
        `<div class="contact-item">${escapeHtml(c.nickname || c.id)} (${c.id}) <button class="contact-rm" data-id="${c.id}">×</button></div>`
      ).join('');
  el.querySelectorAll('.contact-rm').forEach(btn => {
    btn.addEventListener('click', () => {
      removeContact(btn.dataset.id);
      renderContacts();
      renderRecipient();
      renderNeighbors();
    });
  });
}

function renderGroups() {
  const el = document.getElementById('groupsList');
  if (!el) return;
  el.innerHTML = groups.length === 0
    ? '<span class="empty">Нет групп</span>'
    : groups.map(gid =>
        `<span class="chip">Группа ${gid} <button class="chip-rm" data-gid="${gid}">×</button></span>`
      ).join('');
  el.querySelectorAll('.chip-rm').forEach(btn => {
    btn.addEventListener('click', () => {
      sendCmd({ cmd: 'removeGroup', group: parseInt(btn.dataset.gid, 10) });
    });
  });
}

function renderRoutes() {
  const el = document.getElementById('routesList');
  if (!el) return;
  el.innerHTML = routes.length === 0
    ? '<span class="empty">Нет маршрутов</span>'
    : routes.map(r => `<div>${r.dest || '?'} via ${r.nextHop || '?'} ${r.hops || 0}h</div>`).join('');
}

function renderMap() {
  const el = document.getElementById('mapContainer');
  if (!el) return;
  const entries = Object.entries(locations);
  if (entries.length === 0) {
    el.innerHTML = '<span>Нет данных о локациях</span>';
  } else {
    el.innerHTML = entries.map(([from, loc]) =>
      `<div class="chip">${getContactName(from)}: ${loc.lat?.toFixed(5)}, ${loc.lon?.toFixed(5)}</div>`
    ).join('');
  }
}

function handleEvent(data) {
  try {
    const obj = JSON.parse(data);
    const evt = obj.evt;

    if (evt === 'msg') {
      addMessage(obj.from, obj.text || '', true);
    } else if (evt === 'info') {
      nodeId = obj.id || '';
      nickname = obj.nickname || '';
      if (Array.isArray(obj.groups)) groups = obj.groups;
      if (Array.isArray(obj.neighbors)) { neighbors = obj.neighbors; neighborsRssi = obj.neighborsRssi || obj.rssi || []; }
      migrateLegacyContactsWithKnownIds([nodeId, ...neighbors]);
      if (Array.isArray(obj.routes)) routes = obj.routes;
      currentRegion = obj.region || '';
      currentChannel = obj.channel ?? -1;
      powersaveEnabled = obj.powersave === true;
      offlinePending = obj.offlinePending || 0;
      if (obj.gpsPresent !== undefined) gpsPresent = obj.gpsPresent;
      if (obj.gpsEnabled !== undefined) gpsEnabled = obj.gpsEnabled;
      if (obj.gpsFix !== undefined) gpsFix = obj.gpsFix;
      updateNodeInfo();
      updateChannelSection();
      updateRegionButtons();
      updatePowersaveToggle();
      updateGpsSection();
      renderRecipient();
      renderNeighbors();
      renderGroups();
      renderRoutes();
    } else if (evt === 'groups') {
      if (Array.isArray(obj.groups)) { groups = obj.groups; renderRecipient(); renderGroups(); }
    } else if (evt === 'neighbors') {
      if (Array.isArray(obj.neighbors)) {
        neighbors = obj.neighbors;
        neighborsRssi = obj.rssi || obj.neighborsRssi || [];
        migrateLegacyContactsWithKnownIds([nodeId, ...neighbors]);
        renderRecipient(); renderNeighbors();
      }
    } else if (evt === 'routes') {
      if (Array.isArray(obj.routes)) { routes = obj.routes; renderRoutes(); }
    } else if (evt === 'location') {
      const from = obj.from || '?';
      locations[from] = { lat: obj.lat || 0, lon: obj.lon || 0 };
      addMessage(from, `📍 ${(obj.lat || 0).toFixed(5)}, ${(obj.lon || 0).toFixed(5)}`, true);
    } else if (evt === 'error') {
      showToast(obj.msg || 'Ошибка', 'error');
    } else if (evt === 'sent') {
      const to = obj.to || '';
      const msgId = obj.msgId || 0;
      if (to && msgId) {
        const m = findLastOutgoingWithoutMsgId(normalizeNodeId(to));
        if (m) {
          m.dataset.msgid = String(msgId);
          const st = m.querySelector('.status');
          if (st) st.textContent = '✓';
        }
      }
    } else if (evt === 'delivered') {
      const from = normalizeNodeId(obj.from || '');
      const msgId = obj.msgId || 0;
      if (msgId) {
        const m = document.querySelector(`.msg.outgoing[data-msgid="${msgId}"]`);
        if (m && m.dataset.recipient === from) {
          const st = m.querySelector('.status');
          if (st) st.textContent = '✓✓';
        }
      }
    } else if (evt === 'read') {
      const from = normalizeNodeId(obj.from || '');
      const msgId = obj.msgId || 0;
      if (msgId) {
        const m = document.querySelector(`.msg.outgoing[data-msgid="${msgId}"]`);
        if (m && m.dataset.recipient === from) {
          const st = m.querySelector('.status');
          if (st) st.textContent = '✓✓✓';
        }
      }
    } else if (evt === 'pong') {
      const from = obj.from || '?';
      const rssi = obj.rssi;
      showToast(`Pong от ${getContactName(from)}${rssi ? ` (${rssi})` : ''}`, '');
    } else if (evt === 'selftest') {
      const radioOk = obj.radioOk === true;
      const displayOk = obj.displayOk === true;
      const bat = obj.batteryMv || 0;
      const heap = obj.heapFree || 0;
      if (bat > 0) batteryMv = bat;
      showToast(`Selftest: ${radioOk ? '✓' : '✗'} радио, ${displayOk ? '✓' : '✗'} дисплей. ${bat}mV, ${heap}B heap`, radioOk && displayOk ? '' : 'error');
      updateNodeInfo();
    } else if (evt === 'invite') {
      const id = obj.id || '';
      const pubKey = obj.pubKey || '';
      const channelKey = obj.channelKey || '';
      const el = document.getElementById('inviteResult');
      if (el) {
        el.style.display = 'block';
        const copyData = channelKey ? JSON.stringify({ id, pubKey, channelKey }) : JSON.stringify({ id, pubKey });
        el.innerHTML = `
          <div class="invite-card">
            <div class="invite-row">
              <span class="invite-label">ID</span>
              <code class="invite-value" title="${escapeHtml(id)}">${escapeHtml(id)}</code>
            </div>
            <div class="invite-row">
              <span class="invite-label">PubKey</span>
              <code class="invite-value invite-pubkey" title="${escapeHtml(pubKey)}">${escapeHtml(pubKey.slice(0, 32))}…</code>
            </div>
            <button class="btn btn-copy" id="btnCopyInvite">
              <span class="copy-icon">📋</span> Копировать приглашение
            </button>
          </div>`;
        const copyBtn = el.querySelector('#btnCopyInvite');
        if (copyBtn) copyBtn.onclick = () => {
          navigator.clipboard.writeText(copyData).then(() => {
            showToast('Скопировано');
            copyBtn.classList.add('copied');
            copyBtn.querySelector('.copy-icon').textContent = '✓';
            setTimeout(() => { copyBtn.classList.remove('copied'); copyBtn.querySelector('.copy-icon').textContent = '📋'; }, 1500);
          }).catch(() => showToast('Ошибка копирования', 'error'));
        };
      }
    } else if (evt === 'region') {
      if (obj.region) currentRegion = obj.region;
      if (obj.channel !== undefined) currentChannel = obj.channel;
      updateChannelSection();
      updateRegionButtons();
    } else if (evt === 'telemetry') {
      const from = normalizeNodeId(obj.from || '?');
      telemetry[from] = { battery: obj.battery || 0, heapKb: obj.heapKb || 0 };
    } else if (evt === 'wifi') {
      const connected = obj.connected === true;
      const ssid = obj.ssid || '';
      const ip = obj.ip || '';
      showToast(connected ? `WiFi: ${ssid} — ${ip}` : `WiFi: не подключено`, connected ? '' : 'error');
    } else if (evt === 'gps') {
      if (obj.present !== undefined) gpsPresent = obj.present;
      if (obj.enabled !== undefined) gpsEnabled = obj.enabled;
      if (obj.hasFix !== undefined) gpsFix = obj.hasFix;
      updateGpsSection();
    } else if (evt === 'ota') {
      const ip = obj.ip || '192.168.4.1';
      const ssid = obj.ssid || 'RiftLink-OTA';
      const pass = obj.password || obj.pass || 'riftlink123';
      showToast(`OTA: Подключитесь к WiFi "${ssid}", пароль: ${pass}`, '');
    } else if (evt === 'voice') {
      const key = obj.from || '?';
      voiceBuffers[key] = voiceBuffers[key] || {};
      voiceBuffers[key][obj.chunk] = obj.data;
      const total = obj.total || 1;
      if (Object.keys(voiceBuffers[key]).length >= total) {
        let b64 = '';
        for (let i = 0; i < total; i++) b64 += (voiceBuffers[key][i] || '');
        delete voiceBuffers[key];
        try {
          const bytes = Uint8Array.from(atob(b64), c => c.charCodeAt(0));
          const blob = new Blob([bytes], { type: bytes[0] === 0x4f ? 'audio/ogg' : 'audio/webm' });
          const url = URL.createObjectURL(blob);
          const audio = new Audio(url);
          audio.onended = () => URL.revokeObjectURL(url);
          audio.play();
          addMessage(key, '🎤 Голос', true);
        } catch (_) {}
      }
    }
  } catch (_) {}
}

function updateNodeInfo() {
  const el = document.getElementById('nodeInfo');
  if (!el) return;
  let s = nickname || nodeId || '—';
  if (batteryMv > 0) s += ` ${(batteryMv / 1000).toFixed(1)}V`;
  if (offlinePending > 0) s += ` [${offlinePending} в очереди]`;
  el.textContent = s;
}

function updateChannelSection() {
  const el = document.getElementById('channelSection');
  if (!el) return;
  const isEu = currentRegion === 'EU' || currentRegion === 'UK';
  el.style.display = isEu ? 'block' : 'none';
  if (isEu) {
    el.querySelectorAll('.channel-btn').forEach(btn => {
      const ch = parseInt(btn.dataset.channel, 10);
      btn.classList.toggle('active', ch === currentChannel);
    });
  }
}

function updateRegionButtons() {
  document.querySelectorAll('.region-btn').forEach(btn => {
    btn.classList.toggle('active', btn.dataset.region === currentRegion);
  });
}

function updateGpsSection() {
  const wrap = document.getElementById('gpsSection');
  if (!wrap) return;
  wrap.style.display = gpsPresent ? 'block' : 'none';
  const toggle = document.getElementById('gpsToggle');
  if (toggle) toggle.checked = gpsEnabled;
  const status = document.getElementById('gpsStatus');
  if (status) status.textContent = gpsFix ? 'Фикс есть' : 'Нет фикса';
}

function updatePowersaveToggle() {
  const el = document.getElementById('powersaveToggle');
  if (el) el.checked = powersaveEnabled;
}

async function doPing(toId, btn) {
  if (!isFullNodeId(toId)) return;
  if (btn) { btn.disabled = true; btn.textContent = '...'; }
  const ok = await sendCmd({ cmd: 'ping', to: toId });
  if (btn) {
    setTimeout(() => { btn.disabled = false; btn.textContent = 'Ping'; }, 3000);
  }
  if (!ok) showToast('Ошибка ping', 'error');
}

function getBleSupportMessage() {
  if (navigator.bluetooth) return null;
  const ua = navigator.userAgent;
  if (/iPad|iPhone|iPod/.test(ua) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1)) {
    return 'Web Bluetooth не поддерживается в Safari. Используйте Chrome на Android или ПК.';
  }
  return 'Web Bluetooth не поддерживается. Используйте Chrome, Edge или Яндекс.Браузер.';
}

function formatBleError(e) {
  const name = (e.name || '').toLowerCase();
  const msg = (e.message || '').toLowerCase();
  if (name === 'notfounderror') return 'Устройство не найдено. Убедитесь, что RiftLink включён.';
  if (name === 'securityerror') return 'Нет разрешения на Bluetooth.';
  if (msg.includes('timeout')) return 'Таймаут — подойдите ближе.';
  if (msg.includes('disconnect') || msg.includes('gatt')) return 'Устройство отключилось. Подключитесь снова.';
  return e.message || 'Ошибка подключения';
}

async function connect() {
  if (!navigator.bluetooth) {
    showToast(getBleSupportMessage(), 'error');
    return;
  }
  const btn = document.getElementById('btnConnect');
  if (btn) { btn.disabled = true; btn.textContent = 'Поиск устройства...'; }
  try {
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
        showToast('RiftLink не найден по фильтру. Показываю все устройства...', '');
        device = await navigator.bluetooth.requestDevice({
          acceptAllDevices: true,
          optionalServices: [SERVICE_UUID]
        });
      } else throw e;
    }
    let server;
    for (let attempt = 1; attempt <= 3; attempt++) {
      try {
        server = await device.gatt.connect();
        break;
      } catch (e) {
        if (attempt === 3) throw e;
        showToast(`Попытка ${attempt}/3...`, '');
        await new Promise(r => setTimeout(r, 500));
      }
    }
    if (!server?.connected) throw new Error('GATT disconnected');
    document.getElementById('scanView').classList.remove('active');
    document.getElementById('appView').classList.add('active');
    showToast('Загрузка сервиса...', '');
    for (let attempt = 1; attempt <= 4; attempt++) {
      try {
        if (!device.gatt?.connected) {
          showToast('Переподключение...', '');
          server = await device.gatt.connect();
        }
        const service = await server.getPrimaryService(SERVICE_UUID);
        txChar = await service.getCharacteristic(TX_UUID);
        rxChar = await service.getCharacteristic(RX_UUID);
        await rxChar.startNotifications();
        break;
      } catch (e) {
        if (attempt === 4) throw e;
        const isGatt = /disconnect|gatt/i.test(e?.message || '');
        showToast(isGatt ? `Устройство перезагрузилось. Ожидание 6 сек... (${attempt}/4)` : `Попытка ${attempt + 1}/4...`, '');
        await new Promise(r => setTimeout(r, isGatt ? 6000 : 1500));
      }
    }
    addRxListener();
    try { device.removeEventListener('gattserverdisconnected', handleDisconnect); } catch (_) {}
    device.addEventListener('gattserverdisconnected', handleDisconnect);
    await new Promise(r => setTimeout(r, 200));
    sendCmd({ cmd: 'info' }).then(() => sendCmd({ cmd: 'groups' })).then(() => sendCmd({ cmd: 'routes' })).catch(() => {});
    showToast('Подключено', '');
  } catch (e) {
    txChar = null;
    rxChar = null;
    device = null;
    document.getElementById('appView').classList.remove('active');
    document.getElementById('scanView').classList.add('active');
    updateConnectButton();
    const msg = formatBleError(e);
    showToast(msg, 'error');
    console.error('[RiftLink] Connect failed:', e?.name, e?.message);
  } finally {
    if (btn) btn.disabled = false;
    updateConnectButton();
  }
}

function handleDisconnect() {
  if (intentionalDisconnect) return;
  txChar = null;
  rxChar = null;
  document.getElementById('appView').classList.remove('active');
  document.getElementById('scanView').classList.add('active');
  updateConnectButton();
  showToast('Устройство отключилось. Нажмите «Подключиться снова».', 'error');
}

function updateConnectButton() {
  const btn = document.getElementById('btnConnect');
  if (!btn) return;
  btn.textContent = device ? 'Подключиться снова' : 'Подключиться';
}

function disconnect() {
  intentionalDisconnect = true;
  if (device?.gatt?.connected) {
    try {
      device.removeEventListener('gattserverdisconnected', handleDisconnect);
    } catch (_) {}
    device.gatt.disconnect();
  }
  txChar = null;
  rxChar = null;
  device = null;
  intentionalDisconnect = false;
  document.getElementById('appView').classList.remove('active');
  document.getElementById('scanView').classList.add('active');
  updateConnectButton();
  showToast('Отключено', '');
}

async function reconnect() {
  if (!device) return connect();
  const btn = document.getElementById('btnConnect');
  if (btn) { btn.disabled = true; btn.textContent = 'Переподключение...'; }
  try {
    showToast('Переподключение...', '');
    let server;
    for (let attempt = 1; attempt <= 3; attempt++) {
      try {
        server = await device.gatt.connect();
        break;
      } catch (e) {
        if (attempt === 3) throw e;
        showToast(`Попытка ${attempt}/3...`, '');
        await new Promise(r => setTimeout(r, 500));
      }
    }
    if (!server?.connected) throw new Error('GATT disconnected');
    document.getElementById('scanView').classList.remove('active');
    document.getElementById('appView').classList.add('active');
    showToast('Загрузка сервиса...', '');
    for (let attempt = 1; attempt <= 4; attempt++) {
      try {
        if (!device.gatt?.connected) {
          showToast('Переподключение...', '');
          server = await device.gatt.connect();
        }
        const service = await server.getPrimaryService(SERVICE_UUID);
        txChar = await service.getCharacteristic(TX_UUID);
        rxChar = await service.getCharacteristic(RX_UUID);
        await rxChar.startNotifications();
        break;
      } catch (e) {
        if (attempt === 4) throw e;
        const isGatt = /disconnect|gatt/i.test(e?.message || '');
        showToast(isGatt ? `Устройство перезагрузилось. Ожидание 6 сек... (${attempt}/4)` : `Попытка ${attempt + 1}/4...`, '');
        await new Promise(r => setTimeout(r, isGatt ? 6000 : 1500));
      }
    }
    addRxListener();
    try { device.removeEventListener('gattserverdisconnected', handleDisconnect); } catch (_) {}
    device.addEventListener('gattserverdisconnected', handleDisconnect);
    await new Promise(r => setTimeout(r, 200));
    sendCmd({ cmd: 'info' }).then(() => sendCmd({ cmd: 'groups' })).then(() => sendCmd({ cmd: 'routes' })).catch(() => {});
    showToast('Подключено', '');
  } catch (e) {
    if (e?.message?.includes('disconnect') || e?.message?.includes('GATT') || e?.name === 'NotFoundError') {
      device = null;
    }
    document.getElementById('appView').classList.remove('active');
    document.getElementById('scanView').classList.add('active');
    updateConnectButton();
    showToast(formatBleError(e), 'error');
  } finally {
    if (btn) btn.disabled = false;
    updateConnectButton();
  }
}

async function sendMessage() {
  const input = document.getElementById('msgInput');
  const text = input?.value?.trim();
  if (!text) return;
  const recipient = document.getElementById('recipient')?.value || '';
  let cmd;
  if (recipient.startsWith('g')) {
    cmd = { cmd: 'send', group: parseInt(recipient.slice(1), 10), text };
  } else if (isFullNodeId(recipient)) {
    cmd = { cmd: 'send', to: normalizeNodeId(recipient), text };
  } else {
    cmd = { cmd: 'send', text };
  }
  const ok = await sendCmd(cmd);
  if (ok) {
    const recipientKey = (recipient && !recipient.startsWith('g')) ? normalizeNodeId(recipient) : '';
    const displayName = recipient ? (recipient.startsWith('g') ? 'Группа ' + recipient.slice(1) : recipient) : 'Broadcast';
    addMessage(displayName, text, false, null, recipientKey);
    input.value = '';
  } else {
    showToast('Ошибка отправки', 'error');
  }
}

function init() {
  const notice = document.getElementById('bleNotice');
  if (getBleSupportMessage()) {
    notice.textContent = getBleSupportMessage();
    notice.style.display = 'block';
  }

  document.getElementById('btnConnect').addEventListener('click', () => (device ? reconnect() : connect()));
  document.getElementById('btnDisconnect').addEventListener('click', disconnect);
  document.getElementById('btnSend').addEventListener('click', sendMessage);

  document.getElementById('msgInput')?.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') sendMessage();
  });

  document.getElementById('recipient')?.addEventListener('change', updateMsgInputLimit);

  document.getElementById('btnAddGroup')?.addEventListener('click', async () => {
    const inp = document.getElementById('groupId');
    const gid = parseInt(inp?.value, 10);
    if (!gid || gid <= 0) { showToast('Введите ID группы', 'error'); return; }
    const ok = await sendCmd({ cmd: 'addGroup', group: gid });
    if (ok) { inp.value = ''; sendCmd({ cmd: 'groups' }); }
  });

  document.getElementById('btnNickname')?.addEventListener('click', async () => {
    const nick = document.getElementById('nickname')?.value?.trim() || '';
    const ok = await sendCmd({ cmd: 'nickname', nickname: nick });
    if (ok) { nickname = nick; updateNodeInfo(); }
  });

  document.querySelectorAll('.region-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const r = btn.dataset.region;
      currentRegion = r;
      updateChannelSection();
      updateRegionButtons();
      sendCmd({ cmd: 'region', region: r });
      showToast('Регион: ' + r);
    });
  });

  document.getElementById('btnLocation')?.addEventListener('click', async () => {
    if (!navigator.geolocation) { showToast('Геолокация недоступна', 'error'); return; }
    navigator.geolocation.getCurrentPosition(
      async (pos) => {
        const ok = await sendCmd({
          cmd: 'location',
          lat: pos.coords.latitude,
          lon: pos.coords.longitude,
          alt: Math.round(pos.coords.altitude || 0)
        });
        if (ok) addMessage('Я', `📍 ${pos.coords.latitude.toFixed(5)}, ${pos.coords.longitude.toFixed(5)}`, false);
      },
      () => showToast('Не удалось получить координаты', 'error'),
      { enableHighAccuracy: true, timeout: 10000 }
    );
  });

  document.getElementById('btnOta')?.addEventListener('click', () => sendCmd({ cmd: 'ota' }));

  document.getElementById('btnWifi')?.addEventListener('click', async () => {
    const ssid = (document.getElementById('wifiSsid')?.value || '').trim();
    const pass = (document.getElementById('wifiPass')?.value || '').trim();
    if (!ssid) { showToast('Введите SSID', 'error'); return; }
    const ok = await sendCmd({ cmd: 'wifi', ssid, pass });
    if (ok) showToast('Подключение к WiFi...');
  });

  document.getElementById('gpsToggle')?.addEventListener('change', (e) => {
    const enabled = e.target.checked;
    sendCmd({ cmd: 'gps', enabled });
    gpsEnabled = enabled;
    showToast('GPS: ' + (enabled ? 'вкл' : 'выкл'));
  });

  document.querySelectorAll('.lang-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const lang = btn.dataset.lang;
      sendCmd({ cmd: 'lang', lang });
      showToast('Язык: ' + (lang === 'ru' ? 'Русский' : 'English'));
    });
  });

  document.getElementById('btnSelftest')?.addEventListener('click', () => {
    sendCmd({ cmd: 'selftest' });
    showToast('Selftest запущен...', '');
  });

  document.getElementById('powersaveToggle')?.addEventListener('change', (e) => {
    const enabled = e.target.checked;
    sendCmd({ cmd: 'powersave', enabled });
    powersaveEnabled = enabled;
    showToast('Powersave: ' + (enabled ? 'вкл' : 'выкл'));
  });

  document.querySelectorAll('.channel-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const ch = parseInt(btn.dataset.channel, 10);
      sendCmd({ cmd: 'channel', channel: ch });
      currentChannel = ch;
      updateChannelSection();
      showToast('Канал: ' + ch);
    });
  });

  document.getElementById('btnInvite')?.addEventListener('click', () => {
    document.getElementById('inviteResult').style.display = 'none';
    sendCmd({ cmd: 'invite' });
    showToast('Создание приглашения...', '');
  });

  document.getElementById('btnPasteInvite')?.addEventListener('click', async () => {
    try {
      const text = await navigator.clipboard.readText();
      const r = JSON.parse(text);
      const id = (r?.id ?? '').replace(/[^0-9A-Fa-f]/g, '').slice(0, 16);
      const pubKey = r?.pubKey ?? '';
      const channelKey = r?.channelKey ?? '';
      document.getElementById('acceptId').value = id;
      document.getElementById('acceptPubKey').value = pubKey;
      document.getElementById('acceptChannelKey').value = channelKey;
      showToast('Вставлено');
    } catch (_) { showToast('Ошибка вставки (ожидается JSON invite)', 'error'); }
  });

  document.getElementById('btnAcceptInvite')?.addEventListener('click', async () => {
    const id = normalizeNodeId((document.getElementById('acceptId')?.value || '').trim());
    const pubKey = (document.getElementById('acceptPubKey')?.value || '').trim();
    const channelKey = (document.getElementById('acceptChannelKey')?.value || '').trim();
    if (!isFullNodeId(id) || !pubKey) {
      showToast('Введите ID и PubKey', 'error');
      return;
    }
    const cmd = { cmd: 'acceptInvite', id, pubKey };
    if (channelKey) cmd.channelKey = channelKey;
    const ok = await sendCmd(cmd);
    if (ok) {
      showToast('Приглашение принято');
      sendCmd({ cmd: 'info' });
    } else showToast('Ошибка acceptInvite', 'error');
  });

  document.getElementById('btnAddContact')?.addEventListener('click', () => {
    const id = normalizeNodeId((document.getElementById('contactId')?.value || '').trim());
    const nick = (document.getElementById('contactNick')?.value || '').trim().slice(0, 16);
    if (!isFullNodeId(id)) { showToast('Введите ID (16 hex)', 'error'); return; }
    if (!saveContact(id, nick || id)) { showToast('Ошибка контакта', 'error'); return; }
    document.getElementById('contactId').value = '';
    document.getElementById('contactNick').value = '';
    renderContacts();
    renderRecipient();
    renderNeighbors();
    showToast('Контакты обновлены');
  });

  document.getElementById('btnVoice')?.addEventListener('click', async () => {
    const recipient = document.getElementById('recipient')?.value || '';
    if (!recipient || recipient.startsWith('g') || !isFullNodeId(recipient)) {
      showToast('Выберите получателя (unicast)', 'error');
      return;
    }
    if (!navigator.mediaDevices?.getUserMedia) {
      showToast('Микрофон недоступен', 'error');
      return;
    }
    const btn = document.getElementById('btnVoice');
    try {
      const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
      const opts = { audioBitsPerSecond: 8000 };
      const mime = MediaRecorder.isTypeSupported('audio/webm;codecs=opus') ? 'audio/webm;codecs=opus' : 'audio/webm';
      const recorder = new MediaRecorder(stream, { mimeType: mime, ...opts });
      const chunks = [];
      recorder.ondataavailable = (e) => { if (e.data.size) chunks.push(e.data); };
      recorder.onstop = async () => {
        stream.getTracks().forEach(t => t.stop());
        const blob = new Blob(chunks, { type: 'audio/webm' });
        const buf = await blob.arrayBuffer();
        const bytes = new Uint8Array(buf);
        const CHUNK_RAW = 384;
        const total = Math.ceil(bytes.length / CHUNK_RAW);
        for (let i = 0; i < total; i++) {
          const slice = bytes.slice(i * CHUNK_RAW, (i + 1) * CHUNK_RAW);
          const b64 = btoa(String.fromCharCode.apply(null, slice));
          const ok = await sendCmd({ cmd: 'voice', to: normalizeNodeId(recipient), data: b64, chunk: i, total });
          if (!ok) { showToast('Ошибка отправки голоса', 'error'); break; }
        }
        if (total > 0) addMessage(normalizeNodeId(recipient), '🎤 Голос', false);
      };
      btn.classList.add('recording');
      recorder.start(500);
      const stop = () => {
        if (recorder.state === 'recording') { recorder.stop(); btn.classList.remove('recording'); }
      };
      btn.onpointerup = stop;
      btn.onpointerleave = stop;
      setTimeout(stop, 30000);
    } catch (e) {
      showToast('Ошибка микрофона: ' + (e.message || 'unknown'), 'error');
    }
  });

  document.querySelectorAll('.back-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.panel-view').forEach(v => v.classList.remove('active'));
      document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
      const chatNav = document.querySelector('.nav-btn[data-view="chat"]');
      if (chatNav) chatNav.classList.add('active');
    });
  });

  document.getElementById('btnMap')?.addEventListener('click', () => {
    document.getElementById('mapView').classList.add('active');
    renderMap();
  });

  document.getElementById('btnMesh')?.addEventListener('click', () => {
    document.getElementById('networkView').classList.add('active');
    sendCmd({ cmd: 'routes' });
  });

  document.getElementById('btnSettings')?.addEventListener('click', () => {
    document.getElementById('settingsView').classList.add('active');
    document.getElementById('nickname').value = nickname;
    renderContacts();
    updatePowersaveToggle();
    updateChannelSection();
    updateRegionButtons();
    updateGpsSection();
  });

  document.querySelectorAll('.nav-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      const view = btn.dataset.view;
      document.querySelectorAll('.panel-view').forEach(v => v.classList.remove('active'));
      if (view === 'network') {
        document.getElementById('networkView').classList.add('active');
        sendCmd({ cmd: 'routes' });
      } else if (view === 'settings') {
        document.getElementById('settingsView').classList.add('active');
        document.getElementById('nickname').value = nickname;
        renderContacts();
        updatePowersaveToggle();
        updateChannelSection();
        updateRegionButtons();
        updateGpsSection();
      }
    });
  });
}

init();
