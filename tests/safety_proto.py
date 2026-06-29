"""safety_proto - 純 Python 鏡像實作 safety_protocol.h 的線格式 (wire format)。

此模組為 single source of truth (`userspace/common/safety_protocol.h`) 的純 Python
對應，供測試套件使用。所有結構皆 little-endian、packed，並與 C header byte-identical。

設計重點：
  - Header 16 bytes，struct format ``<HBBHHII``。
  - checksum 使用 CRC-32 (poly 0xEDB88320)，等同 Python ``binascii.crc32``。
  - validate() 回傳與 C header ``enum safety_validate_result`` 相同的結果碼。

無任何外部相依 (no external deps)，僅使用標準函式庫 (struct / binascii)。
"""

import binascii
import struct

# ---- 協定常數 (protocol constants) ----
SAFETY_PROTO_MAGIC = 0x5346      # 'S','F' little-endian
SAFETY_PROTO_VERSION = 0x01
SAFETY_MAX_PAYLOAD = 256
SAFETY_HEADER_SIZE = 16
SAFETY_MAX_FRAME_SIZE = SAFETY_HEADER_SIZE + SAFETY_MAX_PAYLOAD

# header struct: magic(u16) version(u8) type(u8) sequence_id(u16)
#                payload_length(u16) timestamp_ms(u32) checksum(u32)
_HEADER_FMT = "<HBBHHII"
assert struct.calcsize(_HEADER_FMT) == SAFETY_HEADER_SIZE

# ---- Frame 型別 (header.type) ----
FRAME_HEARTBEAT = 0x01
FRAME_TELEMETRY = 0x02
FRAME_FAULT_EVENT = 0x03
FRAME_COMMAND = 0x04
FRAME_ACK = 0x05
FRAME_NACK = 0x06
FRAME_RECOVERY_REPORT = 0x07

FRAME_TYPE_NAME = {
    FRAME_HEARTBEAT: "HEARTBEAT",
    FRAME_TELEMETRY: "TELEMETRY",
    FRAME_FAULT_EVENT: "FAULT_EVENT",
    FRAME_COMMAND: "COMMAND",
    FRAME_ACK: "ACK",
    FRAME_NACK: "NACK",
    FRAME_RECOVERY_REPORT: "RECOVERY_REPORT",
}

# ---- Command 型別 (CommandPayload.command) ----
CMD_GET_STATUS = 0x01
CMD_INJECT_FAULT = 0x02
CMD_REQUEST_RECOVERY = 0x03
CMD_ENTER_SAFE_MODE = 0x04
CMD_RESET_FAULT_STATE = 0x05

# ---- Fault 型別 (INJECT_FAULT 與 FAULT_EVENT 共用) ----
FAULT_NONE = 0x00
FAULT_TASK_HANG = 0x01
FAULT_CHECKSUM_ERROR = 0x02
FAULT_CRITICAL = 0x03

FAULT_TYPE_NAME = {
    FAULT_NONE: "NONE",
    FAULT_TASK_HANG: "HEARTBEAT_STOP",
    FAULT_CHECKSUM_ERROR: "CHECKSUM_ERROR",
    FAULT_CRITICAL: "CRITICAL",
}

# ---- NACK 原因碼 (nack reasons) ----
NACK_CHECKSUM = 0x01
NACK_VERSION = 0x02
NACK_SEQUENCE = 0x03
NACK_UNKNOWN_TYPE = 0x04
NACK_PAYLOAD = 0x05

# ---- 驗證結果碼 (enum safety_validate_result) ----
VALIDATE_OK = 0
VALIDATE_BAD_MAGIC = 1
VALIDATE_BAD_VERSION = 2
VALIDATE_BAD_LENGTH = 3
VALIDATE_BAD_CHECKSUM = 4

# 驗證失敗結果碼 -> 對應 NACK 原因碼。BAD_LENGTH 對應 PAYLOAD。
VALIDATE_TO_NACK = {
    VALIDATE_BAD_CHECKSUM: NACK_CHECKSUM,
    VALIDATE_BAD_VERSION: NACK_VERSION,
    VALIDATE_BAD_LENGTH: NACK_PAYLOAD,
}

# ---- Payload struct formats（皆 little-endian、packed）----
PAYLOAD_FMT = {
    FRAME_HEARTBEAT: "<II",        # uptime_ms, beat_seq
    FRAME_TELEMETRY: "<HHHH",      # temp_c_x10, voltage_mv, cpu_load_pct, fault_count
    FRAME_FAULT_EVENT: "<BBHI",    # fault_type, severity, detail_code, fault_uptime_ms
    FRAME_COMMAND: "<BBHI",        # command, arg8, arg16, arg32
    FRAME_ACK: "<HH",              # acked_seq, reserved
    FRAME_NACK: "<HBB",            # nacked_seq, reason, reserved
    FRAME_RECOVERY_REPORT: "<BBHI",  # recovered, new_state, reserved, recovery_uptime_ms
}


def crc32(data):
    """標準 CRC-32 (IEEE 802.3)，等同 ``binascii.crc32``。回傳 unsigned 32-bit。"""
    return binascii.crc32(data) & 0xFFFFFFFF


