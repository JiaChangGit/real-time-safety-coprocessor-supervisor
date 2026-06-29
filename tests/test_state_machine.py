"""test_state_machine - Health FSM 轉移測試。

涵蓋：每個合法轉移、非法轉移被拒（狀態不變且 valid=False）、
完整 fault->recovery happy path、以及失敗路徑到 FAILED。
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import safety_fsm as fsm  # noqa: E402


class TestValidTransitions(unittest.TestCase):
    """逐一驗證轉移表中每個合法 (state, event) -> to_state。"""

    VALID = [
        (fsm.BOOTING, fsm.HEARTBEAT_OK, fsm.HEALTHY),
        (fsm.HEALTHY, fsm.HEARTBEAT_TIMEOUT, fsm.DEGRADED),
        (fsm.HEALTHY, fsm.CRITICAL_FAULT, fsm.SAFE_MODE),
        (fsm.DEGRADED, fsm.RECOVERY_SENT, fsm.RECOVERING),
        (fsm.DEGRADED, fsm.RECOVERY_FAILED, fsm.FAILED),
        (fsm.DEGRADED, fsm.CRITICAL_FAULT, fsm.SAFE_MODE),
        (fsm.RECOVERING, fsm.HEARTBEAT_RESTORED, fsm.HEALTHY),
        (fsm.RECOVERING, fsm.RECOVERY_FAILED, fsm.FAILED),
    ]

    def _drive_to(self, machine, target):
        """以已知合法路徑把 machine 推到 target 狀態。"""
        paths = {
            fsm.BOOTING: [],
            fsm.HEALTHY: [fsm.HEARTBEAT_OK],
            fsm.DEGRADED: [fsm.HEARTBEAT_OK, fsm.HEARTBEAT_TIMEOUT],
            fsm.RECOVERING: [fsm.HEARTBEAT_OK, fsm.HEARTBEAT_TIMEOUT,
                             fsm.RECOVERY_SENT],
            fsm.FAILED: [fsm.HEARTBEAT_OK, fsm.HEARTBEAT_TIMEOUT,
                         fsm.RECOVERY_FAILED],
            fsm.SAFE_MODE: [fsm.HEARTBEAT_OK, fsm.CRITICAL_FAULT],
        }
        for ev in paths[target]:
            machine.apply(ev)
        self.assertEqual(machine.state, target)

    def test_each_valid_transition(self):
        for from_state, event, to_state in self.VALID:
            with self.subTest(frm=from_state, event=event):
                m = fsm.HealthStateMachine()
                self._drive_to(m, from_state)
                r = m.apply(event)
                self.assertTrue(r.valid)
                self.assertEqual(r.from_state, from_state)
                self.assertEqual(r.to_state, to_state)
                self.assertEqual(m.state, to_state)


class TestInvalidTransitions(unittest.TestCase):
    """非法 (state, event) 組合：狀態不變、valid=False、hook 不觸發。"""

    def _all_valid_pairs(self):
        return {(f, e) for f, e, _ in TestValidTransitions.VALID}

    def test_all_invalid_pairs_rejected(self):
        valid_pairs = self._all_valid_pairs()
        driver = TestValidTransitions()
        for state in fsm.STATES:
            for event in fsm.EVENTS:
                if (state, event) in valid_pairs:
                    continue
                with self.subTest(state=state, event=event):
                    m = fsm.HealthStateMachine()
                    driver._drive_to(m, state)
                    fired = []
                    m.set_hook(lambda r: fired.append(r))
                    r = m.apply(event)
                    self.assertFalse(r.valid)
                    self.assertEqual(m.state, state)
                    self.assertEqual(r.from_state, state)
                    self.assertEqual(r.to_state, state)
                    self.assertEqual(fired, [])

    def test_terminal_states_have_no_exit(self):
        driver = TestValidTransitions()
        for terminal in (fsm.FAILED, fsm.SAFE_MODE):
            for event in fsm.EVENTS:
                with self.subTest(terminal=terminal, event=event):
                    m = fsm.HealthStateMachine()
                    driver._drive_to(m, terminal)
                    r = m.apply(event)
                    self.assertFalse(r.valid)
                    self.assertEqual(m.state, terminal)

    def test_unknown_event_raises(self):
        m = fsm.HealthStateMachine()
        with self.assertRaises(ValueError):
            m.apply("NOT_AN_EVENT")


class TestFullPaths(unittest.TestCase):
    """端到端路徑：fault->recovery happy path 與 failure path。"""

    def test_happy_path_fault_to_recovery(self):
        changes = []
        m = fsm.HealthStateMachine(hook=lambda r: changes.append(
            (r.from_state, r.to_state, r.event)))
        # BOOTING -> HEALTHY -> DEGRADED -> RECOVERING -> HEALTHY
        m.apply(fsm.HEARTBEAT_OK)
        m.apply(fsm.HEARTBEAT_TIMEOUT)
        m.apply(fsm.RECOVERY_SENT)
        m.apply(fsm.HEARTBEAT_RESTORED)
        self.assertEqual(m.state, fsm.HEALTHY)
        self.assertEqual(changes, [
            (fsm.BOOTING, fsm.HEALTHY, fsm.HEARTBEAT_OK),
            (fsm.HEALTHY, fsm.DEGRADED, fsm.HEARTBEAT_TIMEOUT),
            (fsm.DEGRADED, fsm.RECOVERING, fsm.RECOVERY_SENT),
            (fsm.RECOVERING, fsm.HEALTHY, fsm.HEARTBEAT_RESTORED),
        ])

    def test_failure_path_to_failed_from_degraded(self):
        m = fsm.HealthStateMachine()
        m.apply(fsm.HEARTBEAT_OK)
        m.apply(fsm.HEARTBEAT_TIMEOUT)
        m.apply(fsm.RECOVERY_FAILED)
        self.assertEqual(m.state, fsm.FAILED)

    def test_failure_path_to_failed_from_recovering(self):
        m = fsm.HealthStateMachine()
        m.apply(fsm.HEARTBEAT_OK)
        m.apply(fsm.HEARTBEAT_TIMEOUT)
        m.apply(fsm.RECOVERY_SENT)
        m.apply(fsm.RECOVERY_FAILED)
        self.assertEqual(m.state, fsm.FAILED)

    def test_critical_fault_to_safe_mode(self):
        m = fsm.HealthStateMachine()
        m.apply(fsm.HEARTBEAT_OK)
        m.apply(fsm.CRITICAL_FAULT)
        self.assertEqual(m.state, fsm.SAFE_MODE)


if __name__ == "__main__":
    unittest.main()
