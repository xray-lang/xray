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

CANCELLABLE_ERROR = '''import time
enum Failure { Bad }
fn waiting(ready: Channel<i64>) throws(Failure) -> i64 {
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
    "a03_panic_only_pure": ('''try { print(42) }
catch panic { print("unexpected") }
''', "42\n", 0, ""),
    "a03_a07_a08_spec": ('''enum Failure { Bad }
fn fail() throws(Failure) { throw Failure.Bad }
fn relay() throws(Failure) {
    try { fail() } catch panic { print("unexpected panic") }
}
const ch = Channel<i64>(1)
ch.close()
try { ch.send(1) } catch panic (p) { print(p.code) }
try { relay() } catch (e: Failure) { print("business error") }
try { throw Failure.Bad } catch (e: Failure) { print("direct throw") }
''', "702\nbusiness error\ndirect throw\n", 0, ""),
    "a02_error_expressions": ('''enum Failure { Bad }
fn fail() throws(Failure) -> i64 { throw Failure.Bad }
try { print(try fail()) } catch (e: Failure) { print("propagated") }
print((try? fail()) == null)
try { print(try! fail()) } catch panic (p) { print(p.code) }
''', "propagated\ntrue\n446\n", 0, ""),
    "a08_direct_throw_mixed": ('''enum Failure { Bad }
try { throw Failure.Bad }
catch (e: Failure) { print("business") }
catch panic { print("unexpected") }
''', "business\n", 0, ""),
    "a10_consumed_sources": ('''enum Failure { Bad }
fn maybe() throws(Failure) -> i64 { return 42 }
try { print((try? maybe()) ?? 0) } catch (e: Failure) { print("unexpected") }
try { print(try! maybe()) } catch (e: Failure) { print("unexpected") }
try { try { maybe() } catch (e: Failure) {} }
catch (e: Failure) { print("unexpected") }
print("alive")
''', "42\n42\nalive\n", 0, ""),
    "a11_generic_business": ('''enum Failure { Bad }
fn maybe() throws(Failure) -> i64 { return 42 }
fn generic<E: Error>(action: fn() throws(E) -> i64) throws(E) -> i64 {
    return try action()
}
print(try generic<Failure>(maybe))
''', "42\n", 0, ""),
    "a10_dead_handler_generic_storage": ('''enum Failure { Bad }
fn maybe() throws(Failure) -> i64 { return 42 }
fn identity<T>(value: T) -> T { return value }
print(identity<i64>(1))
try { print((try? maybe()) ?? 0) }
catch (e: Failure) {
    print(identity<i64>(2))
    print(identity<i64>(3))
    print(identity<i64>(4))
}
print(identity<i64>(5))
print(identity<i64>(6))
''', "1\n42\n5\n6\n", 0, ""),
    "a12_declared_indirect": ('''enum Failure { Bad }
fn maybe() throws(Failure) -> i64 { return 42 }
var action: fn() throws(Failure) -> i64 = maybe
print(try action())
''', "42\n", 0, ""),
    "a15_separate_channels": ('''enum Failure { Bad }
fn action(panicNow: bool) throws(Failure) {
    if (panicNow) { assert(false) }
    throw Failure.Bad
}
try { action(false) }
catch (e: Failure) { print("business") }
catch panic { print("unexpected") }
try { action(true) }
catch (e: Failure) { print("unexpected") }
catch panic { print("panic") }
''', "business\npanic\n", 0, ""),
    "a16_handler_escape": ('''enum Failure { First, Second }
fn fail() throws(Failure) { throw Failure.First }
try {
    try { fail() } catch (e: Failure) { throw Failure.Second }
} catch (e: Failure) { print(e.ordinal) }
try {
    try { fail() } catch (e: Failure) { assert(false) }
} catch panic { print("outer panic") }
''', "1\nouter panic\n", 0, ""),
    "a17_independent_panic": ('''enum Failure { Bad }
fn crash() throws(Failure) -> i64 {
    const ch = Channel<i64>(1)
    ch.close()
    ch.send(1)
    return 42
}
try { const value = try? crash(); print("unexpected") }
catch panic (p) { print(p.code) }
try { const value = try! crash(); print("unexpected") }
catch panic (p) { print(p.code) }
''', "702\n702\n", 0, ""),
    "a17_optional_cancellation": (CANCELLABLE_ERROR + '''fn main() {
    defer { print("root-cleanup") }
    const ready = Channel<i64>(1)
    var task = go waiting(ready)
    ready.recv()
    task.cancel()
    try { const value = try? await task; print("unreachable") }
    catch (e: Failure) { print("caught-error") }
    catch panic { print("caught-panic") }
}
main()
''', "child-cleanup\nroot-cleanup\n", 1, "[Uncaught Cancellation]\n"),
    "a17_assert_cancellation": (CANCELLABLE_ERROR + '''fn main() {
    defer { print("root-cleanup") }
    const ready = Channel<i64>(1)
    var task = go waiting(ready)
    ready.recv()
    task.cancel()
    try { const value = try! await task; print("unreachable") }
    catch (e: Failure) { print("caught-error") }
    catch panic { print("caught-panic") }
}
main()
''', "child-cleanup\nroot-cleanup\n", 1, "[Uncaught Cancellation]\n"),
    "timeout_parenthesized_read": ('''fn value() -> i64 { return 42 }
