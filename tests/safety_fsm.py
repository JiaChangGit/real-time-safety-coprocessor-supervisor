"""safety_fsm - 純 Python 鏡像實作 Health FSM（健康狀態機）。

對應 C++ ``userspace/safety-supervisord/health_state_machine.{h,cpp}`` 的契約：

狀態 (states)：BOOTING, HEALTHY, DEGRADED, RECOVERING, FAILED, SAFE_MODE
事件 (events)：HEARTBEAT_OK, HEARTBEAT_TIMEOUT, RECOVERY_SENT,
              HEARTBEAT_RESTORED, RECOVERY_FAILED, CRITICAL_FAULT

轉移表 (transition table)，未列出之 (state,event) 一律視為不合法 (invalid)：
  BOOTING    + HEARTBEAT_OK       -> HEALTHY
  HEALTHY    + HEARTBEAT_TIMEOUT  -> DEGRADED
  HEALTHY    + CRITICAL_FAULT     -> SAFE_MODE
  DEGRADED   + RECOVERY_SENT      -> RECOVERING
  DEGRADED   + RECOVERY_FAILED    -> FAILED
  DEGRADED   + CRITICAL_FAULT     -> SAFE_MODE
  RECOVERING + HEARTBEAT_RESTORED -> HEALTHY
  RECOVERING + RECOVERY_FAILED    -> FAILED

FAILED 與 SAFE_MODE 為終端狀態 (terminal)，無對外轉移。

合法性規則 (與 C++ 一致)：僅當 ``to != from`` 時 transition 才算 valid。
"""

# ---- 狀態 (states) ----
BOOTING = "BOOTING"
HEALTHY = "HEALTHY"
DEGRADED = "DEGRADED"
RECOVERING = "RECOVERING"
FAILED = "FAILED"
SAFE_MODE = "SAFE_MODE"

STATES = (BOOTING, HEALTHY, DEGRADED, RECOVERING, FAILED, SAFE_MODE)

# ---- 事件 (events) ----
HEARTBEAT_OK = "HEARTBEAT_OK"
HEARTBEAT_TIMEOUT = "HEARTBEAT_TIMEOUT"
RECOVERY_SENT = "RECOVERY_SENT"
HEARTBEAT_RESTORED = "HEARTBEAT_RESTORED"
RECOVERY_FAILED = "RECOVERY_FAILED"
CRITICAL_FAULT = "CRITICAL_FAULT"

EVENTS = (
    HEARTBEAT_OK,
    HEARTBEAT_TIMEOUT,
    RECOVERY_SENT,
    HEARTBEAT_RESTORED,
    RECOVERY_FAILED,
    CRITICAL_FAULT,
)

# 轉移表：{from_state: {event: to_state}}
_TRANSITIONS = {
    BOOTING: {HEARTBEAT_OK: HEALTHY},
    HEALTHY: {HEARTBEAT_TIMEOUT: DEGRADED, CRITICAL_FAULT: SAFE_MODE},
    DEGRADED: {
        RECOVERY_SENT: RECOVERING,
        RECOVERY_FAILED: FAILED,
        CRITICAL_FAULT: SAFE_MODE,
    },
    RECOVERING: {HEARTBEAT_RESTORED: HEALTHY, RECOVERY_FAILED: FAILED},
    FAILED: {},
    SAFE_MODE: {},
}


class TransitionResult:
    """單次 apply 的結果，對應 C++ ``TransitionResult``。"""

    __slots__ = ("valid", "from_state", "to_state", "event")

    def __init__(self, valid, from_state, to_state, event):
        self.valid = valid
        self.from_state = from_state
        self.to_state = to_state
        self.event = event

    def __repr__(self):
        return "TransitionResult(valid={}, {}-[{}]->{})".format(
            self.valid, self.from_state, self.event, self.to_state)


class HealthStateMachine:
    """Health FSM，與 C++ 行為一致：僅合法轉移會更新狀態並觸發 hook。"""

    def __init__(self, hook=None):
        self.state = BOOTING
        self.hook = hook

    def set_hook(self, hook):
        self.hook = hook

    def apply(self, event):
        """套用一個事件，回傳 :class:`TransitionResult`。

        合法轉移 (to != from) 才更新內部狀態並觸發 hook；否則狀態不變、valid=False。
        """
        if event not in EVENTS:
            raise ValueError("unknown event: {!r}".format(event))
        from_state = self.state
        to_state = _TRANSITIONS.get(from_state, {}).get(event, from_state)
        valid = to_state != from_state
        result = TransitionResult(valid, from_state, to_state, event)
        if valid:
            self.state = to_state
            if self.hook is not None:
                self.hook(result)
        return result