def compute_checksum(header_bytes_no_checksum, payload):
    """對 (checksum 欄位歸零的 16-byte header) ++ payload 做 CRC-32。

    等價於 C 端 ``safety_frame_compute_checksum``。
    """
    return crc32(header_bytes_no_checksum + payload)


def pack_header(frame_type, sequence_id, payload_length, timestamp_ms, checksum=0):
    """封裝 16-byte header（checksum 預設為 0，供計算用）。"""
    return struct.pack(
        _HEADER_FMT,
        SAFETY_PROTO_MAGIC,
        SAFETY_PROTO_VERSION,
        frame_type,
        sequence_id,
        payload_length,
        timestamp_ms,
        checksum,
    )


def pack_payload(frame_type, fields):
    """依 frame_type 對應的 struct format 封裝 payload。"""
    fmt = PAYLOAD_FMT[frame_type]
    return struct.pack(fmt, *fields)


def pack_frame(frame_type, sequence_id, timestamp_ms, payload):
    """封裝完整 frame：填好 checksum 的 header ++ payload。

    payload 為已封裝的 bytes（呼叫端可用 :func:`pack_payload`）。
    """
    payload_length = len(payload)
    header0 = pack_header(frame_type, sequence_id, payload_length, timestamp_ms, 0)
    checksum = compute_checksum(header0, payload)
    header = pack_header(frame_type, sequence_id, payload_length, timestamp_ms, checksum)
    return header + payload


def build_frame(frame_type, sequence_id, timestamp_ms, fields):
    """便利函式：以 payload 欄位 tuple 直接組成完整 frame。"""
    return pack_frame(frame_type, sequence_id, timestamp_ms, pack_payload(frame_type, fields))


class ParsedHeader:
    """解析後的 header 欄位容器。"""

    __slots__ = (
        "magic",
        "version",
        "type",
        "sequence_id",
        "payload_length",
        "timestamp_ms",
        "checksum",
    )

    def __init__(self, magic, version, type_, sequence_id, payload_length,
                 timestamp_ms, checksum):
        self.magic = magic
        self.version = version
        self.type = type_
        self.sequence_id = sequence_id
        self.payload_length = payload_length
        self.timestamp_ms = timestamp_ms
        self.checksum = checksum

    def __eq__(self, other):
        if not isinstance(other, ParsedHeader):
            return NotImplemented
        return all(getattr(self, s) == getattr(other, s) for s in self.__slots__)

    def __repr__(self):
        return (
            "ParsedHeader(magic=0x{:04x}, version={}, type={}, seq={}, "
            "payload_length={}, timestamp_ms={}, checksum=0x{:08x})".format(
                self.magic, self.version, self.type, self.sequence_id,
                self.payload_length, self.timestamp_ms, self.checksum)
        )


def parse_header(buf):
    """解析 16-byte header，回傳 :class:`ParsedHeader`。"""
    if len(buf) < SAFETY_HEADER_SIZE:
        raise ValueError("buffer too short for header")
    fields = struct.unpack(_HEADER_FMT, buf[:SAFETY_HEADER_SIZE])
    return ParsedHeader(*fields)


def parse_payload(frame_type, payload):
    """依 frame_type 解析 payload，回傳欄位 tuple。"""
    fmt = PAYLOAD_FMT[frame_type]
    return struct.unpack(fmt, payload)


class ParsedFrame:
    """解析後的完整 frame：header + payload bytes + 解析後欄位。"""

    __slots__ = ("header", "payload", "fields")

    def __init__(self, header, payload, fields):
        self.header = header
        self.payload = payload
        self.fields = fields


def parse_frame(buf):
    """解析完整 frame buffer，回傳 :class:`ParsedFrame`。

    僅在 frame_type 已知且長度足夠時解析 payload 欄位，否則 fields 為 None。
    """
    header = parse_header(buf)
    payload = bytes(buf[SAFETY_HEADER_SIZE:SAFETY_HEADER_SIZE + header.payload_length])
    fields = None
    if header.type in PAYLOAD_FMT and len(payload) == struct.calcsize(PAYLOAD_FMT[header.type]):
        fields = parse_payload(header.type, payload)
    return ParsedFrame(header, payload, fields)


def validate(buf):
    """驗證 frame buffer，回傳與 C header 相同的結果碼。

    對應 C 端 ``safety_frame_validate``：依序檢查 length / magic / version /
    payload_length / 完整長度 / checksum。
    """
    if len(buf) < SAFETY_HEADER_SIZE:
        return VALIDATE_BAD_LENGTH
    header = parse_header(buf)
    if header.magic != SAFETY_PROTO_MAGIC:
        return VALIDATE_BAD_MAGIC
    if header.version != SAFETY_PROTO_VERSION:
        return VALIDATE_BAD_VERSION
    if header.payload_length > SAFETY_MAX_PAYLOAD:
        return VALIDATE_BAD_LENGTH
    if len(buf) < SAFETY_HEADER_SIZE + header.payload_length:
        return VALIDATE_BAD_LENGTH
    header0 = pack_header(
        header.type, header.sequence_id, header.payload_length,
        header.timestamp_ms, 0)
    payload = buf[SAFETY_HEADER_SIZE:SAFETY_HEADER_SIZE + header.payload_length]
    want = compute_checksum(header0, payload)
    if want != header.checksum:
        return VALIDATE_BAD_CHECKSUM
    return VALIDATE_OK
