// main.cpp - safety-supervisord 進入點
//
// 職責：參數解析、模式分派 (live/mock/replay)、訊號處理、組裝各元件。
// 三種模式互斥：預設 live；--mock-device 走 mock；--replay 走 replay。
//
// 注意：所有對外輸出（log/stderr）一律英文，避免 mojibake。

#include "device_client.h"
#include "event_loop.h"
#include "health_state_machine.h"
#include "logger_worker.h"
#include "protocol.h"
#include "recovery_worker.h"
#include "replay_mode.h"
#include "safety_protocol.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

// 全域停止旗標，由 SIGINT/SIGTERM 清除（async-signal-safe）。
std::atomic<bool> g_running{true};

void on_signal(int)
{
    g_running.store(false);
}

void print_help(const char *prog)
{
    std::cout <<
        "Usage: " << prog << " [options]\n"
        "\n"
        "Real-Time Safety Co-Processor Supervisor daemon.\n"
        "\n"
        "Mode (choose exactly one; default is live):\n"
        "  --device PATH            live: open the char device (default /dev/safety_copro)\n"
        "  --mock-device FILE       read concatenated binary frames from FILE\n"
        "  --replay FILE            replay an events.jsonl (deterministic, single-thread)\n"
        "\n"
        "Options:\n"
        "  --report-dir DIR         output directory for reports (default reports)\n"
        "  --heartbeat-timeout-ms N heartbeat timeout in ms (default 350)\n"
        "  --duration-ms N          live/mock: run N ms then exit; 0 = until EOF/signal\n"
        "  --help                   show this help and exit\n";
}

struct Args {
    enum class Mode { LIVE, MOCK, REPLAY } mode = Mode::LIVE;
    std::string device = "/dev/safety_copro";
    std::string mock_file;
    std::string replay_file;
    std::string report_dir = "reports";
    uint32_t hb_timeout_ms = 350;
    uint32_t duration_ms = 0;
};

