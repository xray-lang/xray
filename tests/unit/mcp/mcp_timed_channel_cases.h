/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * mcp_timed_channel_cases.h - Real timed Source and inherited Task clocks
 */
#ifndef MCP_TIMED_CHANNEL_CASES_H
#define MCP_TIMED_CHANNEL_CASES_H
static const char mcp_timed_channel_code[] =
    "fn sent(result:SendResult)->i64 { return match(result) { SendResult.Sent -> 1, SendResult.Full -> -1, SendResult.Timeout -> -2, SendResult.Closed -> -3 } }\n"
    "fn take(ch:Channel<i64>)->i64 { return match(ch.recvTimeout(5)) { Recv.Value { value:item } -> item, Recv.Empty -> -1, Recv.Timeout -> -2, Recv.Closed -> -3 } }\n"
    "fn tick(counter:Atomic<i64>) { counter.add(1) }\n"
    "fn main() throws(Error)->i64 {\n"
    " const buffered=Channel<i64>(1)\n"
    " const sentValue=sent(buffered.sendTimeout(42,0))\n"
    " const received=take(buffered)\n"
    " const expired=take(buffered)\n"
    " const child=try await go take(Channel<i64>())\n"
    " const counter=Atomic<i64>(0)\n"
    " const unit=Channel<()>()\n"
    " const unitExpired=sent(unit.sendTimeout(tick(counter),5))\n"
    " return sentValue*10000+received*100+expired*10+child+unitExpired+counter.load()\n"
    "}\n"
    "print(try main())\n"
    ;
#endif // MCP_TIMED_CHANNEL_CASES_H
