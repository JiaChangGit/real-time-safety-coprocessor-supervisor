/*
 * safety_protocol.h - Real-Time Safety Co-Processor Supervisor 共用通訊協定
 *
 * 這是整個專案的「單一事實來源 (single source of truth)」。
 * Linux kernel driver (C)、Zephyr firmware (C)、C++ userspace 三端共用「位元組
 * 完全一致」的線格式 (wire format)。本檔案會被同步複製到三個 consumer 位置：
 *
 *   - include/safety_protocol.h                    (此檔，canonical 來源)
 *   - userspace/common/safety_protocol.h           (C++ userspace/bridge copy)
 *   - linux/drivers/safety_copro/safety_protocol.h (kernel 內建 driver)
 *   - zephyr/safety_copro_firmware/include/...      (Zephyr firmware)
 *
 * consumer copy 必須與此檔 byte-identical；scripts/05_build_userspace.sh 會做一致性檢查。
 *
 * 設計重點：
 *   1. 整數寬度型別抽象 (safety_u8/16/32)，讓同一份 header 可在 kernel space、
 *      Zephyr、C++17 三種環境「不修改」直接編譯（kernel 無 <stdint.h>）。
 *   2. 線格式固定 little-endian。本專案三端 (x86_64 host、ARM64 Linux guest、
 *      ARM64 Zephyr) 皆為 little-endian，因此線上不做 byte swap，僅在文件中註明。
 *   3. checksum 使用標準 CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320)，
 *      與 zlib / Python binascii.crc32 完全相容，方便跨語言驗證 (tests/)。
 */

#ifndef SAFETY_PROTOCOL_H
#define SAFETY_PROTOCOL_H

/* ---- 整數型別抽象：核心空間沒有 <stdint.h> ---- */
#if defined(__KERNEL__)
#  include <linux/types.h>
#  include <linux/string.h>          /* memcpy / memset */
typedef u8  safety_u8;
typedef u16 safety_u16;
typedef u32 safety_u32;
#else
#  include <stdint.h>
#  include <stddef.h>
#  include <string.h>
typedef uint8_t  safety_u8;
typedef uint16_t safety_u16;
typedef uint32_t safety_u32;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 協定常數 ---- */
#define SAFETY_PROTO_MAGIC        0x5346u   /* 'S','F' little-endian */
#define SAFETY_PROTO_VERSION      0x01u
#define SAFETY_MAX_PAYLOAD        256u
#define SAFETY_HEADER_SIZE        16u       /* sizeof(SafetyFrameHeader) */
#define SAFETY_MAX_FRAME_SIZE     (SAFETY_HEADER_SIZE + SAFETY_MAX_PAYLOAD)

/* ---- Frame 型別 (header.type) ---- */
enum safety_frame_type {
    SAFETY_FRAME_HEARTBEAT       = 0x01,
    SAFETY_FRAME_TELEMETRY       = 0x02,
    SAFETY_FRAME_FAULT_EVENT     = 0x03,
    SAFETY_FRAME_COMMAND         = 0x04,
    SAFETY_FRAME_ACK             = 0x05,
    SAFETY_FRAME_NACK            = 0x06,
    SAFETY_FRAME_RECOVERY_REPORT = 0x07,
};

/* ---- Command 型別 (CommandPayload.command) ---- */
enum safety_command_type {
    SAFETY_CMD_GET_STATUS            = 0x01,
    SAFETY_CMD_INJECT_FAULT          = 0x02,
    SAFETY_CMD_REQUEST_RECOVERY      = 0x03,
    SAFETY_CMD_ENTER_SAFE_MODE       = 0x04,
    SAFETY_CMD_RESET_FAULT_STATE     = 0x05,
};

/* ---- Fault 型別 (INJECT_FAULT 與 FAULT_EVENT 共用) ---- */
enum safety_fault_type {
    SAFETY_FAULT_NONE            = 0x00,
    SAFETY_FAULT_TASK_HANG       = 0x01,  /* heartbeat_stop */
    SAFETY_FAULT_CHECKSUM_ERROR  = 0x02,  /* checksum_error_response */
    SAFETY_FAULT_CRITICAL        = 0x03,
};

