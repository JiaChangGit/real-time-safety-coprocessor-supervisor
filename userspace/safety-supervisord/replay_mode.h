// replay_mode.h - REPLAY 模式（單執行緒、確定性、Valgrind 友善）
//
// 讀取一個 events.jsonl，對 kind=="rx_frame" / "timeout" 推導 FSM 事件，
// 記錄 state_change 行到 <report-dir>/replay_events.jsonl，並印出英文摘要。
// 全程單執行緒、無背景 thread、配置最小化，是 valgrind 主要驗證對象。

#ifndef SAFETY_REPLAY_MODE_H
#define SAFETY_REPLAY_MODE_H

#include <string>

namespace safety {

// 回傳 process exit code（成功為 0）。
int run_replay(const std::string &input_path, const std::string &report_dir);

} // namespace safety

#endif // SAFETY_REPLAY_MODE_H
