"""Verify managed channel field storage and array insertion through generated C."""
from native_source_compile import compile_cases


if __name__ == "__main__":
    cases = {
        "direct": """export fn probe() -> i64 {
 var channel = Channel<string>(1)
 var channels: Array<Channel<string>> = []
 channels.push(channel)
 return len(channels)
}
""",
        "field": """type Holder = { notifications: Channel<string> }
export fn probe() -> i64 {
 var record: Holder = { notifications: Channel<string>(1) }
 var channels: Array<Channel<string>> = []
 channels.push(record.notifications)
 return len(channels)
}
""",
    }
    cases["const_constructor_argument"] = """
final class Holder {
    channel: Channel<i64>
    constructor(channel: Channel<i64>) { this.channel = channel }
}
export fn probe() -> i64 {
    const channel = Channel<i64>(1)
    var holder = Holder(channel)
    return len(holder.channel)
}
"""
    cases["const_defer_close"] = """export fn probe() -> i64 {
 const notifications = Channel<string>(1)
 defer { notifications.close() }
 return len(notifications)
}
"""
    raise SystemExit(compile_cases(cases))
