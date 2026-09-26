"""Compile suspending local methods through their source timer wrapper."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    dependency = 'import time\nexport class Worker { static wait() -> bool { time.sleep(1); return true } run() -> bool { time.sleep(1); return true } }'
    raise SystemExit(compile_cases({
        "local_method": 'import time\nclass Worker { run() -> bool { time.sleep(1); return true } }\nexport fn probe() -> bool { var worker = Worker(); return worker.run() }',
        "imported_method": 'import { Worker } from "./worker"\nexport fn probe() -> bool { var worker = Worker(); return worker.run() }',
        "imported_static_method": 'import { Worker } from "./worker"\nexport fn probe() -> bool { return Worker.wait() }',
        "semaphore_acquire": 'import { Semaphore } from sync\nexport fn probe() -> bool { var semaphore = Semaphore(1); return semaphore.acquire() }',
        "unused_default_parameter": 'import time\nexport fn probe(hint: i64 = -1) -> i64 { time.sleep(1); return 42 }',
        "event_count_wait": 'import { EventCount } from sync\nexport fn probe() -> i64 { var event = EventCount(1); return event.wait(0) }',
        "default_method_branch": 'import { CountdownLatch } from sync\nexport fn probe(provided: i64, receiver: CountdownLatch, count: i64?) -> i64 { if (provided == 0) { return receiver.done() }; return receiver.done(count!) }',
        "unit_coroutine": 'import time\nexport fn probe() { time.sleep(1) }',
        "cached_receiver": 'import { ResultGroup } from sync\nexport fn probe(group: ResultGroup) -> i64 { return group.batchSize }',
        "direct_wrapper": 'import time\nexport fn probe() -> i64 { time.sleep(1); return 42 }',
    }, {"worker.xr": dependency}))
