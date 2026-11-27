"""Run independent fixtures with a bounded pool and owned child launches."""

from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from contextlib import contextmanager
import threading


class CaseCancelled(Exception):
    pass


class SpawnGate:
    def __init__(self):
        self._lock = threading.Lock()
        self._stopped = False

    def stopped(self):
        with self._lock:
            return self._stopped

    def stop(self):
        with self._lock:
            self._stopped = True

    @contextmanager
    def launch(self):
        with self._lock:
            if self._stopped:
                raise CaseCancelled()
            yield


def run_ordered(cases, execute, jobs, gate, recovery_required):
    pending = {}
    queued = iter(enumerate(cases))
    outcomes = {}
    fatal = []

    def guarded(index, case):
        try:
            if gate.stopped():
                raise CaseCancelled()
            return execute(index, case)
        except CaseCancelled:
            raise
        except BaseException:
            gate.stop()
            raise

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        def queue_one():
            if not gate.stopped() and (entry := next(queued, None)) is not None:
                index, case = entry
                pending[pool.submit(guarded, index, case)] = index

        try:
            for _ in range(jobs):
                queue_one()
            while pending:
                done, _ = wait(pending, return_when=FIRST_COMPLETED)
                for future in sorted(done, key=pending.__getitem__):
                    index = pending.pop(future)
                    try:
                        outcomes[index] = future.result()
                    except CaseCancelled:
                        pass
                    except BaseException as error:
                        fatal.append((index, error))
                    queue_one()
        except BaseException:
            gate.stop()
            for future in pending:
                future.cancel()
            raise

    if fatal:
        # Never lose unverified child drain behind an ordinary assertion.
        _, error = min(fatal, key=lambda failure: (
            not isinstance(failure[1], recovery_required), failure[0]))
        raise error
    return [outcomes[index] for index in range(len(cases))]
