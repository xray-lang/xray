#!/usr/bin/env python3
"""Public output, terminal status and cleanup observations on both backends."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

PRELUDE = '''import time
enum Failure { Bad }
fn fail() throws(Failure) { throw Failure.Bad }
fn waiting(ready: Channel<i64>) -> i64 {
    defer { print("child-cleanup") }
    ready.send(1)
    time.sleep(10000)
    return 7
}
'''

CASES = {
    "never": ('''fn value() -> i64 { return 42 }
var task = go value()
print(await task)
print(Task.all([task])[0])
print(Task.first([task]))
''', "42\n42\n42\n", 0, ""),
    "await_chain": (PRELUDE + '''fn middle(task: Task<i64>) -> i64 {
    defer { print("middle-cleanup") }
    try { const v = await task; fail(); return v }
    catch (e) { print("caught-middle-error") }
    catch panic (p) { print("caught-middle-panic") }
    return -1
}
fn main() {
    defer { print("root-cleanup") }
    const ready = Channel<i64>(1)
    var task = go waiting(ready)
    ready.recv()
    var waiter = go middle(task)
    task.cancel()
    try { print(await waiter); fail() }
    catch (e) { print("caught-root-error") }
    catch panic (p) { print("caught-root-panic") }
    print("unreachable")
}
main()
''', "child-cleanup\nmiddle-cleanup\nroot-cleanup\n", 1, "[Uncaught Cancellation]\n"),
    "observe": (PRELUDE + '''const ready = Channel<i64>(1)
var task = go waiting(ready)
ready.recv()
task.cancel()
print(task.awaitResult().ordinal)
print(task.poll()!.ordinal)
print("alive")
''', "child-cleanup\n3\n3\nalive\n", 0, ""),
    "scope_failure": (PRELUDE + '''fn failing(gate: Channel<i64>) throws(Failure) {
    gate.recv()
    throw Failure.Bad
}
fn receive(task: Task<i64>) -> i64 {
    defer { print("helper-cleanup") }
    return await task
}
fn main() {
    const ready = Channel<i64>(1)
    const gate = Channel<i64>(1)
    try {
        scope {
            defer { print("body-cleanup") }
            var task = go waiting(ready)
            ready.recv()
            go failing(gate)
            gate.send(1)
            try { print(receive(task)); fail() }
            catch(e) { print("caught-inner-error") }
            catch panic(p) { print("caught-inner-panic") }
            print("unreachable")
        }
    } catch (e: Failure) { print("scope-error") }
    print("alive")
}
main()
''', "child-cleanup\nhelper-cleanup\nbody-cleanup\nscope-error\nalive\n", 0, ""),
    "scope_independent": (PRELUDE + '''fn main() {
    defer { print("root-cleanup") }
    const ready = Channel<i64>(1)
    try {
        scope {
            defer { print("body-cleanup") }
            var task = go waiting(ready)
            ready.recv()
            task.cancel()
            await task
            fail()
        }
    } catch (e) { print("caught-error") }
    catch panic (p) { print("caught-panic") }
    print("unreachable")
}
main()
''', "child-cleanup\nbody-cleanup\nroot-cleanup\n", 1, "[Uncaught Cancellation]\n"),
    "all_cancel": (PRELUDE + '''fn main() {
    defer { print("root-cleanup") }
    const ready = Channel<i64>(1)
    var task = go waiting(ready)
    ready.recv()
    task.cancel()
    Task.all([task])
    print("unreachable")
}
main()
''', "child-cleanup\nroot-cleanup\n", 1, "[Uncaught Cancellation]\n"),
    "first_cancel": (PRELUDE + '''fn main() {
    defer { print("root-cleanup") }
    const ready = Channel<i64>(1)
    var task = go waiting(ready)
    ready.recv()
    task.cancel()
    Task.first([task])
    print("unreachable")
}
main()
''', "child-cleanup\nroot-cleanup\n", 1, "[Uncaught Cancellation]\n"),
    "closed_send": ('''const ch = Channel<string>(1)
ch.close()
fn send(ch: Channel<string>) {
    defer { print("send-cleanup") }
    ch.send("owned payload")
}
send(ch)
''', "send-cleanup\n", 1, "[Uncaught Panic] E0702: cannot send on a closed channel\n"),
    "closed_results": ('''const ch = Channel<string>(1)
ch.close()
print(ch.trySend("owned payload").ordinal)
print(ch.sendTimeout("owned payload", 1).ordinal)
assertPanics(fn() { ch.send("owned payload") })
print("alive")
''', "3\n3\nalive\n", 0, ""),
    "select_send": ('''const ch = Channel<string>(1)
select { "payload" to ch -> { print("sent") } }
match(ch.recv()) { Recv.Value { value } -> print(value), _ -> assert(false) }
''', "sent\npayload\n", 0, ""),
    "select_closed": ('''fn main() {
    defer { print("select-cleanup") }
    const ch = Channel<string>()
    ch.close()
    select { "payload" to ch -> { print("unreachable") } }
}
main()
''', "select-cleanup\n", 1, "[Uncaught Panic] E0702: cannot send on a closed channel\n"),
    "markers": ('''enum Failure { Bad }
fn value() -> i64 { return 42 }
fn maybe() throws(Failure) -> i64 { return 42 }
fn generic<E: Error>(action: fn() throws(E) -> i64) throws(E) -> i64 {
    return try action()
}
fn main() throws(Failure) {
    print(try maybe())
    print((try? maybe()) ?? 0)
    print(try! maybe())
    try { print(maybe()) } catch(e) { print("unexpected") }
    print(generic<Never>(value))
}
try main()
''', "42\n42\n42\n42\n42\n", 0, ""),
    "timeout_success": ('''fn value() -> i64 { return 42 }
var task = go value()
print(try Task.timeout(task, 100))
''', "42\n", 0, ""),
    "timeout_unit": ('''fn work() {}
var task = go work()
try Task.timeout(task, 100)
print("unit")
''', "unit\n", 0, ""),
    "timeout_error": (PRELUDE + '''var task = go fail()
try { Task.timeout(task, 100) }
catch(e: Failure) { print("business-error") }
catch(e: Timeout) { print("unexpected-timeout") }
''', "business-error\n", 0, ""),
    "timeout_cancel": (PRELUDE + '''fn main() {
    defer { print("root-cleanup") }
    const ready = Channel<i64>(1)
    var task = go waiting(ready)
    ready.recv()
    task.cancel()
    try { Task.timeout(task, 10000) } catch(e: Timeout) { print("caught-timeout") }
    catch panic(p) { print("caught-panic") }
    print("unreachable")
}
main()
''', "child-cleanup\nroot-cleanup\n", 1, "[Uncaught Cancellation]\n"),
    "timeout_expired": (PRELUDE + '''fn main() {
    const ready = Channel<i64>(1)
    var task = go waiting(ready)
    ready.recv()
    var timedOut = false
    try { Task.timeout(task, 0) } catch(e: Timeout) { timedOut = true }
    const terminal = task.awaitResult()
    assert(timedOut)
    print("timeout")
    print(terminal.ordinal)
    print("alive")
}
main()
''', "child-cleanup\ntimeout\n3\nalive\n", 0, ""),
    "parent_scope_cancel": (PRELUDE + '''fn parent(ready: Channel<i64>) {
    defer { print("parent-cleanup") }
    scope {
        go waiting(ready)
        time.sleep(10000)
    }
}
fn main() {
    defer { print("root-cleanup") }
    const ready = Channel<i64>(1)
    var task = go parent(ready)
    ready.recv()
    task.cancel()
    await task
    print("unreachable")
}
main()
''', "child-cleanup\nparent-cleanup\nroot-cleanup\n", 1, "[Uncaught Cancellation]\n"),
}

REJECTED = {
    "try": "fn value() -> i64 { return 1 }\nprint(try value())\n",
    "optional": "fn value() -> i64 { return 1 }\nprint(try? value())\n",
    "assert": "fn value() -> i64 { return 1 }\nprint(try! value())\n",
    "block": "try { print(1) } catch(e) { print(2) }\n",
    "await": "fn value() -> i64 { return 1 }\nvar t = go value()\nprint(try await t)\n",
    "send": "const ch = Channel<i64>(1)\ntry ch.send(1)\n",
    "nested": "enum E { Bad }\ntry { const f = fn() throws(E) { throw E.Bad }; print(1) } catch(e) {}\n",
    "missing_try": "enum E { Bad }\nfn fail() throws(E) { throw E.Bad }\nfail()\n",
    "timeout_missing_try": "fn value()->i64{return 42}\nvar t=go value()\nTask.timeout(t, 100)\n",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--engine", choices=("vm", "native"), required=True)
    parser.add_argument("--case", choices=tuple(CASES))
    args = parser.parse_args()
    binary = args.binary.resolve()
    env = dict(os.environ, PYTHONUTF8="1", NO_COLOR="1")
    failures = []
    with tempfile.TemporaryDirectory(prefix="await-cancel-") as directory:
        if not args.case:
            for name, code in REJECTED.items():
                source = Path(directory) / f"rejected_{name}.xr"
                source.write_text(code, encoding="utf-8")
                result = subprocess.run([str(binary), "check", str(source)], cwd=args.root,
                                        env=env, capture_output=True, timeout=30)
                diagnostic = b"E0330:" if name.endswith("missing_try") else b"E0336:"
                if result.returncode != 1 or result.stdout or diagnostic not in result.stderr:
                    failures.append(f"{name}: expected {diagnostic!r}, got {result.returncode}, {result.stderr!r}")
                else:
                    print(f"PASS {args.engine} rejected_{name}")
        for name, (code, stdout, status, stderr) in CASES.items():
            if args.case and args.case != name:
                continue
            source = Path(directory) / f"{name}.xr"
            source.write_text(code, encoding="utf-8")
            command = [str(binary), "run", str(source)]
            if args.engine == "native":
                executable = source.with_suffix(".exe" if os.name == "nt" else "")
                built = subprocess.run([str(binary), "build", str(source), "-o", str(executable)],
                                       cwd=args.root, env=env, capture_output=True, timeout=90)
                if built.returncode:
                    failures.append(f"{name}: build failed\n{built.stdout!r}\n{built.stderr!r}")
                    continue
                command = [str(executable)]
            result = subprocess.run(command, cwd=args.root, env=env, capture_output=True, timeout=30)
            actual = (result.stdout, result.returncode, result.stderr)
            expected = (stdout.encode(), status, stderr.encode())
            if actual != expected:
                failures.append(f"{name}: expected {expected!r}, got {actual!r}")
            else:
                print(f"PASS {args.engine} {name}")
    if failures:
        raise AssertionError("\n".join(failures))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
