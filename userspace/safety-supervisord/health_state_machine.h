// health_state_machine.h - Userspace 健康狀態機 (Health FSM)
//
// 純邏輯、無任何 I/O，方便單元測試與 replay 模式重用。
// 狀態與轉移嚴格依照專案規格實作（見 .cpp 中的轉移表）。
//
// 注意：此 FSM 與 kernel 端的 safety_link_state 不同；後者僅描述連線層心跳狀態，
// 本 FSM 描述 supervisor 對 co-processor 健康的高階判斷。

#ifndef SAFETY_HEALTH_STATE_MACHINE_H
#define SAFETY_HEALTH_STATE_MACHINE_H

#include <functional>

namespace safety {

// ---- FSM 狀態 ----
enum class HealthState {
    BOOTING,
    HEALTHY,
    DEGRADED,
    RECOVERING,
    FAILED,
    SAFE_MODE,
};

// ---- FSM 輸入事件 ----
enum class HealthEvent {
    HEARTBEAT_OK,
    HEARTBEAT_TIMEOUT,
    RECOVERY_SENT,
    HEARTBEAT_RESTORED,
    RECOVERY_FAILED,
    CRITICAL_FAULT,
};

// 轉移結果：valid 表示該事件在當前狀態下是否為合法轉移。
struct TransitionResult {
    bool valid;            // 是否為已定義的合法轉移
    HealthState from;      // 轉移前狀態
    HealthState to;        // 轉移後狀態（不合法時等於 from）
    HealthEvent event;     // 觸發事件
};

const char *state_name(HealthState s);
const char *event_name(HealthEvent e);

class HealthStateMachine {
public:
    // 轉移回呼：每次「合法」轉移後呼叫，供呼叫端記錄 (from,to,event)。
    using TransitionHook = std::function<void(const TransitionResult &)>;

    HealthStateMachine() = default;

    HealthState state() const { return state_; }

    // 設定轉移 hook（合法轉移時觸發）。
    void set_hook(TransitionHook hook) { hook_ = std::move(hook); }

    // 套用一個事件；回傳轉移結果。合法轉移才會更新內部狀態並觸發 hook。
    TransitionResult apply(HealthEvent event);

private:
    HealthState state_ = HealthState::BOOTING;
    TransitionHook hook_;
};

} // namespace safety

#endif // SAFETY_HEALTH_STATE_MACHINE_H
