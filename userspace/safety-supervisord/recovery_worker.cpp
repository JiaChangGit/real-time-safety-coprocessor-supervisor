// recovery_worker.cpp - Recovery worker thread 實作

#include "recovery_worker.h"
#include "protocol.h"
#include "safety_protocol.h"

#include <chrono>
#include <utility>

namespace safety {

RecoveryWorker::RecoveryWorker(SendCommandFn send_cmd, ReportFn report,
                               uint32_t deadline_ms)
    : send_cmd_(std::move(send_cmd)), report_(std::move(report)),
      deadline_ms_(deadline_ms)
{
}

RecoveryWorker::~RecoveryWorker()
{
    stop();
}

void RecoveryWorker::start()
{
    thread_ = std::thread(&RecoveryWorker::run, this);
}

void RecoveryWorker::stop()
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
}

void RecoveryWorker::request_recovery(uint16_t seq, int64_t now_ms)
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back({Request::Kind::RECOVER, seq, now_ms});
    }
    cv_.notify_one();
}

void RecoveryWorker::notify_heartbeat_restored(int64_t now_ms)
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back({Request::Kind::RESTORED, 0, now_ms});
    }
    cv_.notify_one();
}

void RecoveryWorker::run()
{
    std::unique_lock<std::mutex> lk(mtx_);
    for (;;) {
        if (stopping_)
            return;

        // 計算下一次喚醒時機：若有進行中的 recovery，需在 deadline 醒來檢查逾時。
        if (queue_.empty()) {
            if (active_) {
                auto until = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(deadline_ms_);
                cv_.wait_until(lk, until);
            } else {
                cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
            }
            if (stopping_)
                return;
        }

        // 1) 處理所有待辦請求。
        while (!queue_.empty()) {
            Request req = queue_.front();
            queue_.pop_front();

            if (req.kind == Request::Kind::RECOVER) {
                // 解鎖後送 command（避免在持鎖時做 I/O / 觸發回呼）。
                uint16_t seq = req.seq;
                int64_t ts = req.ts_ms;
                active_ = true;
                active_seq_ = seq;
                deadline_at_ms_ = ts + static_cast<int64_t>(deadline_ms_);

                lk.unlock();
                std::vector<uint8_t> cmd = build_command(
                    seq, static_cast<uint32_t>(ts),
                    SAFETY_CMD_REQUEST_RECOVERY, SAFETY_FAULT_NONE, 0, 0);
                bool sent = send_cmd_ ? send_cmd_(cmd) : false;
                if (sent && report_)
                    report_({RecoveryOutcome::SENT, seq, ts});
                lk.lock();
            } else { // RESTORED
                if (active_) {
                    active_ = false;
                    uint16_t seq = active_seq_;
                    int64_t ts = req.ts_ms;
                    lk.unlock();
                    if (report_)
                        report_({RecoveryOutcome::SUCCESS, seq, ts});
                    lk.lock();
                }
            }
        }

        // 2) 檢查 recovery deadline 是否逾時 -> 回報 FAILED。
        if (active_) {
            int64_t now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            // 以 wall-ish steady time 與 deadline 比較；deadline 以請求時的虛擬
            // 時間 + deadline_ms_ 設定，但 live 模式以真實逝去時間判斷較直覺。
            // 為避免兩種時鐘混用，這裡以「自 wait 起算」逾時為準：若已 active 且
            // 本輪非由新請求喚醒（佇列空），即視為到期。
            (void)now_ms;
            if (queue_.empty()) {
                active_ = false;
                uint16_t seq = active_seq_;
                int64_t ts = deadline_at_ms_;
                lk.unlock();
                if (report_)
                    report_({RecoveryOutcome::FAILED, seq, ts});
                lk.lock();
            }
        }
    }
}

} // namespace safety