/* ---- NACK 原因碼 ---- */
enum safety_nack_reason {
    SAFETY_NACK_CHECKSUM     = 0x01,
    SAFETY_NACK_VERSION      = 0x02,
    SAFETY_NACK_SEQUENCE     = 0x03,
    SAFETY_NACK_UNKNOWN_TYPE = 0x04,
    SAFETY_NACK_PAYLOAD      = 0x05,
};

/* ---- Frame Header：固定 16 bytes，packed ---- */
#if defined(__GNUC__)
#  define SAFETY_PACKED __attribute__((packed))
#else
#  define SAFETY_PACKED
#endif

struct SAFETY_PACKED SafetyFrameHeader {
    safety_u16 magic;          /* SAFETY_PROTO_MAGIC */
    safety_u8  version;        /* SAFETY_PROTO_VERSION */
    safety_u8  type;           /* enum safety_frame_type */
    safety_u16 sequence_id;    /* 單調遞增序號，用於 ACK/NACK/duplicate 偵測 */
    safety_u16 payload_length; /* payload bytes，<= SAFETY_MAX_PAYLOAD */
    safety_u32 timestamp_ms;   /* 送出端 uptime (ms) */
    safety_u32 checksum;       /* CRC-32 over (header[checksum=0] ++ payload) */
};

/* ---- Payload 結構（皆為固定大小，packed）---- */
struct SAFETY_PACKED SafetyHeartbeatPayload {
    safety_u32 uptime_ms;
    safety_u32 beat_seq;
};

struct SAFETY_PACKED SafetyTelemetryPayload {
    safety_u16 temperature_c_x10; /* 攝氏溫度 x10，例如 372 = 37.2C */
    safety_u16 voltage_mv;
    safety_u16 cpu_load_pct;      /* 0..100 */
    safety_u16 fault_count;
};

struct SAFETY_PACKED SafetyFaultEventPayload {
    safety_u8  fault_type;        /* enum safety_fault_type */
    safety_u8  severity;          /* 0=info 1=warn 2=critical */
    safety_u16 detail_code;
    safety_u32 fault_uptime_ms;
};

struct SAFETY_PACKED SafetyCommandPayload {
    safety_u8  command;           /* enum safety_command_type */
    safety_u8  arg8;              /* 例如 INJECT_FAULT 時的 fault_type */
    safety_u16 arg16;             /* 例如 SET_HEARTBEAT_INTERVAL 的 ms */
    safety_u32 arg32;
};

struct SAFETY_PACKED SafetyAckPayload {
    safety_u16 acked_sequence_id;
    safety_u16 reserved;
};

struct SAFETY_PACKED SafetyNackPayload {
    safety_u16 nacked_sequence_id;
    safety_u8  reason;            /* enum safety_nack_reason */
    safety_u8  reserved;
};

struct SAFETY_PACKED SafetyRecoveryReportPayload {
    safety_u8  recovered;         /* 1 = 成功恢復, 0 = 進入 safe mode */
    safety_u8  new_state;         /* co-processor 回報的內部狀態 */
    safety_u16 reserved;
    safety_u32 recovery_uptime_ms;
};

/* ===================================================================
 *  CRC-32 (IEEE 802.3) - 與 zlib / Python binascii.crc32 完全相容
 *  reflected input/output, poly 0xEDB88320, init 0xFFFFFFFF,
 *  final xor 0xFFFFFFFF。三端共用同一份 inline 實作以保證一致。
 * =================================================================== */

/* 不套用 init/final 的單步更新，可跨多段 buffer 累加（與 zlib 連續呼叫等價）。 */
static inline safety_u32 safety_crc32_step(safety_u32 crc,
                                           const void *data, size_t len)
{
    const safety_u8 *p = (const safety_u8 *)data;
    size_t i;
    int k;
    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++) {
            /* (~(crc & 1u) + 1u) 等於 -(crc & 1u)，避免 signed 溢位 UB */
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
        }
    }
    return crc;
}

