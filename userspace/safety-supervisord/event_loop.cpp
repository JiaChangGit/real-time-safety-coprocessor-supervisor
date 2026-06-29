// event_loop.cpp - LIVE 模式 epoll 事件迴圈實作

#include "event_loop.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace safety {

EventLoop::EventLoop(DeviceClient &dev, HealthStateMachine &fsm,
                     LoggerWorker &logger, RecoveryWorker &recovery,
                     uint32_t heartbeat_timeout_ms)
    : dev_(dev), fsm_(fsm), logger_(logger), recovery_(recovery),
      hb_timeout_ms_(heartbeat_timeout_ms)
{
}

EventLoop::~EventLoop() = default;

int64_t EventLoop::now_ms() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool EventLoop::setup()
{
    int ep = ::epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0)
        return false;
    epoll_.reset(ep);

    int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tfd < 0)
        return false;
    timer_.reset(tfd);

    int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (efd < 0)
        return false;
    wake_.reset(efd);

    auto add = [&](int fd) {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        return ::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &ev) == 0;
    };

    if (!add(dev_.device_fd()))
        return false;
    if (!add(timer_.get()))
        return false;
    if (!add(wake_.get()))
        return false;

    arm_heartbeat_timer();
    return true;
}

void EventLoop::arm_heartbeat_timer()
{
    // 設定 one-shot 計時器：hb_timeout_ms_ 後到期（每次收到 heartbeat 重置）。
    itimerspec its{};
    its.it_value.tv_sec = hb_timeout_ms_ / 1000;
    its.it_value.tv_nsec = (hb_timeout_ms_ % 1000) * 1000000L;
    ::timerfd_settime(timer_.get(), 0, &its, nullptr);
}

void EventLoop::post_recovery_report(const RecoveryReport &rep)
{
    {
        std::lock_guard<std::mutex> lk(report_mtx_);
        reports_.push_back(rep);
    }
    uint64_t one = 1;
    ssize_t n = ::write(wake_.get(), &one, sizeof(one));
    (void)n; // eventfd 寫入失敗極罕見；epoll 仍會於下次 readable 處理
}

void EventLoop::run(uint32_t duration_ms, std::atomic<bool> &running)
{
    const int64_t start = now_ms();
    std::array<epoll_event, 8> events;

    while (running.load()) {
        int timeout = 200; // ms：定期回頭檢查 running / duration
        if (duration_ms > 0) {
            int64_t elapsed = now_ms() - start;
            if (elapsed >= static_cast<int64_t>(duration_ms))
                break;
            int remaining = static_cast<int>(duration_ms - elapsed);
            if (remaining < timeout)
                timeout = remaining;
        }

        int n = ::epoll_wait(epoll_.get(), events.data(),
                             static_cast<int>(events.size()), timeout);
        if (n < 0) {
            if (errno == EINTR)
                continue; // 被訊號打斷
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == timer_.get()) {
                uint64_t exp;
                while (::read(timer_.get(), &exp, sizeof(exp)) == sizeof(exp)) {
                }
                on_heartbeat_timeout();
            } else if (fd == wake_.get()) {
                uint64_t val;
                while (::read(wake_.get(), &val, sizeof(val)) == sizeof(val)) {
                }
                drain_recovery_reports();
            } else {
                handle_readable(fd);
            }
        }
    }
}

void EventLoop::handle_readable(int fd)
{
    std::array<uint8_t, 1024> buf;
    ssize_t n = dev_.read_some(fd, buf.data(), buf.size());
    if (n <= 0)
        return; // 0=EOF / <0=error，由上層 running 控制退出

    std::vector<ParsedFrame> frames;
    assembler_.feed(buf.data(), static_cast<size_t>(n), frames);
    process_frames(frames);
}

