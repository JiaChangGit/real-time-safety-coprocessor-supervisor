// logger_worker.cpp - logger worker thread 與 JSONL 序列化實作

#include "logger_worker.h"

#include <utility>

namespace safety {

const char *log_kind_name(LogKind k)
{
    switch (k) {
    case LogKind::RX_FRAME:        return "rx_frame";
    case LogKind::TX_ACK:          return "tx_ack";
    case LogKind::TX_NACK:         return "tx_nack";
    case LogKind::TX_COMMAND:      return "tx_command";
    case LogKind::TIMEOUT:         return "timeout";
    case LogKind::STATE_CHANGE:    return "state_change";
    case LogKind::DUPLICATE:       return "duplicate";
    case LogKind::CHECKSUM_ERROR:  return "checksum_error";
    case LogKind::RECOVERY_RESULT: return "recovery_result";
    }
    return "unknown";
}

namespace {
// 將字串以 JSON 規則 escape 後附加到 out（含必要的控制字元處理）。
void append_json_escaped(std::string &out, const std::string &s)
{
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                // 其他控制字元以 \u00XX 形式輸出。
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[(c >> 4) & 0xF];
                out += hex[c & 0xF];
            } else {
                out += c;
            }
        }
    }
}
} // namespace

std::string serialize_event(const LogEvent &e)
{
    // 鍵順序與集合為 LOCKED schema：
    // ts_ms, kind, frame_type, seq, fault, from_state, to_state, detail
    std::string s;
    s.reserve(160);
    s += "{\"ts_ms\":";
    s += std::to_string(e.ts_ms);
    s += ",\"kind\":\"";
    s += log_kind_name(e.kind);
    s += "\",\"frame_type\":\"";
    append_json_escaped(s, e.frame_type);
    s += "\",\"seq\":";
    s += std::to_string(e.seq);
    s += ",\"fault\":\"";
    append_json_escaped(s, e.fault);
    s += "\",\"from_state\":\"";
    append_json_escaped(s, e.from_state);
    s += "\",\"to_state\":\"";
    append_json_escaped(s, e.to_state);
    s += "\",\"detail\":\"";
    append_json_escaped(s, e.detail);
    s += "\"}";
    return s;
}

LoggerWorker::LoggerWorker(std::string path) : path_(std::move(path)) {}

LoggerWorker::~LoggerWorker()
{
    // 保險：若呼叫端未顯式 stop()，析構時收尾，避免 thread 懸置。
    stop();
}

bool LoggerWorker::start()
{
    out_.open(path_, std::ios::out | std::ios::trunc);
    if (!out_.is_open())
        return false;
    started_ = true;
    thread_ = std::thread(&LoggerWorker::run, this);
    return true;
}

void LoggerWorker::push(LogEvent ev)
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back(std::move(ev));
    }
    cv_.notify_one();
}

void LoggerWorker::stop()
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopping_)
            return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
    started_ = false;
}

void LoggerWorker::run()
{
    for (;;) {
        std::deque<LogEvent> batch;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
            // 取出目前所有待寫事件（批次處理降低鎖競爭）。
            batch.swap(queue_);
            // 若已要求停止且無殘留則結束；否則先寫完這批再判斷。
            if (stopping_ && batch.empty())
                return;
        }
        for (const auto &ev : batch) {
            out_ << serialize_event(ev) << '\n';
            out_.flush(); // 規格要求逐行 flush
        }
    }
}

} // namespace safety
