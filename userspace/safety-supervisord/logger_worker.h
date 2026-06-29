// logger_worker.h - events.jsonl 的單一寫入者 (single writer) worker thread
//
// 設計：所有元件透過 thread-safe bounded queue 推送 LogEvent；本 worker 是唯一
// 開檔/寫檔者，逐行寫出並 flush，避免多執行緒交錯。events.jsonl schema 為 LOCKED，
// 鍵集合固定（見規格）。JSON 以手寫方式輸出（無外部 JSON lib）。

#ifndef SAFETY_LOGGER_WORKER_H
#define SAFETY_LOGGER_WORKER_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace safety {

// events.jsonl 的 kind 列舉（字串於序列化時對應）。
enum class LogKind {
    RX_FRAME,
    TX_ACK,
    TX_NACK,
    TX_COMMAND,
    TIMEOUT,
    STATE_CHANGE,
    DUPLICATE,
    CHECKSUM_ERROR,
    RECOVERY_RESULT,
};

const char *log_kind_name(LogKind k);

// 單一 log 事件。未使用的欄位以預設值（空字串 / seq=-1）表示。
struct LogEvent {
    int64_t ts_ms = 0;
    LogKind kind = LogKind::RX_FRAME;
    std::string frame_type;  // "HEARTBEAT".. 或 ""
    int seq = -1;            // -1 表示 N/A
    std::string fault;       // fault 名稱或 ""
    std::string from_state;  // state_change 用
    std::string to_state;    // state_change 用
    std::string detail;      // 自由文字（會被 JSON escape）
};

// 將單一 LogEvent 序列化為一行 JSON（不含換行）。供 logger 與 replay 共用。
std::string serialize_event(const LogEvent &e);

// 多執行緒 logger：背景 thread 消費佇列、單一寫入檔案。
class LoggerWorker {
public:
    // 開啟 report 檔（覆寫）。失敗會在 start() 回報。
    explicit LoggerWorker(std::string path);
    ~LoggerWorker();

    LoggerWorker(const LoggerWorker &) = delete;
    LoggerWorker &operator=(const LoggerWorker &) = delete;

    // 啟動背景 thread。回傳 false 表示檔案開啟失敗。
    bool start();

    // 推入一個事件（thread-safe）。
    void push(LogEvent ev);

    // 停止：標記結束、喚醒、join、把剩餘佇列寫完並關檔。
    void stop();

private:
    void run();

    std::string path_;
    std::ofstream out_;
    std::thread thread_;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<LogEvent> queue_;
    bool stopping_ = false;
    bool started_ = false;
};

} // namespace safety

#endif // SAFETY_LOGGER_WORKER_H