// 解析參數；回傳 false 表示參數錯誤（已印 stderr）。help 透過 out_help 回報。
bool parse_args(int argc, char **argv, Args &a, bool &out_help)
{
    out_help = false;
    int mode_count = 0;
    auto need_value = [&](int &i, const char *opt) -> const char * {
        if (i + 1 >= argc) {
            std::cerr << "error: option " << opt << " requires a value\n";
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string opt = argv[i];
        if (opt == "--help" || opt == "-h") {
            out_help = true;
            return true;
        } else if (opt == "--device") {
            const char *v = need_value(i, "--device");
            if (!v) return false;
            a.device = v;
        } else if (opt == "--mock-device") {
            const char *v = need_value(i, "--mock-device");
            if (!v) return false;
            a.mock_file = v;
            a.mode = Args::Mode::MOCK;
            ++mode_count;
        } else if (opt == "--replay") {
            const char *v = need_value(i, "--replay");
            if (!v) return false;
            a.replay_file = v;
            a.mode = Args::Mode::REPLAY;
            ++mode_count;
        } else if (opt == "--report-dir") {
            const char *v = need_value(i, "--report-dir");
            if (!v) return false;
            a.report_dir = v;
        } else if (opt == "--heartbeat-timeout-ms") {
            const char *v = need_value(i, "--heartbeat-timeout-ms");
            if (!v) return false;
            a.hb_timeout_ms = static_cast<uint32_t>(std::stoul(v));
        } else if (opt == "--duration-ms") {
            const char *v = need_value(i, "--duration-ms");
            if (!v) return false;
            a.duration_ms = static_cast<uint32_t>(std::stoul(v));
        } else {
            std::cerr << "error: unknown option '" << opt << "'\n";
            return false;
        }
    }

    if (mode_count > 1) {
        std::cerr << "error: --mock-device and --replay are mutually exclusive\n";
        return false;
    }
    return true;
}

void install_signals()
{
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // 不設 SA_RESTART：讓 epoll_wait 因 EINTR 返回以檢查旗標
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// ---- MOCK 模式：單執行緒、虛擬時鐘 ----
// 讀整個檔案、跑 streaming assembler，依 frame timestamp 推動虛擬時鐘；
// 連續 HEARTBEAT 之 ts 間隔 > timeout 時對 FSM 注入 HEARTBEAT_TIMEOUT。
int run_mock(const Args &a)
{
    using namespace safety;

    std::ifstream in(a.mock_file, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "mock: cannot open '" << a.mock_file << "'\n";
        return 1;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    in.close();

    const std::string out_path = a.report_dir + "/events.jsonl";
    LoggerWorker logger(out_path);
    if (!logger.start()) {
        std::cerr << "mock: cannot open report file '" << out_path << "'\n";
        return 1;
    }

    HealthStateMachine fsm;
    // state_change hook -> logger。
    fsm.set_hook([&](const TransitionResult &r) {
        LogEvent le;
        le.kind = LogKind::STATE_CHANGE;
        le.from_state = state_name(r.from);
        le.to_state = state_name(r.to);
        le.detail = event_name(r.event);
        logger.push(le);
    });

    FrameAssembler assembler;
    DuplicateDetector dup;

    std::vector<ParsedFrame> frames;
    assembler.feed(data.data(), data.size(), frames);

    bool have_last_hb = false;
    uint32_t last_hb_ts = 0;
    bool in_recovery = false; // 模擬：DEGRADED 後送 recovery 再等 heartbeat
    uint16_t tx_seq = 0;

    long nack_count = 0;

    for (auto &f : frames) {
        const int64_t ts = static_cast<int64_t>(f.ts_ms());
        const char *ftname = frame_type_name(f.type());

        if (!f.ok()) {
            // checksum/驗證錯誤：checksum_error + NACK 計數 + 仍做重複偵測。
            LogEvent le;
            le.ts_ms = ts;
            le.kind = LogKind::CHECKSUM_ERROR;
            le.frame_type = ftname;
            le.seq = f.seq();
            le.detail = "validate_result=" + std::to_string(f.validate_result);
            logger.push(le);

            ++nack_count;
            LogEvent tx;
            tx.ts_ms = ts;
            tx.kind = LogKind::TX_NACK;
            tx.frame_type = "NACK";
            tx.seq = f.seq();
            tx.detail = "nack #" + std::to_string(nack_count);
            logger.push(tx);
            (void)dup.is_duplicate(f.type(), f.seq()); // 記入重複偵測表
            continue;
        }

        if (dup.is_duplicate(f.type(), f.seq())) {
            LogEvent le;
            le.ts_ms = ts;
            le.kind = LogKind::DUPLICATE;
            le.frame_type = ftname;
            le.seq = f.seq();
            logger.push(le);
            continue;
        }

        // rx_frame。
        LogEvent rx;
        rx.ts_ms = ts;
        rx.kind = LogKind::RX_FRAME;
        rx.frame_type = ftname;
        rx.seq = f.seq();
        if (f.type() == SAFETY_FRAME_FAULT_EVENT &&
            f.payload.size() >= sizeof(SafetyFaultEventPayload)) {
            SafetyFaultEventPayload p{};
            std::memcpy(&p, f.payload.data(), sizeof(p));
            rx.fault = fault_type_name(p.fault_type);
        }
        logger.push(rx);

        if (f.type() == SAFETY_FRAME_HEARTBEAT) {
            // 虛擬時鐘：與上一個 heartbeat 的間隔。
            if (have_last_hb) {
                uint32_t gap = f.ts_ms() - last_hb_ts;
                if (gap > a.hb_timeout_ms) {
                    // 偵測到 heartbeat gap：注入 timeout，FSM HEALTHY->DEGRADED。
                    LogEvent to;
                    to.ts_ms = ts;
                    to.kind = LogKind::TIMEOUT;
                    to.frame_type = "HEARTBEAT";
                    to.detail = "gap=" + std::to_string(gap) +
                                "ms > " + std::to_string(a.hb_timeout_ms) + "ms";
                    logger.push(to);

                    HealthState before = fsm.state();
                    auto r = fsm.apply(HealthEvent::HEARTBEAT_TIMEOUT);
                    if (r.valid && before == HealthState::HEALTHY) {
                        // 送 REQUEST_RECOVERY -> DEGRADED->RECOVERING。
                        LogEvent cmd;
                        cmd.ts_ms = ts;
                        cmd.kind = LogKind::TX_COMMAND;
                        cmd.frame_type = "COMMAND";
                        cmd.seq = tx_seq++;
                        cmd.detail = "REQUEST_RECOVERY";
                        logger.push(cmd);
                        fsm.apply(HealthEvent::RECOVERY_SENT);
                        in_recovery = true;
                    }
                }
            }
            have_last_hb = true;
            last_hb_ts = f.ts_ms();

            // heartbeat 推進 FSM：RECOVERING->HEALTHY 或 BOOTING->HEALTHY。
            HealthState before = fsm.state();
            HealthEvent ev = (before == HealthState::RECOVERING)
                                 ? HealthEvent::HEARTBEAT_RESTORED
                                 : HealthEvent::HEARTBEAT_OK;
            fsm.apply(ev);
            if (in_recovery && before == HealthState::RECOVERING) {
                LogEvent rr;
                rr.ts_ms = ts;
                rr.kind = LogKind::RECOVERY_RESULT;
                rr.detail = "RECOVERY_SUCCESS";
                logger.push(rr);
                in_recovery = false;
            }

            // 回 ACK。
            LogEvent ack;
            ack.ts_ms = ts;
            ack.kind = LogKind::TX_ACK;
            ack.frame_type = "ACK";
            ack.seq = f.seq();
            logger.push(ack);
        } else if (f.type() == SAFETY_FRAME_FAULT_EVENT) {
            SafetyFaultEventPayload p{};
            uint8_t severity = 0;
            if (f.payload.size() >= sizeof(p)) {
                std::memcpy(&p, f.payload.data(), sizeof(p));
                severity = p.severity;
            }
            if (severity >= 2)
                fsm.apply(HealthEvent::CRITICAL_FAULT);
        }
        // TELEMETRY / 未知型別已在上面以 rx_frame 記錄（未知型別 frame_type=""）。
    }

    logger.stop();

    std::cout << "==== Mock Run Summary ====\n";
    std::cout << "input            : " << a.mock_file << "\n";
    std::cout << "report           : " << out_path << "\n";
    std::cout << "frames parsed    : " << frames.size() << "\n";
    std::cout << "nack (bad frames): " << nack_count << "\n";
    std::cout << "final state      : " << state_name(fsm.state()) << "\n";
    std::cout << "==========================\n";
    return 0;
}

// ---- LIVE 模式 ----
int run_live(const Args &a)
{
    using namespace safety;

    DeviceClient dev;
    if (!dev.open_device(a.device)) {
        std::cerr << "live: " << dev.last_error() << "\n";
        return 1;
    }

    const std::string out_path = a.report_dir + "/events.jsonl";
    LoggerWorker logger(out_path);
    if (!logger.start()) {
        std::cerr << "live: cannot open report file '" << out_path << "'\n";
        return 1;
    }

    HealthStateMachine fsm;
    fsm.set_hook([&](const TransitionResult &r) {
        LogEvent le;
        le.kind = LogKind::STATE_CHANGE;
        le.from_state = state_name(r.from);
        le.to_state = state_name(r.to);
        le.detail = event_name(r.event);
        logger.push(le);
    });

    // 先建 event loop（recovery worker 的 report callback 需指向它）。
    // 用指標延後綁定。
    EventLoop *loop_ptr = nullptr;
    RecoveryWorker recovery(
        [&dev](const std::vector<uint8_t> &cmd) {
            return dev.send_out(cmd.data(), cmd.size());
        },
        [&loop_ptr](const RecoveryReport &rep) {
            if (loop_ptr)
                loop_ptr->post_recovery_report(rep);
        },
        a.hb_timeout_ms);

    EventLoop loop(dev, fsm, logger, recovery, a.hb_timeout_ms);
    loop_ptr = &loop;

    if (!loop.setup()) {
        std::cerr << "live: failed to set up epoll event loop\n";
        logger.stop();
        return 1;
    }

    recovery.start();
    std::cout << "safety-supervisord: live mode on '" << a.device << "'";
    std::cout << ", heartbeat timeout " << a.hb_timeout_ms << "ms\n";

    loop.run(a.duration_ms, g_running);

    recovery.stop();
    logger.stop();
    std::cout << "safety-supervisord: shutdown complete, final state "
              << state_name(fsm.state()) << "\n";
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    Args a;
    bool want_help = false;
    if (!parse_args(argc, argv, a, want_help)) {
        std::cerr << "Run '" << argv[0] << " --help' for usage.\n";
        return 2;
    }
    if (want_help) {
        print_help(argv[0]);
        return 0;
    }

    install_signals();

    switch (a.mode) {
    case Args::Mode::REPLAY:
        return safety::run_replay(a.replay_file, a.report_dir);
    case Args::Mode::MOCK:
        return run_mock(a);
    case Args::Mode::LIVE:
    default:
        return run_live(a);
    }
}
