#!/usr/bin/env python3
"""
RiftLink Protocol — unit tests
Формат пакета: docs/PROTOCOL.md
"""

import struct
import pytest

NODE_ID_LEN = 8
HEADER_LEN = 1 + NODE_ID_LEN * 2 + 1 + 1 + 1  # 20 (with channel)
FLAG_ENCRYPTED = 0x08
FLAG_COMPRESSED = 0x04
FLAG_ACK_REQ = 0x02
VERSION = 0x10

OP_MSG = 0x01
OP_ACK = 0x02
OP_HELLO = 0x03
OP_PING = 0xFF
OP_PONG = 0xFE

BROADCAST_ID = bytes([0xFF] * 8)


def build_packet(from_id: bytes, to_id: bytes, ttl: int, opcode: int,
                 payload: bytes = b'', encrypted: bool = False,
                 ack_req: bool = False, compressed: bool = False,
                 channel: int = 0) -> bytes:
    """Build packet matching firmware protocol::buildPacket."""
    assert len(from_id) == NODE_ID_LEN and len(to_id) == NODE_ID_LEN
    flags = VERSION | (FLAG_ENCRYPTED if encrypted else 0) | \
            (FLAG_ACK_REQ if ack_req else 0) | (FLAG_COMPRESSED if compressed else 0)
    return bytes([flags]) + from_id + to_id + bytes([ttl, opcode, channel]) + payload


def parse_packet(buf: bytes) -> tuple:
    """Parse packet, return (header_dict, payload)."""
    if len(buf) < HEADER_LEN:
        raise ValueError("Packet too short")
    flags = buf[0]
    from_id = buf[1:1+NODE_ID_LEN]
    to_id = buf[1+NODE_ID_LEN:1+NODE_ID_LEN*2]
    ttl = buf[17]
    opcode = buf[18]
    channel = buf[19] if len(buf) >= HEADER_LEN else 0
    payload = buf[HEADER_LEN:] if len(buf) > HEADER_LEN else b''
    return {
        'encrypted': bool(flags & FLAG_ENCRYPTED),
        'compressed': bool(flags & FLAG_COMPRESSED),
        'ack_req': bool(flags & FLAG_ACK_REQ),
        'from': from_id,
        'to': to_id,
        'ttl': ttl,
        'opcode': opcode,
        'channel': channel,
    }, payload


class TestPacketBuild:
    def test_hello_packet(self):
        from_id = bytes.fromhex('A1B2C3D4E5F60708')
        to_id = BROADCAST_ID
        pkt = build_packet(from_id, to_id, 31, OP_HELLO)
        assert len(pkt) == HEADER_LEN
        assert pkt[0] == VERSION
        assert pkt[1:9] == from_id
        assert pkt[9:17] == to_id
        assert pkt[17] == 31
        assert pkt[18] == OP_HELLO

    def test_msg_with_payload(self):
        from_id = bytes.fromhex('0102030405060708')
        to_id = bytes.fromhex('A1B2C3D4E5F60708')
        payload = b'Hello'
        pkt = build_packet(from_id, to_id, 15, OP_MSG, payload)
        assert len(pkt) == HEADER_LEN + 5
        assert pkt[HEADER_LEN:HEADER_LEN+5] == payload

    def test_encrypted_flags(self):
        from_id = bytes(8)
        to_id = bytes(8)
        pkt = build_packet(from_id, to_id, 1, OP_MSG, encrypted=True, ack_req=True)
        assert pkt[0] & FLAG_ENCRYPTED
        assert pkt[0] & FLAG_ACK_REQ


class TestPacketParse:
    def test_parse_hello(self):
        from_id = bytes.fromhex('A1B2C3D4E5F60708')
        pkt = build_packet(from_id, BROADCAST_ID, 31, OP_HELLO)
        hdr, payload = parse_packet(pkt)
        assert hdr['from'] == from_id
        assert hdr['to'] == BROADCAST_ID
        assert hdr['ttl'] == 31
        assert hdr['opcode'] == OP_HELLO
        assert payload == b''

    def test_parse_payload(self):
        pkt = build_packet(bytes(8), bytes(8), 5, OP_MSG, b'x' * 10)
        hdr, payload = parse_packet(pkt)
        assert len(payload) == 10
        assert payload == b'x' * 10

    def test_parse_too_short(self):
        with pytest.raises(ValueError):
            parse_packet(b'\x00' * 10)


class TestRoundtrip:
    def test_build_parse_roundtrip(self):
        from_id = bytes.fromhex('DEADBEEF01234567')
        to_id = bytes.fromhex('A1B2C3D4E5F60708')
        payload = b'Test message'
        pkt = build_packet(from_id, to_id, 15, OP_MSG, payload, encrypted=True)
        hdr, pl = parse_packet(pkt)
        assert hdr['from'] == from_id
        assert hdr['to'] == to_id
        assert hdr['ttl'] == 15
        assert hdr['opcode'] == OP_MSG
        assert hdr['encrypted']
        assert pl == payload


class TestMsgFragFormat:
    """MSG_FRAG payload format: MsgID(4) + Part(1) + Total(1) + Data"""
    def test_frag_header_size(self):
        msg_id = 0x12345678
        part, total = 1, 5
        data = b'x' * 100
        frag = struct.pack('<I', msg_id) + bytes([part, total]) + data
        assert len(frag) == 6 + 100
        mid, p, t = struct.unpack('<IBB', frag[:6])
        assert mid == msg_id and p == part and t == total


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
