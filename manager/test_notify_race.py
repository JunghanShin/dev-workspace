"""heartbeat가 spawn보다 먼저 올 때(design.md 9.5). 표준 라이브러리만, 프로세스 없이 순서만 재현.

    cd manager && python3 -m unittest test_notify_race -v

Manager는 워커를 await create_subprocess_exec로 띄우는데, 그 사이 이벤트 루프가 알림 소켓을 먼저 읽을 수 있다.
빨리 뜨는 워커(READY까지 수 ms)의 READY가 w.proc을 채우기 전에 오면 짝이 없어 버려졌고, 10 s 뒤 "no READY"로
다시 시작됐다. 지금은 짝 없는 heartbeat를 잠시 보관했다가 spawn 직후 처리한다.
"""
import time
import unittest

from workspace_manager.supervisor import Supervisor, Worker


class FakeProc:
    def __init__(self, pid):
        self.pid = pid


def make_sup():
    sup = Supervisor.__new__(Supervisor)  # Manager 전체 없이 heartbeat 처리만
    sup.workers = {}
    sup._early = {}
    sup._emit_worker = lambda w: None
    return sup


def make_worker(wid):
    return Worker({"id": wid, "type": "t", "autostart": False, "preset": None, "saved": {}, "config": {},
                   "temp": False, "desired_state": "running", "config_version": 1})


class EarlyHeartbeat(unittest.TestCase):
    def test_ready_before_spawn_finished(self):
        sup = make_sup()
        w = make_worker("fast")
        sup.workers["fast"] = w
        # 1) spawn을 기다리는 동안 READY가 먼저 옴(w.proc 없음)
        sup.on_notify(4242, {"READY": "1", "id": "fast", "libworker": "0.9.1"})
        self.assertIn(4242, sup._early)
        # 2) spawn이 끝남(supervisor가 w.proc·starting을 채운 뒤 _drain_early)
        w.proc = FakeProc(4242)
        w.actual = "starting"
        w.stop_reason = None
        sup._drain_early(w)
        self.assertEqual(w.actual, "running")
        self.assertEqual(w.libworker, "0.9.1")
        self.assertNotIn(4242, sup._early)

    def test_normal_order_unchanged(self):
        sup = make_sup()
        w = make_worker("slow")
        sup.workers["slow"] = w
        w.proc = FakeProc(7)
        w.actual = "starting"
        w.stop_reason = None
        sup._drain_early(w)  # 보관된 것 없음
        sup.on_notify(7, {"READY": "1", "libworker": "0.9.1"})
        self.assertEqual(w.actual, "running")
        self.assertEqual(sup._early, {})

    def test_old_and_too_many_are_dropped(self):
        sup = make_sup()
        sup.on_notify(1, {"READY": "1"})
        sup._early[1] = [(time.monotonic() - Supervisor.EARLY_KEEP_S - 1, {"READY": "1"})]  # 오래됨
        sup.on_notify(2, {"READY": "1"})
        self.assertNotIn(1, sup._early)
        for pid in range(100, 100 + Supervisor.EARLY_MAX + 10):
            sup.on_notify(pid, {"WATCHDOG": "1"})
        self.assertLessEqual(len(sup._early), Supervisor.EARLY_MAX)


if __name__ == "__main__":
    unittest.main()