void EventLoop::process_frames(std::vector<ParsedFrame> &frames)
{
    for (auto &f : frames) {
        const char *ftname = frame_type_name(f.type());
        const int64_t ts = static_cast<int64_t>(f.ts_ms());

        // checksum / 驗證失敗：記 checksum_error 並回 NACK。
        if (!f.ok()) {
            LogEvent le;
            le.ts_ms = ts;
            le.kind = LogKind::CHECKSUM_ERROR;
            le.frame_type = ftname;
            le.seq = f.seq();
            le.detail = "validate_result=" + std::to_string(f.validate_result);
            logger_.push(le);

            auto nack = build_nack(tx_seq_++, static_cast<uint32_t>(ts),
                                   f.seq(), SAFETY_NACK_CHECKSUM);
            dev_.send_out(nack.data(), nack.size());
            LogEvent tx;
            tx.ts_ms = ts;
            tx.kind = LogKind::TX_NACK;
            tx.frame_type = "NACK";
            tx.seq = f.seq();
            logger_.push(tx);
            continue;
        }

        // 重複偵測。
        if (dup_.is_duplicate(f.type(), f.seq())) {
            LogEvent le;
            le.ts_ms = ts;
            le.kind = LogKind::DUPLICATE;
            le.frame_type = ftname;
            le.seq = f.seq();
            logger_.push(le);
            continue;
        }

        // 記 rx_frame。
        LogEvent rx;
        rx.ts_ms = ts;
        rx.kind = LogKind::RX_FRAME;
        rx.frame_type = ftname;
        rx.seq = f.seq();
        logger_.push(rx);

        // 依型別驅動 FSM。
        if (f.type() == SAFETY_FRAME_HEARTBEAT) {
            arm_heartbeat_timer(); // 收到 heartbeat 重置逾時計時器
            HealthState before = fsm_.state();
            HealthEvent ev = (before == HealthState::RECOVERING)
                                 ? HealthEvent::HEARTBEAT_RESTORED
                                 : HealthEvent::HEARTBEAT_OK;
            fsm_.apply(ev);
            if (before == HealthState::RECOVERING)
                recovery_.notify_heartbeat_restored(ts);

            // 回 ACK。
            auto ack = build_ack(tx_seq_++, static_cast<uint32_t>(ts), f.seq());
            dev_.send_out(ack.data(), ack.size());
            LogEvent tx;
            tx.ts_ms = ts;
            tx.kind = LogKind::TX_ACK;
            tx.frame_type = "ACK";
            tx.seq = f.seq();
            logger_.push(tx);
        } else if (f.type() == SAFETY_FRAME_FAULT_EVENT) {
            // severity==2 (critical) -> CRITICAL_FAULT。
            uint8_t fault = 0, severity = 0;
            if (f.payload.size() >= sizeof(SafetyFaultEventPayload)) {
                SafetyFaultEventPayload p{};
                std::memcpy(&p, f.payload.data(), sizeof(p));
                fault = p.fault_type;
                severity = p.severity;
            }
            rx.fault = fault_type_name(fault); // 補記 fault（rx 已送，這裡僅本地）
            if (severity >= 2) {
                fsm_.apply(HealthEvent::CRITICAL_FAULT);
            } else {
                // 非 critical fault 在 HEALTHY 下視為 heartbeat 逾時類退化訊號。
                if (fsm_.state() == HealthState::HEALTHY)
                    fsm_.apply(HealthEvent::HEARTBEAT_TIMEOUT);
            }
        }
        // TELEMETRY / ACK / NACK / RECOVERY_REPORT：僅記錄，不驅動 FSM。
    }
}

void EventLoop::on_heartbeat_timeout()
{
    const int64_t ts = now_ms();
    LogEvent le;
    le.ts_ms = ts;
    le.kind = LogKind::TIMEOUT;
    le.detail = "heartbeat timeout (backup timer)";
    logger_.push(le);

    HealthState before = fsm_.state();
    auto r = fsm_.apply(HealthEvent::HEARTBEAT_TIMEOUT);
    if (r.valid && before == HealthState::HEALTHY) {
        // 進入 DEGRADED：請 recovery worker 送 REQUEST_RECOVERY。
        recovery_.request_recovery(tx_seq_++, ts);
    }
    // 重新武裝計時器，持續監測。
    arm_heartbeat_timer();
}

void EventLoop::drain_recovery_reports()
{
    std::deque<RecoveryReport> local;
    {
        std::lock_guard<std::mutex> lk(report_mtx_);
        local.swap(reports_);
    }
    for (const auto &rep : local) {
        const char *result = "";
        switch (rep.outcome) {
        case RecoveryOutcome::SENT: {
            // 記 tx_command（REQUEST_RECOVERY 已由 worker 送出）。
            LogEvent tx;
            tx.ts_ms = rep.ts_ms;
            tx.kind = LogKind::TX_COMMAND;
            tx.frame_type = "COMMAND";
            tx.seq = rep.seq;
            tx.detail = "REQUEST_RECOVERY";
            logger_.push(tx);
            fsm_.apply(HealthEvent::RECOVERY_SENT);
            result = "RECOVERY_SENT";
            break;
        }
        case RecoveryOutcome::SUCCESS:
            // FSM 已在收到 heartbeat 時轉回 HEALTHY；此處僅記 recovery_result。
            result = "RECOVERY_SUCCESS";
            break;
        case RecoveryOutcome::FAILED:
            fsm_.apply(HealthEvent::RECOVERY_FAILED);
            result = "RECOVERY_FAILED";
            break;
        }
        LogEvent le;
        le.ts_ms = rep.ts_ms;
        le.kind = LogKind::RECOVERY_RESULT;
        le.seq = rep.seq;
        le.detail = result;
        logger_.push(le);
    }
}

} // namespace safety
