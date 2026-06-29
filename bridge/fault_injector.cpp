// fault_injector.cpp - fault 注入器實作

#include "fault_injector.h"

#include "safety_protocol.h"

#include <chrono>
#include <cstring>
#include <utility>

namespace bridge {

namespace {
int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char *type_name(uint8_t t)
{
    switch (t) {
    case SAFETY_FRAME_HEARTBEAT:       return "HEARTBEAT";
    case SAFETY_FRAME_TELEMETRY:       return "TELEMETRY";
    case SAFETY_FRAME_FAULT_EVENT:     return "FAULT_EVENT";
    case SAFETY_FRAME_COMMAND:         return "COMMAND";
    case SAFETY_FRAME_ACK:             return "ACK";
    case SAFETY_FRAME_NACK:            return "NACK";
    case SAFETY_FRAME_RECOVERY_REPORT: return "RECOVERY_REPORT";
    default:                           return "UNKNOWN";
    }
}
} // namespace

FaultInjector::FaultInjector(InjectorConfig cfg) : cfg_(std::move(cfg))
{
    if (!cfg_.log_path.empty()) {
        log_.open(cfg_.log_path, std::ios::out | std::ios::trunc);
        if (log_.is_open())
            log_ << "ts_ms,dir,type,seq,len,action\n";
    }
}

FaultInjector::~FaultInjector()
{
    if (log_.is_open()) {
        log_.flush();
        log_.close();
    }
}

void FaultInjector::log_line(int64_t ts_ms, const std::string &dir,
                             uint8_t type, uint16_t seq, size_t len,
                             const char *action)
{
    if (!log_.is_open())
        return;
    log_ << ts_ms << ',' << dir << ',' << type_name(type) << ',' << seq << ','
         << len << ',' << action << '\n';
    log_.flush();
}

bool FaultInjector::process(std::vector<uint8_t> &frame,
                            const std::string &dir, int &out_delay_ms)
{
    out_delay_ms = 0;
    ++frame_counter_;

    // 解出 header（frame 已是完整一個 frame）。
    SafetyFrameHeader hdr{};
    if (frame.size() >= SAFETY_HEADER_SIZE)
        std::memcpy(&hdr, frame.data(), SAFETY_HEADER_SIZE);

    const uint8_t type = hdr.type;
    const uint16_t seq = hdr.sequence_id;
    const int64_t ts = now_ms();

    // 1) drop 判斷：型別限定（若有），且符合 drop-every 週期。
    bool type_match = (cfg_.drop_type < 0) ||
                      (type == static_cast<uint8_t>(cfg_.drop_type));
    if (cfg_.drop_every > 0 && type_match &&
        (frame_counter_ % cfg_.drop_every) == 0) {
        log_line(ts, dir, type, seq, frame.size(), "DROP");
        return false; // 不轉發
    }

    // 2) corrupt 判斷：損壞 checksum（翻轉低位元組）。
    const char *action = "FORWARD";
    if (cfg_.corrupt_every > 0 && (frame_counter_ % cfg_.corrupt_every) == 0 &&
        frame.size() >= SAFETY_HEADER_SIZE) {
        // checksum 欄位位於 header 尾端 4 bytes（offset 12..15）。
        // 直接翻轉一個 bit 使 CRC 不符。
        frame[12] ^= 0xFFu;
        action = "CORRUPT";
    }

    // 3) delay：回填給呼叫端 sleep。
    if (cfg_.delay_ms > 0) {
        out_delay_ms = cfg_.delay_ms;
        if (std::strcmp(action, "FORWARD") == 0)
            action = "DELAY";
    }

    log_line(ts, dir, type, seq, frame.size(), action);
    return true; // 轉發
}

} // namespace bridge