/* 單一連續 buffer 的標準 CRC-32。 */
static inline safety_u32 safety_crc32(const void *data, size_t len)
{
    return safety_crc32_step(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

/*
 * 計算 frame checksum：對 (header 之 checksum 欄位歸零後的 16 bytes) 再串接
 * payload 一起做 CRC-32。等價於 Python:
 *   binascii.crc32(header_with_zero_checksum + payload) & 0xFFFFFFFF
 */
static inline safety_u32
safety_frame_compute_checksum(const struct SafetyFrameHeader *hdr,
                              const void *payload, safety_u16 payload_len)
{
    struct SafetyFrameHeader tmp = *hdr;
    safety_u32 crc;
    tmp.checksum = 0;
    crc = safety_crc32_step(0xFFFFFFFFu, &tmp, SAFETY_HEADER_SIZE);
    if (payload && payload_len)
        crc = safety_crc32_step(crc, payload, payload_len);
    return crc ^ 0xFFFFFFFFu;
}

/* ---- Helper：初始化 header（呼叫端後續再填 checksum）---- */
static inline void safety_frame_init(struct SafetyFrameHeader *hdr,
                                     safety_u8 type, safety_u16 sequence_id,
                                     safety_u16 payload_len, safety_u32 ts_ms)
{
    hdr->magic = SAFETY_PROTO_MAGIC;
    hdr->version = SAFETY_PROTO_VERSION;
    hdr->type = type;
    hdr->sequence_id = sequence_id;
    hdr->payload_length = payload_len;
    hdr->timestamp_ms = ts_ms;
    hdr->checksum = 0;
}

/* ---- Helper：填好 payload 後封裝完整 frame，回傳總長度 ---- *
 * out 必須至少 SAFETY_MAX_FRAME_SIZE。失敗 (payload 過長) 回傳 0。      */
static inline size_t safety_frame_pack(safety_u8 *out,
                                       struct SafetyFrameHeader *hdr,
                                       const void *payload)
{
    if (hdr->payload_length > SAFETY_MAX_PAYLOAD)
        return 0;
    hdr->checksum = safety_frame_compute_checksum(hdr, payload,
                                                  hdr->payload_length);
    memcpy(out, hdr, SAFETY_HEADER_SIZE);
    if (payload && hdr->payload_length)
        memcpy(out + SAFETY_HEADER_SIZE, payload, hdr->payload_length);
    return SAFETY_HEADER_SIZE + hdr->payload_length;
}

/* ---- 驗證結果碼 ---- */
enum safety_validate_result {
    SAFETY_VALIDATE_OK            = 0,
    SAFETY_VALIDATE_BAD_MAGIC     = 1,
    SAFETY_VALIDATE_BAD_VERSION   = 2,
    SAFETY_VALIDATE_BAD_LENGTH    = 3,
    SAFETY_VALIDATE_BAD_CHECKSUM  = 4,
};

/*
 * 驗證一段已含完整 header(+payload) 的 buffer。
 * buf_len 是可用位元組數；payload 緊接於 header 之後。
 */
static inline int safety_frame_validate(const safety_u8 *buf, size_t buf_len)
{
    struct SafetyFrameHeader hdr;
    safety_u32 want;
    if (buf_len < SAFETY_HEADER_SIZE)
        return SAFETY_VALIDATE_BAD_LENGTH;
    memcpy(&hdr, buf, SAFETY_HEADER_SIZE);
    if (hdr.magic != SAFETY_PROTO_MAGIC)
        return SAFETY_VALIDATE_BAD_MAGIC;
    if (hdr.version != SAFETY_PROTO_VERSION)
        return SAFETY_VALIDATE_BAD_VERSION;
    if (hdr.payload_length > SAFETY_MAX_PAYLOAD)
        return SAFETY_VALIDATE_BAD_LENGTH;
    if (buf_len < (size_t)SAFETY_HEADER_SIZE + hdr.payload_length)
        return SAFETY_VALIDATE_BAD_LENGTH;
    want = safety_frame_compute_checksum(&hdr, buf + SAFETY_HEADER_SIZE,
                                         hdr.payload_length);
    if (want != hdr.checksum)
        return SAFETY_VALIDATE_BAD_CHECKSUM;
    return SAFETY_VALIDATE_OK;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SAFETY_PROTOCOL_H */
