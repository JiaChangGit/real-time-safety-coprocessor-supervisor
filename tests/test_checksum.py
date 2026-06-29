"""test_checksum - CRC-32 checksum 行為測試。

涵蓋：crc32("123456789") 已知值、frame checksum == binascii.crc32(header0+payload)、
單一 byte 翻轉會改變 checksum 並使 validate() 回傳 BAD_CHECKSUM，且對應的 NACK
原因為 CHECKSUM。以多組輸入參數化 (parametrize)。
"""

import binascii
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import safety_proto as sp  # noqa: E402


class TestCrc32KnownVectors(unittest.TestCase):
    def test_check_string(self):
        # CRC-32 標準 "check" 值。
        self.assertEqual(sp.crc32(b"123456789"), 0xCBF43926)
        self.assertEqual(binascii.crc32(b"123456789") & 0xFFFFFFFF, 0xCBF43926)

    def test_empty_and_simple(self):
        self.assertEqual(sp.crc32(b""), 0x00000000)
        # 已知值：CRC-32 of "a" == 0xE8B7BE43。
        self.assertEqual(sp.crc32(b"a"), 0xE8B7BE43)


class TestFrameChecksum(unittest.TestCase):
    """checksum 等於 binascii.crc32(header_with_zero_checksum ++ payload)。"""

    # 數組輸入：(frame_type, fields, seq, ts)
    CASES = [
        (sp.FRAME_HEARTBEAT, (1234, 7), 42, 1234),
        (sp.FRAME_TELEMETRY, (372, 3300, 55, 2), 10, 2000),
        (sp.FRAME_FAULT_EVENT, (sp.FAULT_CRITICAL, 2, 0x1111, 1500), 3, 1500),
        (sp.FRAME_COMMAND, (sp.CMD_REQUEST_RECOVERY, 0, 350, 0), 0, 2800),
        (sp.FRAME_ACK, (42, 0), 99, 1234),
        (sp.FRAME_NACK, (13, sp.NACK_CHECKSUM, 0), 13, 2900),
        (sp.FRAME_RECOVERY_REPORT, (1, 1, 0, 2800), 5, 2800),
    ]

    def test_checksum_matches_binascii(self):
        for ftype, fields, seq, ts in self.CASES:
            with self.subTest(frame_type=sp.FRAME_TYPE_NAME[ftype]):
                payload = sp.pack_payload(ftype, fields)
                header0 = sp.pack_header(ftype, seq, len(payload), ts, 0)
                expected = binascii.crc32(header0 + payload) & 0xFFFFFFFF

                frame = sp.build_frame(ftype, seq, ts, fields)
                got = sp.parse_header(frame).checksum
                self.assertEqual(got, expected)
                self.assertEqual(sp.validate(frame), sp.VALIDATE_OK)


class TestBitFlipDetection(unittest.TestCase):
    """翻轉任一 byte（payload 或 header，非 checksum 欄位）→ BAD_CHECKSUM。"""

    CASES = [
        (sp.FRAME_HEARTBEAT, (1234, 7), 42, 1234),
        (sp.FRAME_TELEMETRY, (372, 3300, 55, 2), 10, 2000),
        (sp.FRAME_COMMAND, (sp.CMD_INJECT_FAULT, sp.FAULT_TASK_HANG, 0, 0), 4, 100),
    ]

    def test_payload_byte_flip(self):
        for ftype, fields, seq, ts in self.CASES:
            frame = sp.build_frame(ftype, seq, ts, fields)
            for i in range(sp.SAFETY_HEADER_SIZE, len(frame)):
                with self.subTest(frame_type=sp.FRAME_TYPE_NAME[ftype], byte=i):
                    corrupted = bytearray(frame)
                    corrupted[i] ^= 0x01
                    self.assertNotEqual(bytes(corrupted), frame)
                    # checksum 欄位本身不變，但重算後不符 -> BAD_CHECKSUM。
                    self.assertEqual(sp.validate(bytes(corrupted)),
                                     sp.VALIDATE_BAD_CHECKSUM)
                    reason = sp.VALIDATE_TO_NACK[sp.VALIDATE_BAD_CHECKSUM]
                    self.assertEqual(reason, sp.NACK_CHECKSUM)

    def test_header_field_byte_flip(self):
        # 翻轉 header 中 magic/version 以外、且非 checksum 欄位的 byte（如 seq/ts），
        # 應使重算 checksum 不符 -> BAD_CHECKSUM。
        frame = sp.build_frame(sp.FRAME_HEARTBEAT, 42, 1234, (1234, 7))
        # offsets: 0-1 magic, 2 version, 3 type, 4-5 seq, 6-7 len,
        #          8-11 ts, 12-15 checksum
        for i in (3, 4, 5, 8, 9):  # type / seq / ts bytes
            with self.subTest(byte=i):
                corrupted = bytearray(frame)
                corrupted[i] ^= 0x01
                self.assertEqual(sp.validate(bytes(corrupted)),
                                 sp.VALIDATE_BAD_CHECKSUM)

    def test_flip_changes_recomputed_checksum(self):
        # 直接確認：翻轉 payload byte 後，重新計算的 checksum 與原 checksum 不同。
        frame = sp.build_frame(sp.FRAME_TELEMETRY, 10, 2000, (372, 3300, 55, 2))
        orig = sp.parse_header(frame).checksum

        corrupted = bytearray(frame)
        corrupted[sp.SAFETY_HEADER_SIZE] ^= 0xFF
        hdr = sp.parse_header(bytes(corrupted))
        header0 = sp.pack_header(hdr.type, hdr.sequence_id, hdr.payload_length,
                                 hdr.timestamp_ms, 0)
        payload = bytes(corrupted[sp.SAFETY_HEADER_SIZE:
                                  sp.SAFETY_HEADER_SIZE + hdr.payload_length])
        recomputed = sp.compute_checksum(header0, payload)
        self.assertNotEqual(recomputed, orig)


if __name__ == "__main__":
    unittest.main()
