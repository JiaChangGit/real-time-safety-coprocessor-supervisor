"""test_fault_timeline - 由 events.jsonl 建立 fault timeline 的測試。

涵蓋：
  - 將每個 timeout / FAULT_EVENT 與其後第一個 recovery_result 配對，
    計算 fault-to-recovery duration_ms。
  - 斷言依 ts 排序、正確配對、正確時長（以 crafted sample）。
  - 驗證輸出 markdown 結構（含欄位 header 與每段 fault episode 一列）。
  - 若 reports/fault_timeline.md 存在，sanity-check 非空且有預期 header
    （不存在則 gracefully skip）。
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import safety_events as se  # noqa: E402

PROJECT_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir))


def crafted_two_episodes():
    """兩段 fault episode：故意亂序輸入以驗證依 ts 排序。"""
    return [
        # episode 1: timeout@1000 -> recovery@1500 (duration 500)
        {"ts_ms": 1000, "kind": "timeout", "frame_type": "HEARTBEAT", "seq": -1,
         "detail": "gap"},
        {"ts_ms": 1500, "kind": "recovery_result", "seq": -1,
         "detail": "RECOVERY_SUCCESS"},
        # episode 2: fault_event@3000 -> recovery@3200 (duration 200)
        # 故意放在亂序位置以測試排序。
        {"ts_ms": 3200, "kind": "recovery_result", "seq": -1,
         "detail": "RECOVERY_SUCCESS"},
        {"ts_ms": 3000, "kind": "rx_frame", "frame_type": "FAULT_EVENT",
         "seq": 5, "fault": "SENSOR_STUCK", "detail": "severity=2 critical"},
        # 中間的 noise（不影響配對）。
        {"ts_ms": 1200, "kind": "rx_frame", "frame_type": "TELEMETRY", "seq": 2},
        {"ts_ms": 2000, "kind": "tx_ack", "frame_type": "ACK", "seq": 3},
    ]


class TestTimelineBuild(unittest.TestCase):
    def test_pairing_and_durations(self):
        episodes = se.build_fault_timeline(crafted_two_episodes())
        self.assertEqual(len(episodes), 2)

        self.assertEqual(episodes[0]["trigger"], "TIMEOUT")
        self.assertEqual(episodes[0]["start_ts"], 1000)
        self.assertEqual(episodes[0]["end_ts"], 1500)
        self.assertEqual(episodes[0]["duration_ms"], 500)

        self.assertEqual(episodes[1]["trigger"], "FAULT_EVENT")
        self.assertEqual(episodes[1]["start_ts"], 3000)
        self.assertEqual(episodes[1]["end_ts"], 3200)
        self.assertEqual(episodes[1]["duration_ms"], 200)

    def test_ordering_by_ts(self):
        episodes = se.build_fault_timeline(crafted_two_episodes())
        starts = [ep["start_ts"] for ep in episodes]
        self.assertEqual(starts, sorted(starts))

    def test_unresolved_episode(self):
        events = [
            {"ts_ms": 1000, "kind": "timeout", "frame_type": "HEARTBEAT",
             "seq": -1, "detail": "gap"},
        ]
        episodes = se.build_fault_timeline(events)
        self.assertEqual(len(episodes), 1)
        self.assertIsNone(episodes[0]["end_ts"])
        self.assertIsNone(episodes[0]["duration_ms"])
        self.assertEqual(episodes[0]["outcome"], "UNRESOLVED")

    def test_no_double_pairing(self):
        # 一個 timeout 後接連兩個 recovery_result：只配對第一個。
        events = [
            {"ts_ms": 100, "kind": "timeout", "frame_type": "HEARTBEAT",
             "seq": -1, "detail": "gap"},
            {"ts_ms": 200, "kind": "recovery_result", "seq": -1,
             "detail": "RECOVERY_SUCCESS"},
            {"ts_ms": 300, "kind": "recovery_result", "seq": -1,
             "detail": "RECOVERY_SUCCESS"},
        ]
        episodes = se.build_fault_timeline(events)
        self.assertEqual(len(episodes), 1)
        self.assertEqual(episodes[0]["end_ts"], 200)
        self.assertEqual(episodes[0]["duration_ms"], 100)


class TestTimelineMarkdown(unittest.TestCase):
    EXPECTED_HEADER = ("| # | trigger | start_ts_ms | end_ts_ms | "
                       "duration_ms | outcome |")

    def test_markdown_structure(self):
        episodes = se.build_fault_timeline(crafted_two_episodes())
        md = se.timeline_to_markdown(episodes)
        lines = md.splitlines()
        # 含標題、表頭、分隔列，且每段 fault episode 一列。
        self.assertIn(self.EXPECTED_HEADER, lines)
        sep_idx = lines.index(self.EXPECTED_HEADER) + 1
        self.assertTrue(set(lines[sep_idx]) <= set("|-"))
        # 表頭含全部欄位名。
        for col in ("trigger", "start_ts_ms", "end_ts_ms", "duration_ms",
                    "outcome"):
            self.assertIn(col, self.EXPECTED_HEADER)
        # 資料列數 == episode 數（在 separator 之後）。
        data_rows = [l for l in lines[sep_idx + 1:]
                     if l.startswith("|")]
        self.assertEqual(len(data_rows), len(episodes))
        # 每列含正確 duration。
        self.assertIn("| 500 |", data_rows[0])
        self.assertIn("| 200 |", data_rows[1])


class TestRealTimelineReport(unittest.TestCase):
    def test_reports_fault_timeline_md_if_present(self):
        path = os.path.join(PROJECT_ROOT, "reports", "fault_timeline.md")
        if not os.path.exists(path):
            self.skipTest("reports/fault_timeline.md not present")
        with open(path, "r", encoding="utf-8") as f:
            content = f.read()
        self.assertTrue(content.strip(), "fault_timeline.md is empty")
        # 預期至少含表頭的 trigger / duration 欄位字樣或 markdown 表格分隔。
        self.assertTrue(
            "|" in content and ("duration" in content.lower()
                                or "trigger" in content.lower()),
            "fault_timeline.md missing expected table header")


if __name__ == "__main__":
    unittest.main()
