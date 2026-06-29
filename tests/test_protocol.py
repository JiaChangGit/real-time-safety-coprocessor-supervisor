"""test_protocol - 協定線格式 (wire format) 測試。

涵蓋：header 大小、每種 frame 型別的 pack/parse round-trip、golden vector、
以及 validate() 對 bad magic / bad version / over-long payload 的拒絕。
"""

import os
import struct
import sys
import unittest

# 確保可 import 同目錄下的共用 helper（支援 unittest discover 與 pytest 兩種啟動）。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import safety_proto as sp  # noqa: E402


class TestHeader(unittest.TestCase):
    def test_header_size_is_16(self):
        self.assertEqual(sp.SAFETY_HEADER_SIZE, 16)
        self.assertEqual(struct.calcsize("<HBBHHII"), 16)
        self.assertEqual(len(sp.pack_header(sp.FRAME_HEARTBEAT, 0, 0, 0)), 16)

    def test_constants(self):
        self.assertEqual(sp.SAFETY_PROTO_MAGIC, 0x5346)
        self.assertEqual(sp.SAFETY_PROTO_VERSION, 1)


class TestGoldenVector(unittest.TestCase):
    """鎖定的 golden vector：HEARTBEAT seq=42, uptime=1234, beat_seq=7, ts=1234。"""

    def test_golden_vector_exact_hex_and_checksum(self):
        frame = sp.build_frame(
            sp.FRAME_HEARTBEAT, sequence_id=42, timestamp_ms=1234,
            fields=(1234, 7))
        expected_hex = "465301012a000800d2040000b875ef21d204000007000000"
        self.assertEqual(frame.hex(), expected_hex)

        header = sp.parse_header(frame)
        self.assertEqual(header.checksum, 0x21EF75B8)
        self.assertEqual(header.payload_length, 8)
        self.assertEqual(header.sequence_id, 42)
        self.assertEqual(header.timestamp_ms, 1234)
        self.assertEqual(sp.validate(frame), sp.VALIDATE_OK)


class TestRoundTrip(unittest.TestCase):
    """每種 frame 型別的 pack -> parse round-trip。"""

    # (frame_type, payload fields tuple)
    CASES = {
        "heartbeat": (sp.FRAME_HEARTBEAT, (1234, 7)),
        "telemetry": (sp.FRAME_TELEMETRY, (372, 3300, 55, 2)),
        "fault_event": (sp.FRAME_FAULT_EVENT, (sp.FAULT_TASK_HANG, 2, 0xABCD, 999)),
        "command": (sp.FRAME_COMMAND, (sp.CMD_REQUEST_RECOVERY, 0, 350, 0xDEADBEEF)),
        "ack": (sp.FRAME_ACK, (42, 0)),
        "nack": (sp.FRAME_NACK, (42, sp.NACK_CHECKSUM, 0)),
        "recovery_report": (sp.FRAME_RECOVERY_REPORT, (1, 1, 0, 2800)),
    }

    def test_roundtrip_all_frame_types(self):
        for name, (ftype, fields) in self.CASES.items():
            with self.subTest(frame=name):
                frame = sp.build_frame(ftype, sequence_id=7, timestamp_ms=1000,
                                       fields=fields)
                self.assertEqual(sp.validate(frame), sp.VALIDATE_OK)
                parsed = sp.parse_frame(frame)
                self.assertEqual(parsed.header.type, ftype)
                self.assertEqual(parsed.header.sequence_id, 7)
                self.assertEqual(parsed.header.timestamp_ms, 1000)
                self.assertEqual(parsed.header.payload_length,
                                 struct.calcsize(sp.PAYLOAD_FMT[ftype]))
                self.assertEqual(parsed.fields, fields)

    def test_header_roundtrip(self):
        for ftype in sp.FRAME_TYPE_NAME:
            with self.subTest(frame_type=ftype):
                hdr_bytes = sp.pack_header(ftype, sequence_id=0x1234,
                                           payload_length=8,
                                           timestamp_ms=0x01020304,
                                           checksum=0xAABBCCDD)
                hdr = sp.parse_header(hdr_bytes)
                self.assertEqual(hdr.magic, sp.SAFETY_PROTO_MAGIC)
                self.assertEqual(hdr.version, sp.SAFETY_PROTO_VERSION)
                self.assertEqual(hdr.type, ftype)
                self.assertEqual(hdr.sequence_id, 0x1234)
                self.assertEqual(hdr.payload_length, 8)
                self.assertEqual(hdr.timestamp_ms, 0x01020304)
                self.assertEqual(hdr.checksum, 0xAABBCCDD)


class TestValidateRejects(unittest.TestCase):
    """validate() 對非法 frame 回傳正確結果碼。"""

    def _good_frame(self):
        return sp.build_frame(sp.FRAME_HEARTBEAT, 1, 100, (1, 1))

    def test_ok(self):
        self.assertEqual(sp.validate(self._good_frame()), sp.VALIDATE_OK)

    def test_too_short(self):
        self.assertEqual(sp.validate(b"\x00" * 8), sp.VALIDATE_BAD_LENGTH)

    def test_bad_magic(self):
        frame = bytearray(self._good_frame())
        frame[0] ^= 0xFF  # 破壞 magic 低位元組
        self.assertEqual(sp.validate(bytes(frame)), sp.VALIDATE_BAD_MAGIC)

    def test_bad_version(self):
        frame = bytearray(self._good_frame())
        frame[2] = 0x02  # version 欄位 (offset 2)
        self.assertEqual(sp.validate(bytes(frame)), sp.VALIDATE_BAD_VERSION)

    def test_over_long_payload(self):
        # payload_length 欄位宣稱 > SAFETY_MAX_PAYLOAD。
        header = sp.pack_header(sp.FRAME_HEARTBEAT, 1, sp.SAFETY_MAX_PAYLOAD + 1, 100)
        buf = header + b"\x00" * (sp.SAFETY_MAX_PAYLOAD + 1)
        self.assertEqual(sp.validate(buf), sp.VALIDATE_BAD_LENGTH)

    def test_truncated_payload(self):
        # payload_length 宣稱 8，但實際 buffer 只給 4 bytes。
        header0 = sp.pack_header(sp.FRAME_HEARTBEAT, 1, 8, 100, 0)
        partial = b"\x00\x00\x00\x00"
        checksum = sp.compute_checksum(header0, partial + b"\x00\x00\x00\x00")
        header = sp.pack_header(sp.FRAME_HEARTBEAT, 1, 8, 100, checksum)
        self.assertEqual(sp.validate(header + partial), sp.VALIDATE_BAD_LENGTH)

    def test_max_payload_boundary_is_length_ok(self):
        # payload_length == MAX 不應因長度被拒（只要 buffer 夠長）。
        header0 = sp.pack_header(sp.FRAME_TELEMETRY, 1, sp.SAFETY_MAX_PAYLOAD, 100, 0)
        payload = b"\x5a" * sp.SAFETY_MAX_PAYLOAD
        checksum = sp.compute_checksum(header0, payload)
        header = sp.pack_header(sp.FRAME_TELEMETRY, 1, sp.SAFETY_MAX_PAYLOAD, 100, checksum)
        self.assertEqual(sp.validate(header + payload), sp.VALIDATE_OK)


if __name__ == "__main__":
    unittest.main()
