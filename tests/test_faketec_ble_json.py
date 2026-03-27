#!/usr/bin/env python3
"""
FakeTech BLE: NDJSON-волна info и cmdId для TransportResponseRouter (docs/API.md).
"""

import json


def parse_ndjson(s: str):
    out = []
    for line in s.splitlines():
        line = line.strip()
        if not line:
            continue
        out.append(json.loads(line))
    return out


class TestFaketecInfoWave:
    def test_info_wave_has_cmd_id_on_each_evt(self):
        cmd_id = 42
        seq = 3
        lines = [
            json.dumps(
                {
                    'evt': 'node',
                    'seq': seq,
                    'cmdId': cmd_id,
                    'id': 'A1B2C3D4E5F60708',
                    'region': 'EU',
                    'version': '1.0.0-faketec',
                }
            ),
            json.dumps(
                {
                    'evt': 'neighbors',
                    'seq': seq,
                    'cmdId': cmd_id,
                    'neighbors': [],
                    'rssi': [],
                    'hasKey': [],
                }
            ),
            json.dumps({'evt': 'routes', 'seq': seq, 'cmdId': cmd_id, 'routes': []}),
            json.dumps({'evt': 'groups', 'seq': seq, 'cmdId': cmd_id, 'groups': []}),
        ]
        ndjson = '\n'.join(lines) + '\n'
        evts = parse_ndjson(ndjson)
        assert len(evts) == 4
        for e in evts:
            assert e['cmdId'] == cmd_id
            assert e['seq'] == seq
        assert evts[0]['evt'] == 'node'
        assert evts[1]['evt'] == 'neighbors'
        assert evts[2]['evt'] == 'routes'
        assert evts[3]['evt'] == 'groups'

    def test_routes_command_includes_cmd_id(self):
        cmd = {'cmd': 'routes', 'cmdId': 7}
        assert json.loads(json.dumps(cmd))['cmdId'] == 7

    def test_groups_command_includes_cmd_id(self):
        cmd = {'cmd': 'groups', 'cmdId': 8}
        assert json.loads(json.dumps(cmd))['cmdId'] == 8

    def test_region_evt_has_cmd_id(self):
        cmd_id = 5
        evt = {
            'evt': 'region',
            'region': 'EU',
            'freq': 868.0,
            'power': 14,
            'channel': 0,
            'cmdId': cmd_id,
        }
        assert json.loads(json.dumps(evt))['cmdId'] == cmd_id
        assert evt['evt'] == 'region'

    def test_invite_evt_has_cmd_id(self):
        cmd_id = 9
        evt = {
            'evt': 'invite',
            'id': 'A1B2C3D4E5F60708',
            'pubKey': 'dGVzdA==',
            'inviteTtlMs': 600000,
            'cmdId': cmd_id,
        }
        assert json.loads(json.dumps(evt))['cmdId'] == cmd_id

    def test_group_status_and_rekey_have_cmd_id(self):
        cmd_id = 11
        gs = {
            'evt': 'groupStatus',
            'groupUid': 'test-uid',
            'myRole': 'none',
            'keyVersion': 0,
            'status': 'faketec_stub',
            'rekeyRequired': False,
            'cmdId': cmd_id,
        }
        grp = {
            'evt': 'groupRekeyProgress',
            'groupUid': 'test-uid',
            'rekeyOpId': 'faketec_stub',
            'keyVersion': 0,
            'pending': 0,
            'delivered': 0,
            'applied': 0,
            'failed': 0,
            'cmdId': cmd_id,
        }
        assert json.loads(json.dumps(gs))['cmdId'] == cmd_id
        assert json.loads(json.dumps(grp))['cmdId'] == cmd_id
