import { describe, it, expect } from 'vitest';
import { buildInviteCopyData, parseInviteJson } from './utils.js';

describe('buildInviteCopyData', () => {
  it('builds JSON without channelKey', () => {
    const data = buildInviteCopyData({ id: 'A1B2C3D4', pubKey: 'base64key' });
    const parsed = JSON.parse(data);
    expect(parsed.id).toBe('A1B2C3D4');
    expect(parsed.pubKey).toBe('base64key');
    expect(parsed.channelKey).toBeUndefined();
  });

  it('builds JSON with channelKey', () => {
    const data = buildInviteCopyData({
      id: 'A1B2C3D4',
      pubKey: 'pk',
      channelKey: 'chkey32',
    });
    const parsed = JSON.parse(data);
    expect(parsed.channelKey).toBe('chkey32');
  });

  it('excludes channelKey when empty', () => {
    const data = buildInviteCopyData({ id: 'X', pubKey: 'Y', channelKey: '' });
    const parsed = JSON.parse(data);
    expect(parsed.channelKey).toBeUndefined();
  });

  it('escapes special chars', () => {
    const data = buildInviteCopyData({ id: 'A1', pubKey: 'key"with\\quotes' });
    expect(data).toContain('\\"');
    const parsed = JSON.parse(data);
    expect(parsed.pubKey).toBe('key"with\\quotes');
  });
});

describe('parseInviteJson', () => {
  it('parses valid JSON with channelKey', () => {
    const r = parseInviteJson('{"id":"A1B2C3D4","pubKey":"pk","channelKey":"ck"}');
    expect(r.id).toBe('A1B2C3D4');
    expect(r.pubKey).toBe('pk');
    expect(r.channelKey).toBe('ck');
  });

  it('parses JSON without channelKey', () => {
    const r = parseInviteJson('{"id":"DEADBEEF","pubKey":"old"}');
    expect(r.channelKey).toBe('');
  });

  it('normalizes id (hex only)', () => {
    const r = parseInviteJson('{"id":"A1-B2-C3-D4","pubKey":"x"}');
    expect(r.id).toBe('A1B2C3D4');
  });

  it('returns empty on invalid JSON', () => {
    const r = parseInviteJson('{invalid}');
    expect(r.id).toBe('');
    expect(r.pubKey).toBe('');
  });

  it('returns empty on empty string', () => {
    const r = parseInviteJson('');
    expect(r.id).toBe('');
  });
});