fn main() throws(Timeout) {
    var task = go value()
    var milliseconds = 100
    print(try Task.timeout(((task)), ((milliseconds))))
}
try main()
''', "42\n", 0, ""),
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
    "a05_mixed_without_error": "try { print(42) } catch(e) {} catch panic {}\n",
    "a06_inner_try_send": "const ch=Channel<i64>(1)\ntry { try ch.send(1) } catch panic {}\n",
    "a06_inner_try_await": "fn value()->i64{return 42}\nvar t=go value()\ntry { print(try await t) } catch panic {}\n",
    "a07_panic_only_undeclared": "enum Failure { Bad }\nfn fail() throws(Failure){throw Failure.Bad}\nfn relay(){try { fail() } catch panic {}}\nrelay()\n",
    "a09_handler_only": "enum Failure { Bad }\nfn f() throws(Failure){try { print(1) } catch(e){throw Failure.Bad}}\ntry f()\n",
    "a11_caller_never": "fn value()->i64{return 42}\nfn generic<E: Error>(action:fn() throws(E)->i64) throws(E)->i64{return try action()}\nprint(try generic<Never>(value))\n",
    "a10_dead_handler_redundant": "enum Failure { Bad }\nfn maybe() throws(Failure)->i64{return 42}\ntry { print((try? maybe()) ?? 0) } catch(e: Failure) { try { print(1) } catch(inner) {} }\n",
    "a13_explicit_skips_default": "enum Failure { Bad }\nfn read() throws(Failure)->i64{throw Failure.Bad}\nfn value(x:i64=try read())->i64{return x}\nprint(try value(42))\n",
    "a14_panic_scope": "try scope { go fn(){assert(false)}() }\n",
    "a14_cancel_scope": "try scope { var task=go fn(){}(); task.cancel() }\n",
    "timeout_grouped_move_task": "fn value()->i64{return 42}\nfn main() throws(Timeout){var task=go value();var ms=100;print(try Task.timeout((move task),ms))}\ntry main()\n",
    "timeout_grouped_move_duration": "fn value()->i64{return 42}\nfn main() throws(Timeout){var task=go value();var ms=100;print(try Task.timeout(task,(move ms)))}\ntry main()\n",
    "timeout_ref_task": "fn value()->i64{return 42}\nfn main() throws(Timeout){var task=go value();print(try Task.timeout(ref task,100))}\ntry main()\n",
    "timeout_ref_duration": "fn value()->i64{return 42}\nfn main() throws(Timeout){var task=go value();var ms=100;print(try Task.timeout(task,ref ms))}\ntry main()\n",
}

REJECTION_DIAGNOSTICS = {
    "a05_mixed_without_error": b"keep only the panic boundary and review ordinary handler logic before removal",
    "a06_inner_try_send": b"remove the marker and preserve the expression",
    "a07_panic_only_undeclared": b"E0331:",
    "timeout_grouped_move_task": b"Task.timeout takes ordinary readonly Task and i64 arguments",
    "timeout_grouped_move_duration": b"Task.timeout takes ordinary readonly Task and i64 arguments",
    "timeout_ref_task": b"Task.timeout requires its Task and i64 duration",
    "timeout_ref_duration": b"Task.timeout requires its Task and i64 duration",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--engine", choices=("vm", "native"), required=True)
    parser.add_argument("--case", choices=(*CASES, *REJECTED))
    args = parser.parse_args()
    binary = args.binary.resolve()
    env = dict(os.environ, PYTHONUTF8="1", NO_COLOR="1")
    failures = []
    with tempfile.TemporaryDirectory(prefix="await-cancel-") as directory:
        for name, code in REJECTED.items():
            if args.case and args.case != name:
                continue
            source = Path(directory) / f"rejected_{name}.xr"
            source.write_text(code, encoding="utf-8")
            result = subprocess.run([str(binary), "check", str(source)], cwd=args.root,
                                    env=env, capture_output=True, timeout=30)
            diagnostic = REJECTION_DIAGNOSTICS.get(name,
                b"E0330:" if name.endswith("missing_try") else b"E0336:")
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
