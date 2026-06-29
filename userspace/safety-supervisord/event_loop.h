// event_loop.h - LIVE 模式 epoll 事件迴圈
//
// 監聽：device fd 的可讀、一個 timerfd 作為 heartbeat 逾時的備援
//（kernel hrtimer 也會合成 FAULT_EVENT 逾時，但 host 端再加一層備援），
// 以及一個 eventfd 供 recovery worker 回報時喚醒 epoll。
//
// 收到的位元組交給 FrameAssembler -> 對應 FSM 事件，並把 log/recovery 事件分別
// 推送到 logger / recovery worker。本類別不擁有 worker，只持有其參考。

#ifndef SAFETY_EVENT_LOOP_H
#define SAFETY_EVENT_LOOP_H

#include "device_client.h"
#include "health_state_machine.h"
#include "logger_worker.h"
#include "protocol.h"
#include "recovery_worker.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

namespace safety {

class EventLoop {
public:
    EventLoop(DeviceClient &dev, HealthStateMachine &fsm,
              LoggerWorker &logger, RecoveryWorker &recovery,
              uint32_t heartbeat_timeout_ms);
    ~EventLoop();

    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    // 建立 epoll / timerfd / eventfd 並註冊。回傳 false 表示初始化失敗。
    bool setup();

    // 執行迴圈。duration_ms==0 表示直到 EOF/訊號；否則跑約 N ms 後返回。
    // running flag 由外部訊號處理器清除以請求停止。
    void run(uint32_t duration_ms, std::atomic<bool> &running);

    // 供 recovery worker callback 用：推入回報並喚醒 epoll（thread-safe）。
    void post_recovery_report(const RecoveryReport &rep);

private:
    void handle_readable(int fd);
    void process_frames(std::vector<ParsedFrame> &frames);
    void drain_recovery_reports();
    void on_heartbeat_timeout();
    void arm_heartbeat_timer();

    int64_t now_ms() const;

    DeviceClient &dev_;
    HealthStateMachine &fsm_;
    LoggerWorker &logger_;
    RecoveryWorker &recovery_;
    uint32_t hb_timeout_ms_;

    UniqueFd epoll_;
    UniqueFd timer_;   // timerfd：heartbeat 逾時備援
    UniqueFd wake_;    // eventfd：recovery 回報喚醒

    FrameAssembler assembler_;
    DuplicateDetector dup_;
    RetryTracker retry_;

    uint16_t tx_seq_ = 0; // 我方送出 frame 的序號

    std::mutex report_mtx_;
    std::deque<RecoveryReport> reports_;
};

} // namespace safety

#endif // SAFETY_EVENT_LOOP_H
