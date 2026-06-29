// protocol.h - C++ 包裝層，建立於共用 safety_protocol.h 之上
//
// 提供：
//   - ParsedFrame：解析後的單一 frame（header + payload copy + 驗證結果）。
//   - FrameAssembler：streaming 組裝器，從任意 byte 串流 hunt magic / 讀 header /
//     讀 payload / 驗證，產出 ParsedFrame 序列（對應 mock/live 兩種輸入）。
//   - 各種 frame 建構器 (HEARTBEAT/ACK/NACK/COMMAND)。
//   - 依 (type,sequence_id) 的重複偵測與 retry bookkeeping。

#ifndef SAFETY_PROTOCOL_WRAPPER_H
#define SAFETY_PROTOCOL_WRAPPER_H

#include "safety_protocol.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace safety {

// 單一已解析 frame。payload 為 header 之後的位元組複本。
struct ParsedFrame {
    SafetyFrameHeader header{};
    std::vector<uint8_t> payload;
    int validate_result = SAFETY_VALIDATE_OK; // enum safety_validate_result

    bool ok() const { return validate_result == SAFETY_VALIDATE_OK; }
    uint8_t type() const { return header.type; }
    uint16_t seq() const { return header.sequence_id; }
    uint32_t ts_ms() const { return header.timestamp_ms; }
};

// frame_type 數值 -> 字串（events.jsonl 用）。未知型別回傳空字串 ""。
const char *frame_type_name(uint8_t type);

// fault_type 數值 -> 字串（events.jsonl 的 "fault" 欄位用）。
const char *fault_type_name(uint8_t fault);

// Streaming 組裝器：餵入任意位元組，逐步吐出完整（或驗證失敗）的 frame。
//
// 設計：以「hunt magic -> 讀滿 16-byte header -> 依 payload_length 讀滿 payload
// -> 驗證」的狀態流推進。對於 magic 不符的雜訊位元組，逐 byte 丟棄重新對齊。
class FrameAssembler {
public:
    // 餵入一段位元組，將完整 frame 追加到 out（含驗證失敗者，其 validate_result
    // 會標記原因；呼叫端據此記 checksum_error / NACK）。
    void feed(const uint8_t *data, size_t len, std::vector<ParsedFrame> &out);

private:
    std::vector<uint8_t> buf_; // 尚未組成完整 frame 的殘留位元組
};

// ---- Frame 建構器：回傳完整封裝（header+payload，checksum 已填）的位元組 ----
std::vector<uint8_t> build_heartbeat(uint16_t seq, uint32_t ts_ms,
                                     uint32_t uptime_ms, uint32_t beat_seq);
std::vector<uint8_t> build_ack(uint16_t seq, uint32_t ts_ms,
                               uint16_t acked_seq);
std::vector<uint8_t> build_nack(uint16_t seq, uint32_t ts_ms,
                                uint16_t nacked_seq, uint8_t reason);
std::vector<uint8_t> build_command(uint16_t seq, uint32_t ts_ms,
                                   uint8_t command, uint8_t arg8,
                                   uint16_t arg16, uint32_t arg32);

// ---- 重複偵測：以 (type,sequence_id) 為鍵 ----
class DuplicateDetector {
public:
    // 第一次見到回傳 false（非重複），之後相同鍵回傳 true。
    bool is_duplicate(uint8_t type, uint16_t seq);

private:
    std::set<std::pair<uint8_t, uint16_t>> seen_;
};

// ---- Retry bookkeeping：記錄某 sequence 的重送次數 ----
class RetryTracker {
public:
    // 增加並回傳該 seq 目前的 retry 次數。
    unsigned bump(uint16_t seq);
    unsigned count(uint16_t seq) const;
    void clear(uint16_t seq);

private:
    std::vector<std::pair<uint16_t, unsigned>> entries_;
};

} // namespace safety

#endif // SAFETY_PROTOCOL_WRAPPER_H
