"""scheduler tests: per-tag limits are respected, a raising task is recorded
rather than cancelling siblings, and results map back by key.
"""

import threading
import time
import unittest

from _support import bootstrap_xraytest

bootstrap_xraytest()
from xraytest import scheduler  # noqa: E402


class DefaultLimitsTest(unittest.TestCase):
    def test_cpu_saturates_link_throttles(self):
        limits = scheduler.default_limits(16)
        self.assertEqual(limits[scheduler.CPU], 16)
        self.assertEqual(limits[scheduler.LINK], 4)
        self.assertGreaterEqual(limits[scheduler.LINK], 2)

    def test_never_below_floor(self):
        limits = scheduler.default_limits(1)
        self.assertEqual(limits[scheduler.CPU], 1)
        self.assertGreaterEqual(limits[scheduler.LINK], 2)


class RunTest(unittest.TestCase):
    def test_results_map_by_key(self):
        sched = scheduler.Scheduler(scheduler.default_limits(4))
        tasks = [scheduler.Task(key=str(i), fn=(lambda n=i: n * 10)) for i in range(5)]
        results = sched.run(tasks)
        self.assertEqual(results, {"0": 0, "1": 10, "2": 20, "3": 30, "4": 40})

    def test_raising_task_is_recorded_not_propagated(self):
        sched = scheduler.Scheduler(scheduler.default_limits(4))

        def boom():
            raise ValueError("nope")

        tasks = [
            scheduler.Task(key="ok", fn=lambda: 1),
            scheduler.Task(key="bad", fn=boom),
        ]
        results = sched.run(tasks)
        self.assertEqual(results["ok"], 1)
        self.assertIsInstance(results["bad"], ValueError)

    def test_tag_limit_caps_concurrency(self):
        # Two link slots for 6 tasks: peak concurrency must never exceed 2.
        limits = {scheduler.LINK: 2, scheduler.CPU: 8}
        sched = scheduler.Scheduler(limits)
        active = {"now": 0, "peak": 0}
        lock = threading.Lock()

        def work():
            with lock:
                active["now"] += 1
                active["peak"] = max(active["peak"], active["now"])
            time.sleep(0.05)
            with lock:
                active["now"] -= 1

        tasks = [scheduler.Task(key=str(i), fn=work, tag=scheduler.LINK) for i in range(6)]
        sched.run(tasks)
        self.assertLessEqual(active["peak"], 2)

    def test_empty_task_list(self):
        sched = scheduler.Scheduler(scheduler.default_limits(4))
        self.assertEqual(sched.run([]), {})

    def test_serial_matches_parallel_results(self):
        tasks = [scheduler.Task(key=str(i), fn=(lambda n=i: n)) for i in range(4)]
        self.assertEqual(scheduler.run_serial(tasks), {"0": 0, "1": 1, "2": 2, "3": 3})


if __name__ == "__main__":
    unittest.main()
