// replay_mode.cpp - REPLAY 模式實作（單執行緒、確定性）

#include "replay_mode.h"

#include "health_state_machine.h"
#include "logger_worker.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

namespace safety {

namespace {

// 極簡 JSON 取值：從一行 JSONL 中抓 "key":"value"（字串）或 "key":number。
// schema 為 LOCKED 且扁平（無巢狀、無陣列），故此最小 parser 足夠且確定性。
bool extract_string(const std::string &line, const std::string &key,
                    std::string &out)
{
    std::string pat = "\"" + key + "\":\"";
    size_t p = line.find(pat);
    if (p == std::string::npos)
        return false;
    p += pat.size();
    std::string v;
    while (p < line.size()) {
        char c = line[p];
        if (c == '\\' && p + 1 < line.size()) {
            char nx = line[p + 1];
            switch (nx) {
            case 'n': v += '\n'; break;
            case 'r': v += '\r'; break;
            case 't': v += '\t'; break;
            case '"': v += '"'; break;
            case '\\': v += '\\'; break;
            default: v += nx; break;
            }
            p += 2;
            continue;
        }
        if (c == '"')
            break;
        v += c;
        ++p;
    }
    out = v;
    return true;
}

bool extract_int(const std::string &line, const std::string &key, long &out)
{
    std::string pat = "\"" + key + "\":";
    size_t p = line.find(pat);
    if (p == std::string::npos)
        return false;
    p += pat.size();
    if (p < line.size() && line[p] == '"')
        return false; // 這是字串欄位，不是數字
    size_t start = p;
    if (p < line.size() && (line[p] == '-' || line[p] == '+'))
        ++p;
    while (p < line.size() && line[p] >= '0' && line[p] <= '9')
        ++p;
    if (p == start)
        return false;
    out = std::stol(line.substr(start, p - start));
    return true;
}

} // namespace

int run_replay(const std::string &input_path, const std::string &report_dir)
{
    std::ifstream in(input_path);
    if (!in.is_open()) {
        std::cerr << "replay: cannot open input '" << input_path << "'\n";
        return 1;
    }

    const std::string out_path = report_dir + "/replay_events.jsonl";
    std::ofstream out(out_path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "replay: cannot open output '" << out_path << "'\n";
        return 1;
    }

    HealthStateMachine fsm;

    // 統計：每個目標狀態被進入的次數 + 合法轉移總數。
    std::array<long, 6> state_entry_counts{}; // index = (int)HealthState
    long transitions = 0;
    long lines_total = 0;
    long rx_frames = 0;
    long timeouts = 0;
    long ignored = 0;

    // transition hook：每次合法轉移寫一行 state_change，並更新統計。
    fsm.set_hook([&](const TransitionResult &r) {
        ++transitions;
        ++state_entry_counts[static_cast<int>(r.to)];
        LogEvent le;
        le.kind = LogKind::STATE_CHANGE;
        le.from_state = state_name(r.from);
        le.to_state = state_name(r.to);
        le.detail = event_name(r.event);
        out << serialize_event(le) << '\n';
    });

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        ++lines_total;

        std::string kind;
        if (!extract_string(line, "kind", kind))
            continue;

        long ts = 0;
        extract_int(line, "ts_ms", ts);

        if (kind == "rx_frame") {
            ++rx_frames;
            std::string ftype;
            extract_string(line, "frame_type", ftype);

            if (ftype == "HEARTBEAT") {
                // 依當前狀態映射：RECOVERING -> HEARTBEAT_RESTORED，否則 HEARTBEAT_OK。
                HealthEvent ev = (fsm.state() == HealthState::RECOVERING)
                                     ? HealthEvent::HEARTBEAT_RESTORED
                                     : HealthEvent::HEARTBEAT_OK;
                fsm.apply(ev);
            } else if (ftype == "FAULT_EVENT") {
                // critical severity -> CRITICAL_FAULT。以 fault 欄位輔助判斷；
                // detail 可能含 severity 標記。此處保守：fault 為 critical 類即觸發。
                std::string fault;
                extract_string(line, "fault", fault);
                std::string detail;
                extract_string(line, "detail", detail);
                bool critical = detail.find("critical") != std::string::npos ||
                                detail.find("severity=2") != std::string::npos;
                if (critical)
                    fsm.apply(HealthEvent::CRITICAL_FAULT);
                // 非 critical 的 FAULT_EVENT 不直接驅動 FSM（與 live 一致性由
                // 上游 timeout 事件負責）。
            }
            // 其他 frame_type（TELEMETRY/ACK/NACK/...）忽略。
        } else if (kind == "timeout") {
            ++timeouts;
            fsm.apply(HealthEvent::HEARTBEAT_TIMEOUT);
        } else if (kind == "tx_command") {
            // tx_command 可能是 REQUEST_RECOVERY：在 DEGRADED 下推進到 RECOVERING，
            // 讓 replay 能重建 DEGRADED->RECOVERING->HEALTHY 的完整軌跡。
            std::string detail;
            extract_string(line, "detail", detail);
            if (detail.find("REQUEST_RECOVERY") != std::string::npos)
                fsm.apply(HealthEvent::RECOVERY_SENT);
            else
                ++ignored;
        } else {
            // state_change / tx_ack / tx_nack / duplicate / checksum_error /
            // recovery_result 等輸出類：replay 忽略（不重複驅動 FSM）。
            ++ignored;
        }
    }

    out.flush();
    out.close();

    // ---- 英文摘要表 ----
    std::cout << "==== Replay Summary ====\n";
    std::cout << "input              : " << input_path << "\n";
    std::cout << "output             : " << out_path << "\n";
    std::cout << "lines read         : " << lines_total << "\n";
    std::cout << "rx_frame events    : " << rx_frames << "\n";
    std::cout << "timeout events     : " << timeouts << "\n";
    std::cout << "ignored events     : " << ignored << "\n";
    std::cout << "valid transitions  : " << transitions << "\n";
    std::cout << "final state        : " << state_name(fsm.state()) << "\n";
    std::cout << "------------------------\n";
    std::cout << "state entry counts:\n";
    static const HealthState all[] = {
        HealthState::BOOTING,  HealthState::HEALTHY,
        HealthState::DEGRADED, HealthState::RECOVERING,
        HealthState::FAILED,   HealthState::SAFE_MODE};
    for (HealthState s : all) {
        std::cout << "  " << state_name(s) << " : "
                  << state_entry_counts[static_cast<int>(s)] << "\n";
    }
    std::cout << "========================\n";
    return 0;
}

} // namespace safety
