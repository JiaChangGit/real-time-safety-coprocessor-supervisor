// fault_injector.h - pty_bridge 的可設定 fault 注入器
//
// 在轉發 frame 時依設定執行：丟棄 (drop)、延遲 (delay)、損壞 checksum (corrupt)，
// 並可記錄每筆被轉送 frame 的紀錄 (ts,dir,type,seq,len,action)。
//
// 輸出一律英文（與專案慣例一致）。

#ifndef BRIDGE_FAULT_INJECTOR_H
#define BRIDGE_FAULT_INJECTOR_H

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace bridge {

// 注入器設定（由 CLI 填）。
struct InjectorConfig {
    int drop_every = 0;       // 每第 N 個 frame 丟棄一次（0 = 不丟）
    int corrupt_every = 0;    // 每第 N 個 frame 損壞 checksum（0 = 不損壞）
    int delay_ms = 0;         // 每個轉發 frame 延遲毫秒（0 = 不延遲）
    int drop_type = -1;       // 僅丟棄此 frame_type（-1 = 不限定型別）
    std::string log_path;     // 紀錄檔（空 = 不記錄）
};

// 對單一 frame 的處置決定。
enum class Action {
    FORWARD,  // 照常轉發
    DROP,     // 丟棄（不轉發）
    CORRUPT,  // 損壞 checksum 後轉發
    DELAY,    // 延遲後轉發（可與其他併存，但此列舉以主要動作為準）
};

class FaultInjector {
public:
    explicit FaultInjector(InjectorConfig cfg);
    ~FaultInjector();

    FaultInjector(const FaultInjector &) = delete;
    FaultInjector &operator=(const FaultInjector &) = delete;

    // 對一個完整 frame（buf 長度 len）決定處置並就地套用（必要時改 checksum）。
    // dir 為方向標籤（"L->R" / "R->L"）。回傳是否應轉發（DROP 時 false）。
    // 若需延遲，delay_ms 會回填給呼叫端去 sleep。
    bool process(std::vector<uint8_t> &frame, const std::string &dir,
                 int &out_delay_ms);

private:
    void log_line(int64_t ts_ms, const std::string &dir, uint8_t type,
                  uint16_t seq, size_t len, const char *action);

    InjectorConfig cfg_;
    std::ofstream log_;
    long frame_counter_ = 0; // 全域 frame 計數（決定第 N 個）
};

} // namespace bridge

#endif // BRIDGE_FAULT_INJECTOR_H
