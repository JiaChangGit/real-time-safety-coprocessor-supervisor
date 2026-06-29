// health_state_machine.cpp - Health FSM 轉移邏輯實作

#include "health_state_machine.h"

namespace safety {

const char *state_name(HealthState s)
{
    switch (s) {
    case HealthState::BOOTING:    return "BOOTING";
    case HealthState::HEALTHY:    return "HEALTHY";
    case HealthState::DEGRADED:   return "DEGRADED";
    case HealthState::RECOVERING: return "RECOVERING";
    case HealthState::FAILED:     return "FAILED";
    case HealthState::SAFE_MODE:  return "SAFE_MODE";
    }
    return "UNKNOWN";
}

const char *event_name(HealthEvent e)
{
    switch (e) {
    case HealthEvent::HEARTBEAT_OK:       return "HEARTBEAT_OK";
    case HealthEvent::HEARTBEAT_TIMEOUT:  return "HEARTBEAT_TIMEOUT";
    case HealthEvent::RECOVERY_SENT:      return "RECOVERY_SENT";
    case HealthEvent::HEARTBEAT_RESTORED: return "HEARTBEAT_RESTORED";
    case HealthEvent::RECOVERY_FAILED:    return "RECOVERY_FAILED";
    case HealthEvent::CRITICAL_FAULT:     return "CRITICAL_FAULT";
    }
    return "UNKNOWN";
}

TransitionResult HealthStateMachine::apply(HealthEvent event)
{
    TransitionResult r{false, state_, state_, event};

    // 嚴格依照規格的轉移表；未列出的 (state,event) 組合一律視為不合法。
    switch (state_) {
    case HealthState::BOOTING:
        if (event == HealthEvent::HEARTBEAT_OK)
            r.to = HealthState::HEALTHY;
        break;

    case HealthState::HEALTHY:
        if (event == HealthEvent::HEARTBEAT_TIMEOUT)
            r.to = HealthState::DEGRADED;
        else if (event == HealthEvent::CRITICAL_FAULT)
            r.to = HealthState::SAFE_MODE;
        break;

    case HealthState::DEGRADED:
        if (event == HealthEvent::RECOVERY_SENT)
            r.to = HealthState::RECOVERING;
        else if (event == HealthEvent::RECOVERY_FAILED)
            r.to = HealthState::FAILED;
        else if (event == HealthEvent::CRITICAL_FAULT)
            r.to = HealthState::SAFE_MODE;
        break;

    case HealthState::RECOVERING:
        if (event == HealthEvent::HEARTBEAT_RESTORED)
            r.to = HealthState::HEALTHY;
        else if (event == HealthEvent::RECOVERY_FAILED)
            r.to = HealthState::FAILED;
        break;

    case HealthState::FAILED:
    case HealthState::SAFE_MODE:
        // 終端狀態，無對外轉移（需由更高層人工/重啟介入）。
        break;
    }

    // 只有真正改變狀態（或自我轉移但已定義）才算合法；本表中合法轉移皆為換狀態。
    r.valid = (r.to != r.from);
    if (r.valid) {
        state_ = r.to;
        if (hook_)
            hook_(r);
    }
    return r;
}

} // namespace safety
