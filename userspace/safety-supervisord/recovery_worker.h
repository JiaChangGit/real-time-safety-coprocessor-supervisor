// recovery_worker.h - Recovery worker thread
//
// 角色：當 FSM 進入 DEGRADED 時，event loop 透過 thread-safe queue 請求本 worker
// 送出 REQUEST_RECOVERY command，並啟動 recovery deadline。worker 先回報
// RECOVERY_SENT（讓 FSM DEGRADED -> RECOVERING），其後依結果回報 recovery
// 成功（HEARTBEAT_RESTORED）或失敗（RECOVERY_FAILED）。
//
// 與 event loop 的溝通完全透過兩條 bounded queue：
//   - 請求佇列（event loop -> worker）：RecoveryRequest
//   - 回報佇列（worker -> event loop）：RecoveryReport，由 event loop poll/drain。

#ifndef SAFETY_RECOVERY_WORKER_H
#define SAFETY_RECOVERY_WORKER_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace safety {

// worker 對外回報的事件種類。
enum class RecoveryOutcome {
    SENT,     // 已送出 REQUEST_RECOVERY -> FSM RECOVERY_SENT
    SUCCESS,  // recovery 成功 -> FSM HEARTBEAT_RESTORED
    FAILED,   // recovery 逾時/失敗 -> FSM RECOVERY_FAILED
};

struct RecoveryReport {
    RecoveryOutcome outcome;
    uint16_t seq;        // 相關 command sequence
    int64_t ts_ms;       // 事件時間（monotonic-ish ms）
};

class RecoveryWorker {
public:
    // send_command：worker 用來把 REQUEST_RECOVERY frame 寫進 /dev/safety_copro
    // 的 TX queue。report_cb：worker 把 RecoveryReport 推回給
    // event loop（通常 enqueue 到 event loop 的回報佇列並喚醒 epoll）。
    using SendCommandFn = std::function<bool(const std::vector<uint8_t> &)>;
    using ReportFn = std::function<void(const RecoveryReport &)>;

    RecoveryWorker(SendCommandFn send_cmd, ReportFn report,
                   uint32_t deadline_ms);
    ~RecoveryWorker();

    RecoveryWorker(const RecoveryWorker &) = delete;
    RecoveryWorker &operator=(const RecoveryWorker &) = delete;

    void start();
    void stop();

    // event loop 在 FSM 進入 DEGRADED 時呼叫：請求一次 recovery。
    void request_recovery(uint16_t seq, int64_t now_ms);

    // event loop 在 RECOVERING 期間收到正常 heartbeat 時呼叫：標記成功，
    // worker 將回報 SUCCESS（取消 deadline）。
    void notify_heartbeat_restored(int64_t now_ms);

private:
    struct Request {
        enum class Kind { RECOVER, RESTORED } kind;
        uint16_t seq;
        int64_t ts_ms;
    };

    void run();

    SendCommandFn send_cmd_;
    ReportFn report_;
    uint32_t deadline_ms_;

    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    bool stopping_ = false;

    // worker 內部狀態：是否有進行中的 recovery 與其 deadline / seq。
    bool active_ = false;
    uint16_t active_seq_ = 0;
    int64_t deadline_at_ms_ = 0;
};

} // namespace safety

#endif // SAFETY_RECOVERY_WORKER_H
