"""Compile public Channel send contracts without relying on stdlib breadth."""
from native_source_compile import compile_cases


if __name__ == "__main__":
    cases = {
        "string_local": 'export fn probe() -> i64 { var ch = Channel<string>(1); ch.trySend("ok"); return 0 }',
        "string_parameter": 'fn notify(ch: Channel<string>, value: string) { ch.trySend(value) }\nexport fn probe() -> i64 { var ch = Channel<string>(1); notify(ch, "ok"); return 0 }',
        "send_result": 'export fn probe() -> i64 { var ch = Channel<i64>(1); return match (ch.trySend(7)) { SendResult.Sent -> 1, SendResult.Full -> 2, SendResult.Timeout -> 3, SendResult.Closed -> 4 } }',
        "copy_array": 'export fn probe() -> i64 { var ch = Channel<Array<i64>>(1); var xs = [1, 2]; ch.trySend(copy(xs)); xs.push(3); return len(xs) }',
        "move_array": 'export fn probe() -> i64 { var ch = Channel<Array<i64>>(1); var xs = [1, 2]; ch.trySend(move xs); return 0 }',
    }
    raise SystemExit(compile_cases(cases))
