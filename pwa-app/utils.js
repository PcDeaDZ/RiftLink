/**
 * Утилиты PWA — тестируемые чистые функции
 */

/**
 * Собрать JSON для копирования invite (id, pubKey, channelKey)
 * @param {{ id: string, pubKey: string, channelKey?: string }} obj
 * @returns {string}
 */
export function buildInviteCopyData(obj) {
  const id = obj.id || '';
  const pubKey = obj.pubKey || '';
  const channelKey = obj.channelKey || '';
  const data = { id, pubKey };
  if (channelKey) data.channelKey = channelKey;
  return JSON.stringify(data);
}

/**
 * Распарсить JSON invite из буфера (при вставке)
 * @param {string} str
 * @returns {{ id: string, pubKey: string, channelKey: string }}
 */
export function parseInviteJson(str) {
  try {
    const m = JSON.parse(str);
    const id = (m?.id ?? '').replace(/[^0-9A-Fa-f]/g, '').toUpperCase();
    return {
      id: /^[0-9A-F]{16}$/.test(id) ? id : '',
      pubKey: m?.pubKey ?? '',
      channelKey: m?.channelKey ?? '',
    };
  } catch {
    return { id: '', pubKey: '', channelKey: '' };
  }
}
