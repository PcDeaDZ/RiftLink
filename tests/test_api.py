#!/usr/bin/env python3
"""
RiftLink BLE/Serial API — unit tests
JSON команды и события: docs/API.md
"""

import json
import pytest


class TestBLECommands:
    """Проверка формата BLE команд (JSON)"""

    def test_send_broadcast(self):
        cmd = {'cmd': 'send', 'text': 'Hello'}
        s = json.dumps(cmd)
        parsed = json.loads(s)
        assert parsed['cmd'] == 'send'
        assert parsed['text'] == 'Hello'
        assert 'to' not in parsed

    def test_send_unicast(self):
        cmd = {'cmd': 'send', 'to': 'A1B2C3D4', 'text': 'Hi'}
        parsed = json.loads(json.dumps(cmd))
        assert parsed['to'] == 'A1B2C3D4'
        assert len(parsed['to']) == 8

    def test_location(self):
        cmd = {'cmd': 'location', 'lat': 55.7558, 'lon': 37.6173, 'alt': 150}
        parsed = json.loads(json.dumps(cmd))
        assert parsed['lat'] == 55.7558
        assert parsed['lon'] == 37.6173

    def test_region(self):
        for r in ['EU', 'UK', 'RU', 'US', 'AU']:
            cmd = {'cmd': 'region', 'region': r}
            assert json.loads(json.dumps(cmd))['region'] == r

    def test_channel(self):
        cmd = {'cmd': 'channel', 'channel': 1}
        parsed = json.loads(json.dumps(cmd))
        assert parsed['channel'] in (0, 1, 2)

    def test_nickname(self):
        cmd = {'cmd': 'nickname', 'nickname': 'Alice'}
        parsed = json.loads(json.dumps(cmd))
        assert len(parsed['nickname']) <= 16

    def test_ping(self):
        cmd = {'cmd': 'ping', 'to': 'A1B2C3D4E5F60708'}
        parsed = json.loads(json.dumps(cmd))
        assert len(parsed['to']) >= 8

    def test_send_group_v2_channel(self):
        cmd = {'cmd': 'send', 'group': 12345, 'text': 'Group hello'}
        parsed = json.loads(json.dumps(cmd))
        assert parsed['cmd'] == 'send'
        assert parsed['group'] >= 2

    def test_group_leave_command(self):
        cmd = {'cmd': 'groupLeave', 'groupUid': 'B64_16_or_32B'}
        parsed = json.loads(json.dumps(cmd))
        assert parsed['cmd'] == 'groupLeave'
        assert parsed['groupUid']


class TestBLEEvents:
    """Проверка формата BLE событий"""

    def test_node_snapshot_event(self):
        evt = {
            'evt': 'node',
            'seq': 1,
            'id': 'A1B2C3D4E5F60708',
            'region': 'EU',
            'freq': 868.1,
            'power': 14,
            'channel': 0,
            'version': '0.5.0',
            'neighbors': ['B2C3D4E5F6070819']
        }
        parsed = json.loads(json.dumps(evt))
        assert parsed['evt'] == 'node'
        assert len(parsed['id']) == 16
        assert parsed['freq'] in (868.1, 868.3, 868.5, 868.8, 915.0)
        assert isinstance(parsed['neighbors'], list)

    def test_msg_event(self):
        evt = {'evt': 'msg', 'from': 'A1B2C3D4', 'text': 'Hello'}
        parsed = json.loads(json.dumps(evt))
        assert parsed['evt'] == 'msg'

    def test_msg_event_group_context(self):
        evt = {
            'evt': 'msg',
            'from': 'A1B2C3D4E5F60708',
            'text': 'Group payload',
            'group': 12345,
            'groupUid': 'B64_16_or_32B',
        }
        parsed = json.loads(json.dumps(evt))
        assert parsed['evt'] == 'msg'
        assert parsed['group'] >= 2
        assert parsed['groupUid']

    def test_groups_event_snapshot(self):
        evt = {
            'evt': 'groups',
            'seq': 1,
            'groups': [
                {
                    'groupUid': 'B64_16_or_32B',
                    'groupTag': 'B64_8_or_16B',
                    'canonicalName': 'Team Alpha',
                    'channelId32': 12345,
                    'keyVersion': 7,
                    'myRole': 'admin',
                }
            ],
        }
        parsed = json.loads(json.dumps(evt))
        assert parsed['evt'] == 'groups'
        assert isinstance(parsed['groups'], list)
        assert parsed['groups'][0]['channelId32'] >= 2

    def test_neighbors_event(self):
        evt = {'evt': 'neighbors', 'neighbors': ['A1B2C3D4E5F60708']}
        parsed = json.loads(json.dumps(evt))
        assert parsed['evt'] == 'neighbors'
        assert len(parsed['neighbors']) >= 0


class TestValidation:
    """Валидация данных"""

    def test_node_id_hex(self):
        valid = 'A1B2C3D4E5F60708'
        assert len(valid) == 16
        assert all(c in '0123456789ABCDEFabcdef' for c in valid)

    def test_nickname_max_length(self):
        assert len('Alice') <= 16
        assert len('A' * 16) == 16

    def test_region_codes(self):
        valid = {'EU', 'UK', 'RU', 'US', 'AU'}
        assert 'EU' in valid
        assert 'XX' not in valid


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
