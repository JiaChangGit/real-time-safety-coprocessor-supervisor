// protocol.cpp - C++ 協定包裝層實作

#include "protocol.h"

#include <algorithm>
#include <cstring>

namespace safety {

const char *frame_type_name(uint8_t type)
{
    switch (type) {
    case SAFETY_FRAME_HEARTBEAT:       return "HEARTBEAT";
    case SAFETY_FRAME_TELEMETRY:       return "TELEMETRY";
    case SAFETY_FRAME_FAULT_EVENT:     return "FAULT_EVENT";
    case SAFETY_FRAME_COMMAND:         return "COMMAND";
    case SAFETY_FRAME_ACK:             return "ACK";
    case SAFETY_FRAME_NACK:            return "NACK";
    case SAFETY_FRAME_RECOVERY_REPORT: return "RECOVERY_REPORT";
    default:                           return ""; // 未知型別 -> 空字串
    }
}

const char *fault_type_name(uint8_t fault)
{
    switch (fault) {
    case SAFETY_FAULT_NONE:             return "NONE";
    case SAFETY_FAULT_TASK_HANG:        return "HEARTBEAT_STOP";
    case SAFETY_FAULT_CHECKSUM_ERROR:   return "CHECKSUM_ERROR";
    case SAFETY_FAULT_CRITICAL:         return "CRITICAL";
    default:                            return "";
    }
}

// ---- FrameAssembler ----------------------------------------------------

void FrameAssembler::feed(const uint8_t *data, size_t len,
                          std::vector<ParsedFrame> &out)
{
    // 將新位元組附加到殘留緩衝後，反覆嘗試取出 frame。
    buf_.insert(buf_.end(), data, data + len);

    size_t pos = 0; // buf_ 中目前的解析游標

    for (;;) {
        // 1) hunt magic：丟棄無法構成合法 magic 起點的位元組。
        //    magic 為 little-endian 0x5346 -> 位元組序列 0x46 0x53。
        while (pos + 2 <= buf_.size()) {
            uint16_t m;
            std::memcpy(&m, buf_.data() + pos, sizeof(m));
            if (m == SAFETY_PROTO_MAGIC)
                break;
            ++pos; // 對齊：前移一個 byte 重新嘗試
        }
        // 不足以判斷 magic：保留殘留。
        if (pos + 2 > buf_.size())
            break;

        // 2) 需要完整 header 才能讀 payload_length。
        if (pos + SAFETY_HEADER_SIZE > buf_.size())
            break;

        SafetyFrameHeader hdr;
        std::memcpy(&hdr, buf_.data() + pos, SAFETY_HEADER_SIZE);

        // payload_length 過大屬於 BAD_LENGTH：作為一個錯誤 frame 回報，
        // 並跳過此 header（前移 1 byte 重新 hunt，避免吃掉後續真實 frame）。
        if (hdr.payload_length > SAFETY_MAX_PAYLOAD) {
            ParsedFrame f;
            f.header = hdr;
            f.validate_result = SAFETY_VALIDATE_BAD_LENGTH;
            out.push_back(std::move(f));
            ++pos;
            continue;
        }

        const size_t total = SAFETY_HEADER_SIZE + hdr.payload_length;
        // 3) 需要完整 payload。
        if (pos + total > buf_.size())
            break;

        // 4) 驗證整個 frame。
        ParsedFrame f;
        f.header = hdr;
        f.payload.assign(buf_.data() + pos + SAFETY_HEADER_SIZE,
                         buf_.data() + pos + total);
        f.validate_result =
            safety_frame_validate(buf_.data() + pos, total);
        out.push_back(std::move(f));

        pos += total; // 前進到下一個 frame
    }

    // 移除已消化的位元組，保留尚未組成 frame 的殘留。
    if (pos > 0)
        buf_.erase(buf_.begin(), buf_.begin() + pos);
}

// ---- Frame 建構器 ------------------------------------------------------

namespace {
// 共用封裝流程：填 header、套 payload、計算 checksum，回傳完整位元組。
std::vector<uint8_t> pack_frame(uint8_t type, uint16_t seq, uint32_t ts_ms,
                                const void *payload, uint16_t payload_len)
{
    SafetyFrameHeader hdr;
    safety_frame_init(&hdr, type, seq, payload_len, ts_ms);

    std::vector<uint8_t> out(SAFETY_HEADER_SIZE + payload_len);
    // safety_frame_pack 會計算 checksum 並寫入 out（out 需 >= MAX_FRAME_SIZE
    // 之要求僅在 payload 可能達上限時；此處 out 已精確配置足夠長度）。
    hdr.checksum =
        safety_frame_compute_checksum(&hdr, payload, payload_len);
    std::memcpy(out.data(), &hdr, SAFETY_HEADER_SIZE);
    if (payload && payload_len)
        std::memcpy(out.data() + SAFETY_HEADER_SIZE, payload, payload_len);
    return out;
}
} // namespace

std::vector<uint8_t> build_heartbeat(uint16_t seq, uint32_t ts_ms,
                                     uint32_t uptime_ms, uint32_t beat_seq)
{
    SafetyHeartbeatPayload p{};
    p.uptime_ms = uptime_ms;
    p.beat_seq = beat_seq;
    return pack_frame(SAFETY_FRAME_HEARTBEAT, seq, ts_ms, &p, sizeof(p));
}

std::vector<uint8_t> build_ack(uint16_t seq, uint32_t ts_ms,
                               uint16_t acked_seq)
{
    SafetyAckPayload p{};
    p.acked_sequence_id = acked_seq;
    p.reserved = 0;
    return pack_frame(SAFETY_FRAME_ACK, seq, ts_ms, &p, sizeof(p));
}

std::vector<uint8_t> build_nack(uint16_t seq, uint32_t ts_ms,
                                uint16_t nacked_seq, uint8_t reason)
{
    SafetyNackPayload p{};
    p.nacked_sequence_id = nacked_seq;
    p.reason = reason;
    p.reserved = 0;
    return pack_frame(SAFETY_FRAME_NACK, seq, ts_ms, &p, sizeof(p));
}

std::vector<uint8_t> build_command(uint16_t seq, uint32_t ts_ms,
                                   uint8_t command, uint8_t arg8,
                                   uint16_t arg16, uint32_t arg32)
{
    SafetyCommandPayload p{};
    p.command = command;
    p.arg8 = arg8;
    p.arg16 = arg16;
    p.arg32 = arg32;
    return pack_frame(SAFETY_FRAME_COMMAND, seq, ts_ms, &p, sizeof(p));
}

// ---- DuplicateDetector -------------------------------------------------

bool DuplicateDetector::is_duplicate(uint8_t type, uint16_t seq)
{
    auto key = std::make_pair(type, seq);
    // insert 回傳 .second == false 表示鍵已存在 -> 重複。
    return !seen_.insert(key).second;
}

// ---- RetryTracker ------------------------------------------------------

unsigned RetryTracker::bump(uint16_t seq)
{
    for (auto &e : entries_) {
        if (e.first == seq)
            return ++e.second;
    }
    entries_.emplace_back(seq, 1u);
    return 1u;
}

unsigned RetryTracker::count(uint16_t seq) const
{
    for (const auto &e : entries_) {
        if (e.first == seq)
            return e.second;
    }
    return 0u;
}

void RetryTracker::clear(uint16_t seq)
{
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [seq](const std::pair<uint16_t, unsigned> &e) {
                                      return e.first == seq;
                                  }),
                   entries_.end());
}

} // namespace safety
