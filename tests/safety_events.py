"""safety_events - events.jsonl schema 與 replay / timeline 邏輯 (純 Python)。

對應 C++ ``logger_worker`` 的 LOCKED schema 與 ``replay_mode`` 的重播規則：

JSON 鍵順序 (LOCKED)：ts_ms, kind, frame_type, seq, fault, from_state, to_state, detail
kind 合法集合 (LOCKED)：
  rx_frame, tx_ack, tx_nack, tx_command, timeout, state_change,
  duplicate, checksum_error, recovery_result

replay 規則 (鏡像 replay_mode.cpp)：
  rx_frame + HEARTBEAT   -> RECOVERING 時為 HEARTBEAT_RESTORED，否則 HEARTBEAT_OK
  rx_frame + FAULT_EVENT -> detail 含 "critical"/"severity=2" 才觸發 CRITICAL_FAULT
  timeout                -> HEARTBEAT_TIMEOUT
  tx_command (REQUEST_RECOVERY) -> RECOVERY_SENT
  其餘 kind 一律忽略（不驅動 FSM）。
"""

import json

import safety_fsm as fsm

# events.jsonl 的 LOCKED key 集合與順序。
SCHEMA_KEYS = (
    "ts_ms",
    "kind",
    "frame_type",
    "seq",
    "fault",
    "from_state",
    "to_state",
    "detail",
)

# LOCKED kind 集合。
KINDS = frozenset({
    "rx_frame",
    "tx_ack",
    "tx_nack",
    "tx_command",
    "timeout",
    "state_change",
    "duplicate",
    "checksum_error",
    "recovery_result",
})


def serialize_event(ev):
    """以 LOCKED 鍵順序序列化單一事件 dict 為一行 JSON（鏡像 C++ serialize_event）。

    缺少的欄位以預設值補齊：字串欄為 ""、ts_ms 為 0、seq 為 -1。
    """
    out = {
        "ts_ms": ev.get("ts_ms", 0),
        "kind": ev["kind"],
        "frame_type": ev.get("frame_type", ""),
        "seq": ev.get("seq", -1),
        "fault": ev.get("fault", ""),
        "from_state": ev.get("from_state", ""),
        "to_state": ev.get("to_state", ""),
        "detail": ev.get("detail", ""),
    }
    # 維持鍵順序，並用 separators 緊湊輸出（與手寫 C++ 輸出語意一致）。
    parts = []
    for k in SCHEMA_KEYS:
        parts.append(json.dumps(k) + ":" + json.dumps(out[k]))
    return "{" + ",".join(parts) + "}"


def validate_event(obj):
    """驗證單一已解析的事件 dict 是否符合 LOCKED schema。

    回傳錯誤訊息字串清單；空清單表示通過。
    """
    errors = []
    if not isinstance(obj, dict):
        return ["event is not a JSON object"]
    keys = set(obj.keys())
    expected = set(SCHEMA_KEYS)
    missing = expected - keys
    extra = keys - expected
    if missing:
        errors.append("missing keys: {}".format(sorted(missing)))
    if extra:
        errors.append("unexpected keys: {}".format(sorted(extra)))
    if "kind" in obj and obj["kind"] not in KINDS:
        errors.append("illegal kind: {!r}".format(obj["kind"]))
    # 型別檢查。
    if "ts_ms" in obj and not isinstance(obj["ts_ms"], int):
        errors.append("ts_ms must be int")
    if "seq" in obj and not isinstance(obj["seq"], int):
        errors.append("seq must be int")
    for s in ("frame_type", "fault", "from_state", "to_state", "detail"):
        if s in obj and not isinstance(obj[s], str):
            errors.append("{} must be string".format(s))
    return errors


def parse_lines(text):
    """將 JSONL 文字解析為事件 dict 清單（略過空行）。"""
    events = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        events.append(json.loads(line))
    return events


