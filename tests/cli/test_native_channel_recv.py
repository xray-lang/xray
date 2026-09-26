"""Compile public Channel receive identity and payload contracts."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    cases = {
        "closed_query": 'export fn probe() -> i64 { const ch = Channel<i64>(1); var before = ch.isClosed; ch.close(); if (before == false && ch.isClosed) { return 1 }; return 0 }',
        "empty": 'export fn probe() -> i64 { var ch = Channel<i64>(1); var r = ch.tryRecv(); print(r); return 0 }',
        "scalar_match": 'export fn probe() -> i64 { var ch = Channel<i64>(1); ch.trySend(42); return match(ch.tryRecv()) { Recv.Value { value: v } -> v, _ -> -1 } }',
        "string_match": 'export fn probe() -> i64 { var ch = Channel<string>(1); ch.trySend("ok"); return match(ch.tryRecv()) { Recv.Value { value: v } -> len(v), _ -> -1 } }',
        "array_move": 'export fn probe() -> i64 { var ch = Channel<Array<i64>>(1); var xs = [1, 2]; ch.trySend(move xs); return match(ch.tryRecv()) { Recv.Value { value: v } -> len(v), _ -> -1 } }',
    }
    cases["blocking_parameter"] = 'fn read(ch: Channel<i64>) -> i64 { return ch.recvOr(0) }\nexport fn probe() -> i64 { var ch = Channel<i64>(1); ch.trySend(42); return read(ch) }'
    cases["blocking_field"] = 'final class Holder { channel: Channel<i64>; constructor(ch: Channel<i64>) { this.channel = ch } }\nfn read(holder: Holder) -> i64 { return holder.channel.recvOr(0) }\nexport fn probe() -> i64 { var ch = Channel<i64>(1); ch.trySend(42); return read(Holder(ch)) }'
    cases["closed_parameter"] = 'fn closed(ch: Channel<i64>) -> bool { return ch.isClosed }\nexport fn probe() -> i64 { var ch = Channel<i64>(1); ch.close(); if (closed(ch)) { return 1 }; return 0 }'
    raise SystemExit(compile_cases(cases))
