"""test_replay_logs - events.jsonl schema 與 replay FSM 重建測試。

涵蓋：
  - schema 檢查（keys 與 LOCKED kind 集合）。
  - 在 temp dir 產生 sample events.jsonl，逐行驗證可解析且 conform。
  - 將 rx_frame / timeout / tx_command 餵入 python FSM，斷言最終狀態與
    產生的 state_change 序列符合預期 fault->recovery chain。
  - 若 reports/events.jsonl 存在，額外驗證其 conform（不存在則 gracefully skip）。
"""

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import safety_events as se  # noqa: E402
import safety_fsm as fsm  # noqa: E402

PROJECT_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir))


def make_sample_events():
    """產生一段涵蓋完整 fault->recovery chain 的 sample 事件流。"""
    return [
        {"ts_ms": 1000, "kind": "rx_frame", "frame_type": "HEARTBEAT", "seq": 0},
        {"ts_ms": 1000, "kind": "tx_ack", "frame_type": "ACK", "seq": 0},
        {"ts_ms": 1100, "kind": "rx_frame", "frame_type": "HEARTBEAT", "seq": 1},
        {"ts_ms": 1100, "kind": "rx_frame", "frame_type": "TELEMETRY", "seq": 2},
        {"ts_ms": 2800, "kind": "timeout", "frame_type": "HEARTBEAT", "seq": -1,
         "detail": "gap=900ms > 350ms"},
        {"ts_ms": 2800, "kind": "tx_command", "frame_type": "COMMAND", "seq": 0,
         "detail": "REQUEST_RECOVERY"},
        {"ts_ms": 3000, "kind": "rx_frame", "frame_type": "HEARTBEAT", "seq": 3},
        {"ts_ms": 3000, "kind": "recovery_result", "seq": -1,
         "detail": "RECOVERY_SUCCESS"},
    ]


class TestSchema(unittest.TestCase):
    def test_locked_kind_set(self):
        self.assertEqual(se.KINDS, frozenset({
            "rx_frame", "tx_ack", "tx_nack", "tx_command", "timeout",
            "state_change", "duplicate", "checksum_error", "recovery_result"}))

    def test_locked_key_order(self):
        self.assertEqual(se.SCHEMA_KEYS, (
            "ts_ms", "kind", "frame_type", "seq", "fault",
            "from_state", "to_state", "detail"))

    def test_serialize_event_key_order_and_round_trip(self):
        line = se.serialize_event(
            {"ts_ms": 0, "kind": "state_change", "from_state": "BOOTING",
             "to_state": "HEALTHY", "detail": "HEARTBEAT_OK"})
        # 鍵順序必須與 LOCKED schema 完全一致。
        expected = ('{"ts_ms":0,"kind":"state_change","frame_type":"","seq":-1,'
                    '"fault":"","from_state":"BOOTING","to_state":"HEALTHY",'
                    '"detail":"HEARTBEAT_OK"}')
        self.assertEqual(line, expected)
        obj = json.loads(line)
        self.assertEqual(se.validate_event(obj), [])

    def test_validate_rejects_bad(self):
        self.assertTrue(se.validate_event({"kind": "rx_frame"}))  # 缺鍵
        bad = {k: "" for k in se.SCHEMA_KEYS}
        bad["ts_ms"] = 0
        bad["seq"] = -1
        bad["kind"] = "not_a_kind"
        self.assertIn("illegal kind: 'not_a_kind'", se.validate_event(bad))
        # 多餘鍵。
        extra = {k: ("" if isinstance(v, str) else v)
                 for k, v in {"ts_ms": 0, "kind": "rx_frame", "frame_type": "",
                              "seq": -1, "fault": "", "from_state": "",
                              "to_state": "", "detail": "", "junk": 1}.items()}
        self.assertTrue(any("unexpected keys" in e
                            for e in se.validate_event(extra)))


class TestSampleEventsFile(unittest.TestCase):
    def test_generate_and_validate_each_line(self):
        events = make_sample_events()
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "events.jsonl")
            with open(path, "w", encoding="utf-8") as f:
                for ev in events:
                    f.write(se.serialize_event(ev) + "\n")
            # 逐行讀回、解析、驗證。
            with open(path, "r", encoding="utf-8") as f:
                text = f.read()
            parsed = se.parse_lines(text)
            self.assertEqual(len(parsed), len(events))
            for i, obj in enumerate(parsed):
                with self.subTest(line=i):
                    self.assertEqual(se.validate_event(obj), [])


class TestReplayFsm(unittest.TestCase):
    def test_replay_reconstructs_fault_recovery_chain(self):
        events = make_sample_events()
        final_state, changes = se.replay(events)
        self.assertEqual(final_state, fsm.HEALTHY)
        self.assertEqual(changes, [
            (fsm.BOOTING, fsm.HEALTHY, fsm.HEARTBEAT_OK),
            (fsm.HEALTHY, fsm.DEGRADED, fsm.HEARTBEAT_TIMEOUT),
            (fsm.DEGRADED, fsm.RECOVERING, fsm.RECOVERY_SENT),
            (fsm.RECOVERING, fsm.HEALTHY, fsm.HEARTBEAT_RESTORED),
        ])

    def test_replay_critical_fault_goes_safe_mode(self):
        events = [
            {"ts_ms": 100, "kind": "rx_frame", "frame_type": "HEARTBEAT", "seq": 0},
            {"ts_ms": 200, "kind": "rx_frame", "frame_type": "FAULT_EVENT",
             "seq": 1, "fault": "TASK_HANG", "detail": "severity=2 critical"},
        ]
        final_state, changes = se.replay(events)
        self.assertEqual(final_state, fsm.SAFE_MODE)
        self.assertEqual(changes[-1],
                         (fsm.HEALTHY, fsm.SAFE_MODE, fsm.CRITICAL_FAULT))

    def test_replay_failure_to_failed(self):
        # timeout 後送 RECOVERY_SENT，再以 recovery_result 失敗（不重建 HEALTHY），
        # 改以另一個 timeout 之後... 這裡測 DEGRADED -> FAILED 路徑需 RECOVERY_FAILED。
        # replay 規則不產生 RECOVERY_FAILED，故此處改驗證 FSM 直接 fed 的失敗鏈。
        m = fsm.HealthStateMachine()
        for ev in (fsm.HEARTBEAT_OK, fsm.HEARTBEAT_TIMEOUT, fsm.RECOVERY_FAILED):
            m.apply(ev)
        self.assertEqual(m.state, fsm.FAILED)


class TestRealReportsEvents(unittest.TestCase):
    def test_reports_events_jsonl_conforms_if_present(self):
        path = os.path.join(PROJECT_ROOT, "reports", "events.jsonl")
        if not os.path.exists(path):
            self.skipTest("reports/events.jsonl not present")
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        parsed = se.parse_lines(text)
        self.assertGreater(len(parsed), 0)
        for i, obj in enumerate(parsed):
            with self.subTest(line=i):
                self.assertEqual(se.validate_event(obj), [],
                                 "line {} failed schema".format(i))


if __name__ == "__main__":
    unittest.main()