def replay(events):
    """以鏡像 replay_mode.cpp 的規則餵入事件，回傳 (final_state, state_changes)。

    state_changes 為 list of (from_state, to_state, event_name)，順序與產生序一致。
    """
    machine = fsm.HealthStateMachine()
    changes = []
    machine.set_hook(lambda r: changes.append(
        (r.from_state, r.to_state, r.event)))

    for ev in events:
        kind = ev.get("kind")
        if kind == "rx_frame":
            ftype = ev.get("frame_type", "")
            if ftype == "HEARTBEAT":
                if machine.state == fsm.RECOVERING:
                    machine.apply(fsm.HEARTBEAT_RESTORED)
                else:
                    machine.apply(fsm.HEARTBEAT_OK)
            elif ftype == "FAULT_EVENT":
                detail = ev.get("detail", "")
                critical = "critical" in detail or "severity=2" in detail
                if critical:
                    machine.apply(fsm.CRITICAL_FAULT)
        elif kind == "timeout":
            machine.apply(fsm.HEARTBEAT_TIMEOUT)
        elif kind == "tx_command":
            if "REQUEST_RECOVERY" in ev.get("detail", ""):
                machine.apply(fsm.RECOVERY_SENT)
        # 其餘 kind 忽略。
    return machine.state, changes


# =====================================================================
#  Fault timeline：把事件流轉成「fault 事件 -> recovery」配對與時長。
# =====================================================================

def build_fault_timeline(events):
    """從事件流建立 fault timeline。

    規則：每個 ``timeout``（或 critical ``FAULT_EVENT``）開啟一段 fault episode，
    與其後第一個 ``recovery_result``（或 RECOVERING 狀態下的 HEARTBEAT_RESTORED）
    配對結束，計算 duration_ms = end_ts - start_ts。

    回傳 list of dict：{start_ts, end_ts, duration_ms, trigger, outcome}。
    事件先依 ts_ms 穩定排序以確保 ordering。
    """
    ordered = sorted(events, key=lambda e: e.get("ts_ms", 0))
    episodes = []
    open_ep = None
    for ev in ordered:
        kind = ev.get("kind")
        ts = ev.get("ts_ms", 0)
        is_trigger = (kind == "timeout") or (
            kind == "rx_frame" and ev.get("frame_type") == "FAULT_EVENT"
            and ("critical" in ev.get("detail", "")
                 or "severity=2" in ev.get("detail", "")))
        if is_trigger and open_ep is None:
            trig = "TIMEOUT" if kind == "timeout" else "FAULT_EVENT"
            open_ep = {"start_ts": ts, "trigger": trig}
        elif open_ep is not None:
            is_recovery = (kind == "recovery_result")
            if is_recovery:
                open_ep["end_ts"] = ts
                open_ep["duration_ms"] = ts - open_ep["start_ts"]
                open_ep["outcome"] = ev.get("detail", "") or "RECOVERY"
                episodes.append(open_ep)
                open_ep = None
    # 未配對的 episode：標記為未結束。
    if open_ep is not None:
        open_ep["end_ts"] = None
        open_ep["duration_ms"] = None
        open_ep["outcome"] = "UNRESOLVED"
        episodes.append(open_ep)
    return episodes


def timeline_to_markdown(episodes):
    """將 fault timeline 轉成 markdown 表格（每段 fault episode 一列）。"""
    header = "| # | trigger | start_ts_ms | end_ts_ms | duration_ms | outcome |"
    sep = "|---|---------|-------------|-----------|-------------|---------|"
    lines = ["# Fault Timeline", "", header, sep]
    for i, ep in enumerate(episodes, 1):
        end = "-" if ep["end_ts"] is None else str(ep["end_ts"])
        dur = "-" if ep["duration_ms"] is None else str(ep["duration_ms"])
        lines.append("| {} | {} | {} | {} | {} | {} |".format(
            i, ep["trigger"], ep["start_ts"], end, dur, ep["outcome"]))
    return "\n".join(lines) + "\n"
