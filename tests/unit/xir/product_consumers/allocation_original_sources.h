/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * allocation_original_sources.h - Exact legacy allocation source bodies
 *
 * KEY CONCEPT:
 *   Complete original bytes are the preflight oracle; no wrapper is inserted.
 */
static const char *const allocation_original_sources[] = {
    "fn answer() -> i64 { return 0 }\nfn exported() -> i64 { return 42 }\n@test\nfn checkAnswer() { assert(exported() == 42) }\n",
    "fn answer() -> i64 {\n const counter = Atomic(40)\n const alias = counter\n var (old, matched) = alias.compareExchange(40, 42)\n counter.store(42, Ordering.Release)\n counter.add(1)\n counter.sub(1)\n return counter.load()\n}\nfn exported() -> i64 { return 42 }\n@test\nfn checkAnswer() { assert(exported() == 42) }\n",
    "fn answer() -> i64 {\n const flag = Atomic(false)\n const alias = flag\n var (old, matched) = alias.compareExchange(false, true)\n flag.store(true, Ordering.AcquireRelease)\n const before = flag.toggle(Ordering.Relaxed)\n if (matched && before) { return 42 }\n return 0\n}\nfn exported() -> i64 { return 42 }\n@test\nfn checkAnswer() { assert(exported() == 42) }\n",
    "fn identity(value: f64) -> f64 { return value }\nfn answer() -> i64 {\n const value = identity(1.5)\n const other = identity(2.25)\n const counter = Atomic(value)\n const old = counter.fetchAdd(other)\n var (before, matched) = counter.compareExchange(3.75, 1.5)\n counter.store(1.5, Ordering.Release)\n const text = counter.load().toString()\n if (old < other && len(text) == 3) { return 42 }\n return 0\n}\nfn exported() -> i64 { return 42 }\n@test\nfn checkAnswer() { assert(exported() == 42) }\n",
    "type State = { token:i64, enabled:bool, nested:{ text:string }? }\nvar state:State?=null\nfn answer() -> i64 { return 42 }\nfn exported() -> i64 { return 42 }\n@test\nfn checkAnswer() { assert(exported() == 42) }\n",
    "type Child = { text:string }\ntype State = { token:i64, child:Child }\nfn answer() -> i64 {\n var child:Child={text:\"before\"}\n var state:State={child:child,token:7}\n var alias=state\n alias.token=42\n alias.child.text=\"after\"\n if (state.child.text == \"after\") { return state.token }\n return 0\n}\nfn exported() -> i64 { return 42 }\n@test\nfn checkAnswer() { assert(exported() == 42) }\n",
    "import { NetConn } from net\nvar connection:NetConn?=null\nfn answer() -> i64 { return 42 }\nfn exported() -> i64 { return 42 }\n@test\nfn checkAnswer() { assert(exported() == 42) }\n",
    "fn answer() -> i64 {\n var text = StringBuilder()\n text.append(\"first\").append(42)\n const snapshot = text.toString()\n text.clear().append(\"second\")\n assert(snapshot == \"first42\")\n return 42\n}\nfn exported() -> i64 { return 42 }\n@test\nfn checkAnswer() { assert(exported() == 42) }\n",
};
