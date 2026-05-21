/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_knowledge.c - Built-in knowledge for MCP tools
 *
 * KEY CONCEPT:
 *   Embeds language syntax topics and stdlib module info as static
 *   strings so the MCP server works without external files.
 *   Topics are matched by name or alias with case-insensitive search.
 */

#include "xmcp_knowledge.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <string.h>
#include <ctype.h>

#ifdef XR_OS_WINDOWS
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif
#include <stdio.h>

/* ======================================================================
 * Syntax topic content (compiled-in)
 * ====================================================================== */

static const char TOPIC_VARIABLES[] =
    "## Variables\n"
    "\n"
    "```xray\n"
    "let x = 1              // mutable\n"
    "const PI = 3.14        // immutable\n"
    "let name: string = \"Alice\"  // with type annotation\n"
    "let age: int? = null   // nullable\n"
    "```\n"
    "\n"
    "### Modifiers\n"
    "- `let` — mutable, can be reassigned\n"
    "- `const` — immutable, cannot be reassigned\n"
    "- `shared const` — immutable, readable across coroutines (zero-copy)\n"
    "- `shared let` — mutable, uses move semantics for ownership transfer\n"
    "\n"
    "### Multiple assignment\n"
    "```xray\n"
    "let a, b = 1, 2\n"
    "let q, r = divmod(17, 5)  // from function returning multiple values\n"
    "```\n"
    "\n"
    "### Destructuring\n"
    "```xray\n"
    "let [a, b, c] = [10, 20, 30]    // array destructuring\n"
    "let { name, age } = obj          // object destructuring (no renaming)\n"
    "```\n";

static const char TOPIC_TYPES[] = "## Types\n"
                                  "\n"
                                  "### Primitive types\n"
                                  "- `int` — 64-bit integer\n"
                                  "- `float` — 64-bit double\n"
                                  "- `string` — UTF-8 string\n"
                                  "- `bool` — true / false\n"
                                  "- `void` — no value\n"
                                  "\n"
                                  "### Nullable\n"
                                  "```xray\n"
                                  "let x: int? = null     // nullable int\n"
                                  "let v = x ?? 42        // null coalescing\n"
                                  "let y = obj?.field      // optional chaining\n"
                                  "```\n"
                                  "\n"
                                  "### Collection types\n"
                                  "- `Array<T>` — ordered list\n"
                                  "- `Map<K,V>` — hash map (`#{\"key\": val}`)\n"
                                  "- `Set<T>` — unique elements (`#[1, 2, 3]`)\n"
                                  "- `Json` — dynamic JSON object\n"
                                  "- `Bytes` — byte buffer\n"
                                  "- `BigInt` — arbitrary precision integer\n"
                                  "- `Channel<T>` — inter-coroutine communication\n"
                                  "\n"
                                  "### Union types\n"
                                  "```xray\n"
                                  "let value: int | string = 42   // can hold int or string\n"
                                  "let result: int | null = null  // equivalent to int?\n"
                                  "```\n"
                                  "Note: `T?` is shorthand for `T | null`.\n";

static const char TOPIC_FUNCTIONS[] =
    "## Functions\n"
    "\n"
    "### Named functions (parameters MUST have type annotations)\n"
    "```xray\n"
    "fn add(a: int, b: int) -> int {\n"
    "    return a + b\n"
    "}\n"
    "```\n"
    "\n"
    "### Arrow functions (MUST have type annotations)\n"
    "```xray\n"
    "let double = (x: int) -> x * 2\n"
    "let clamp = fn(x: int, lo: int, hi: int) -> int {\n"
    "    if (x < lo) { return lo }\n"
    "    if (x > hi) { return hi }\n"
    "    return x\n"
    "}\n"
    "```\n"
    "\n"
    "### Default parameters\n"
    "```xray\n"
    "fn greet(name: string, greeting: string = \"Hello\") -> string {\n"
    "    return \"${greeting}, ${name}!\"\n"
    "}\n"
    "```\n"
    "\n"
    "### Multiple return values\n"
    "```xray\n"
    "fn divmod(a: int, b: int) -> (int, int) {\n"
    "    return (a / b, a % b)\n"
    "}\n"
    "let q, r = divmod(17, 5)\n"
    "```\n"
    "\n"
    "### Rest parameters (no type annotation on rest param)\n"
    "```xray\n"
    "fn sum(...nums: int) -> int {\n"
    "    let total = 0\n"
    "    for (let i = 0; i < nums.length; i++) { total = total + nums[i] }\n"
    "    return total\n"
    "}\n"
    "```\n"
    "\n"
    "### Higher-order functions\n"
    "```xray\n"
    "fn apply(f: (int) -> int, x: int) -> int { return f(x) }\n"
    "```\n";

static const char TOPIC_CONTROL_FLOW[] =
    "## Control Flow\n"
    "\n"
    "### if / else\n"
    "```xray\n"
    "if (condition) { ... } else if (other) { ... } else { ... }\n"
    "```\n"
    "\n"
    "### while\n"
    "```xray\n"
    "while (condition) { ... }\n"
    "```\n"
    "\n"
    "### for (C-style)\n"
    "```xray\n"
    "for (let i = 0; i < 10; i++) { ... }\n"
    "```\n"
    "\n"
    "### for-in (range)\n"
    "```xray\n"
    "for (i in 0..10) { ... }       // 0 to 9\n"
    "for (item in array) { ... }    // iterate array\n"
    "```\n"
    "\n"
    "### match (pattern matching)\n"
    "```xray\n"
    "let result = match (x) {\n"
    "    1 -> \"one\",\n"
    "    2, 3 -> \"two or three\",\n"
    "    4..10 -> \"four to ten\",\n"
    "    n if (n < 0) -> \"negative\",\n"
    "    _ -> \"other\"\n"
    "}\n"
    "```\n"
    "\n"
    "### try / catch / finally\n"
    "```xray\n"
    "try { ... } catch (e) { print(e) } finally { cleanup() }\n"
    "throw \"error message\"\n"
    "```\n";

static const char TOPIC_CLASS[] =
    "## Classes\n"
    "\n"
    "```xray\n"
    "class Animal {\n"
    "    name: string\n"
    "    constructor(name: string) { this.name = name }\n"
    "    fn speak() -> string { return \"${this.name} says ...\" }\n"
    "}\n"
    "\n"
    "class Dog extends Animal {\n"
    "    constructor(name: string) { super(name) }\n"
    "    override fn speak() -> string { return \"${this.name} says woof!\" }\n"
    "}\n"
    "```\n"
    "\n"
    "### Features\n"
    "- `constructor()` — called via `new ClassName()`\n"
    "- `extends` — single inheritance\n"
    "- `override` — required keyword when overriding methods\n"
    "- `static` — class-level methods\n"
    "- `private` — field visibility\n"
    "- `this` — current instance reference\n"
    "- `super` — parent class reference\n";

static const char TOPIC_STRUCT[] =
    "## Structs\n"
    "\n"
    "Value types (no inheritance). Created with `StructName{field: value}` syntax.\n"
    "\n"
    "```xray\n"
    "struct Vec2 {\n"
    "    x: float\n"
    "    y: float\n"
    "    fn length() -> float {\n"
    "        return (this.x * this.x + this.y * this.y).sqrt()\n"
    "    }\n"
    "}\n"
    "let p = Vec2{x: 3.0, y: 4.0}\n"
    "```\n";

static const char TOPIC_INTERFACE[] =
    "## Interfaces\n"
    "\n"
    "```xray\n"
    "interface Shape {\n"
    "    area() -> float\n"
    "    describe() -> string\n"
    "}\n"
    "\n"
    "class Circle implements Shape {\n"
    "    radius: float\n"
    "    constructor(r: float) { this.radius = r }\n"
    "    fn area() -> float { return 3.14159 * this.radius * this.radius }\n"
    "    fn describe() -> string { return \"Circle(r=${this.radius})\" }\n"
    "}\n"
    "```\n";

static const char TOPIC_ENUM[] = "## Enums\n"
                                 "\n"
                                 "```xray\n"
                                 "enum Color { Red, Green, Blue }\n"
                                 "print(Color.Red.name)       // \"Red\"\n"
                                 "print(Color.Red.value)      // 0\n"
                                 "print(Color.memberCount)    // 3\n"
                                 "\n"
                                 "// Custom values\n"
                                 "enum HttpStatus {\n"
                                 "    OK = 200,\n"
                                 "    NotFound = 404,\n"
                                 "    ServerError = 500\n"
                                 "}\n"
                                 "\n"
                                 "// Iteration\n"
                                 "for (c in Color) { print(c.name) }\n"
                                 "\n"
                                 "// Match\n"
                                 "match (status) {\n"
                                 "    HttpStatus.OK -> \"Success\",\n"
                                 "    HttpStatus.NotFound -> \"Not Found\",\n"
                                 "    _ -> \"Unknown\"\n"
                                 "}\n"
                                 "```\n";

static const char TOPIC_GENERICS[] =
    "## Generics\n"
    "\n"
    "```xray\n"
    "class Box<T> {\n"
    "    value: T\n"
    "    constructor(v: T) { this.value = v }\n"
    "    fn get() -> T { return this.value }\n"
    "    fn map(f: (T) -> T) -> Box<T> { return new Box(f(this.value)) }\n"
    "}\n"
    "\n"
    "let intBox = new Box(42)      // T inferred as int\n"
    "let strBox = new Box(\"hello\") // T inferred as string\n"
    "```\n"
    "\n"
    "### Multi-parameter generics\n"
    "```xray\n"
    "class Pair<A, B> {\n"
    "    first: A\n"
    "    second: B\n"
    "    constructor(a: A, b: B) { this.first = a; this.second = b }\n"
    "}\n"
    "```\n";

static const char TOPIC_COLLECTIONS[] = "## Collections\n"
                                        "\n"
                                        "### Array\n"
                                        "```xray\n"
                                        "let arr = [1, 2, 3, 4, 5]\n"
                                        "arr.push(6); arr.pop()\n"
                                        "arr[1:3]                       // slice [2, 3]\n"
                                        "arr.map((x: int) -> x * 2)\n"
                                        "arr.filter((x: int) -> x > 2)\n"
                                        "arr.reduce((acc: int, x: int) -> acc + x, 0)\n"
                                        "arr.find((x: int) -> x > 3)\n"
                                        "arr.sort(); arr.reverse()\n"
                                        "arr.indexOf(3); arr.includes(5)\n"
                                        "arr.join(\", \")\n"
                                        "```\n"
                                        "\n"
                                        "### Map\n"
                                        "```xray\n"
                                        "let m = #{\"a\": 1, \"b\": 2}\n"
                                        "m.get(\"a\"); m.set(\"c\", 3); m.delete(\"b\")\n"
                                        "m.has(\"a\"); m.length\n"
                                        "m.keys(); m.values()\n"
                                        "```\n"
                                        "\n"
                                        "### Set\n"
                                        "```xray\n"
                                        "let s = #[1, 2, 3]\n"
                                        "s.add(4); s.delete(1)\n"
                                        "s.has(3); s.length\n"
                                        "```\n"
                                        "\n"
                                        "### Json (dynamic object)\n"
                                        "```xray\n"
                                        "import json\n"
                                        "let obj: Json = { name: \"Alice\", age: 30 }\n"
                                        "obj.name                      // field access\n"
                                        "obj.missing                   // returns null\n"
                                        "json.keys(obj)                // [\"name\", \"age\"]\n"
                                        "json.stringify(obj)           // JSON string\n"
                                        "json.parse('{\"x\":1}')       // parse string to Json\n"
                                        "```\n";

static const char TOPIC_STRING[] = "## Strings\n"
                                   "\n"
                                   "### String interpolation\n"
                                   "```xray\n"
                                   "let name = \"World\"\n"
                                   "print(\"Hello, ${name}!\")     // template literal\n"
                                   "```\n"
                                   "Note: cannot use quotes inside `${}`. Use a variable instead.\n"
                                   "\n"
                                   "### Methods\n"
                                   "```xray\n"
                                   "s.length; s.toLowerCase(); s.toUpperCase()\n"
                                   "s.startsWith(\"He\"); s.endsWith(\"!\")\n"
                                   "s.indexOf(\"sub\"); s.includes(\"sub\")\n"
                                   "s.trim(); s.split(\",\")\n"
                                   "s.replace(\"old\", \"new\")\n"
                                   "s[0:5]                        // substring slice\n"
                                   "```\n"
                                   "\n"
                                   "### Single-quoted strings\n"
                                   "```xray\n"
                                   "let raw = 'no ${interpolation} here'\n"
                                   "```\n";

static const char TOPIC_CHANNEL[] =
    "## Channel\n"
    "\n"
    "Channels are the primary inter-coroutine communication mechanism.\n"
    "\n"
    "### Declaration (MUST be const)\n"
    "```xray\n"
    "shared const ch = new Channel<int>(10)\n"
    "```\n"
    "`let ch = new Channel<int>(10)` is a **compile error** for coroutine sharing.\n"
    "\n"
    "### Operations\n"
    "```xray\n"
    "ch.send(value)                   // blocking send\n"
    "let val = ch.recv()              // blocking receive (null on closed)\n"
    "let val, ok = ch.tryRecv()       // non-blocking receive\n"
    "ch.close()                       // close channel\n"
    "```\n"
    "\n"
    "### Function parameter type\n"
    "```xray\n"
    "fn producer(ch: Channel<int>) {  // use Channel<T> in type position\n"
    "    ch.send(42)\n"
    "}\n"
    "```\n"
    "\n"
    "### Concurrency rules\n"
    "- Channels can be captured by `go` closures (exception to shared rule)\n"
    "- Values sent through channel are deep-copied (pointer types)\n"
    "- Channel is reference-counted on system heap\n";

static const char TOPIC_COROUTINE[] = "## Coroutines & Concurrency\n"
                                      "\n"
                                      "### go — spawn a coroutine\n"
                                      "```xray\n"
                                      "let task = go compute(42)      // returns a Task\n"
                                      "let result = await task         // wait for result\n"
                                      "```\n"
                                      "\n"
                                      "### await all / await any\n"
                                      "```xray\n"
                                      "let results = await all [t1, t2, t3]   // wait for all\n"
                                      "let first = await any [t1, t2]         // first to finish\n"
                                      "```\n"
                                      "\n"
                                      "### scope — structured concurrency\n"
                                      "```xray\n"
                                      "scope {\n"
                                      "    go taskA()\n"
                                      "    go taskB()\n"
                                      "}  // waits for ALL goroutines to finish\n"
                                      "```\n"
                                      "\n"
                                      "### select — multiplex channels\n"
                                      "```xray\n"
                                      "select {\n"
                                      "    msg from ch1 -> { handle(msg) }\n"
                                      "    msg from ch2 -> { handle(msg) }\n"
                                      "    after 1000 -> { print(\"timeout\") }\n"
                                      "}\n"
                                      "```\n"
                                      "\n"
                                      "### defer — LIFO cleanup\n"
                                      "```xray\n"
                                      "fn process() {\n"
                                      "    defer { cleanup() }   // runs when function exits\n"
                                      "    doWork()\n"
                                      "}\n"
                                      "```\n";

static const char TOPIC_CONCURRENCY_RULES[] =
    "## Concurrency Safety Rules\n"
    "\n"
    "**Golden rule: If it compiles, it's concurrency-safe.**\n"
    "\n"
    "### Three ways to share data across coroutines:\n"
    "\n"
    "| Mechanism | Syntax | How it works |\n"
    "|-----------|--------|-------------|\n"
    "| shared const | `shared const CFG = {...}` | Zero-copy reads, immutable |\n"
    "| Channel | `shared const ch = new Channel<T>(n)` | Deep-copy on send |\n"
    "| Function params | `go fn(data)` | Deep-copied to child |\n"
    "\n"
    "### What you CANNOT do (compiler rejects these):\n"
    "```xray\n"
    "let x = [1,2,3]\n"
    "go fn() { print(x) }()    // ERROR: cannot capture let variable\n"
    "\n"
    "let ch = new Channel<int>(1) // ERROR: Channel must be shared const for coroutine sharing\n"
    "\n"
    "shared let data = [1,2,3]\n"
    "go fn() { data.push(4) }() // ERROR: cannot capture shared let\n"
    "```\n"
    "\n"
    "### Move semantics\n"
    "```xray\n"
    "shared let data = [1,2,3]\n"
    "go fn() { let local = move data }()  // ownership transferred\n"
    "print(data)  // ERROR: data already moved\n"
    "```\n";

static const char TOPIC_MODULES[] = "## Modules & Imports\n"
                                    "\n"
                                    "```xray\n"
                                    "import http\n"
                                    "import json\n"
                                    "import time\n"
                                    "\n"
                                    "// Use module functions\n"
                                    "http.route(\"GET\", \"/\", handler)\n"
                                    "let data = json.parse(text)\n"
                                    "time.sleep(100)\n"
                                    "\n"
                                    "// Export from your module\n"
                                    "export fn helper() { ... }\n"
                                    "export class MyClass { ... }\n"
                                    "```\n";

static const char TOPIC_TESTING[] = "## Testing\n"
                                    "\n"
                                    "```xray\n"
                                    "@test\n"
                                    "fn test_add() {\n"
                                    "    assert_eq(1 + 1, 2)\n"
                                    "    assert_true(3 > 2)\n"
                                    "    assert_false(1 > 2)\n"
                                    "    assert_ne(\"a\", \"b\")\n"
                                    "}\n"
                                    "\n"
                                    "@test(skip)\n"
                                    "fn test_todo() { ... }  // skipped\n"
                                    "\n"
                                    "@test(timeout: 5000)\n"
                                    "fn test_slow() { ... }  // 5s timeout\n"
                                    "\n"
                                    "// Assert that a function throws\n"
                                    "assert_throws(fn() { throw new Exception(\"error\") })\n"
                                    "```\n"
                                    "\n"
                                    "Run with: `xray test <file_or_dir>`\n";

static const char TOPIC_OPERATORS[] =
    "## Operators\n"
    "\n"
    "### Arithmetic: `+`, `-`, `*`, `/`, `%`, `**` (power)\n"
    "### Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`\n"
    "### Logical: `&&`, `||`, `!`\n"
    "### Bitwise: `&`, `|`, `^`, `~`, `<<`, `>>`\n"
    "### Assignment: `=`, `+=`, `-=`, `*=`, `/=`, `%=`\n"
    "### Increment/Decrement: `i++`, `i--` (variables only, not fields)\n"
    "### Null coalescing: `x ?? default`\n"
    "### Optional chaining: `obj?.field`\n"
    "### Spread: `...arr`\n"
    "### Type: `typeof(x)`, `x is Type`\n";

static const char TOPIC_BUILTIN[] = "## Built-in Functions\n"
                                    "\n"
                                    "- `print(value)` — print with newline\n"
                                    "- `dump(value)` — debug print with type info\n"
                                    "- `typeof(value)` — returns type as int\n"
                                    "- `int(value)` — convert to int\n"
                                    "- `float(value)` — convert to float\n"
                                    "- `string(value)` — convert to string\n"
                                    "- `bool(value)` — convert to bool\n"
                                    "- `assert(condition)` — assert truthy\n"
                                    "- `assert_eq(a, b)` — assert equal\n"
                                    "- `assert_ne(a, b)` — assert not equal\n"
                                    "- `assert_true(x)` — assert true\n"
                                    "- `assert_false(x)` — assert false\n"
                                    "- `assert_throws(fn)` — assert fn throws\n";

/* ======================================================================
 * Topic registry
 * ====================================================================== */

typedef struct {
    const char *name;
    const char *aliases;
    const char *content;
} TopicDef;

static const TopicDef BUILTIN_TOPICS[] = {
    {"variables", "let,const,var,shared,declaration", TOPIC_VARIABLES},
    {"types", "int,float,string,bool,nullable,type", TOPIC_TYPES},
    {"functions", "fn,function,arrow,closure,lambda,params", TOPIC_FUNCTIONS},
    {"control_flow", "if,else,while,for,loop,match,switch,try,catch,throw,error_handling",
     TOPIC_CONTROL_FLOW},
    {"class", "classes,oop,inheritance,extends,override,constructor", TOPIC_CLASS},
    {"struct", "structs,value_type", TOPIC_STRUCT},
    {"interface", "interfaces,implements", TOPIC_INTERFACE},
    {"enum", "enums,enumeration", TOPIC_ENUM},
    {"generics", "generic,type_parameter,template", TOPIC_GENERICS},
    {"collections", "array,map,set,list,dict,dictionary,json,bytes", TOPIC_COLLECTIONS},
    {"string", "strings,string_methods,interpolation,template", TOPIC_STRING},
    {"channel", "channels,chan,Channel,send,recv,buffered", TOPIC_CHANNEL},
    {"coroutine", "go,goroutine,goroutines,await,task,async,concurrency,scope,select,defer",
     TOPIC_COROUTINE},
    {"concurrency_rules", "shared_rules,sharing,move,safety,concurrent", TOPIC_CONCURRENCY_RULES},
    {"modules", "import,export,module,require,package", TOPIC_MODULES},
    {"testing", "test,assert,assert_eq,unit_test", TOPIC_TESTING},
    {"operators", "operator,arithmetic,comparison,logical,bitwise", TOPIC_OPERATORS},
    {"builtin_functions", "builtin,print,dump,typeof,builtins", TOPIC_BUILTIN},
    {NULL, NULL, NULL}};

/* ======================================================================
 * Stdlib module registry
 * ====================================================================== */

static const XmcpModule BUILTIN_MODULES[] = {
    {"base64", "Base64 encoding/decoding"},
    {"cluster", "Distributed cluster communication (P2P, pub/sub, RPC)"},
    {"compress", "Compression/decompression (zlib)"},
    {"crypto", "Cryptographic hashing and encryption"},
    {"csv", "CSV parsing and generation"},
    {"datetime", "Date and time manipulation"},
    {"encoding", "Character encoding conversion"},
    {"gc", "Garbage collector control interface"},
    {"http", "HTTP client and server (REST API, routing, middleware)"},
    {"io", "File I/O operations"},
    {"json", "JSON parse/stringify, keys/values helpers"},
    {"log", "Structured logging system"},
    {"math", "Mathematical functions (sin, cos, sqrt, etc.)"},
    {"net", "TCP/UDP/TLS networking"},
    {"os", "Operating system interface (env, exec, signals)"},
    {"path", "File path manipulation"},
    {"regex", "Regular expressions"},
    {"time", "Time, timers, sleep"},
    {"toml", "TOML configuration file parsing"},
    {"url", "URL parsing and construction"},
    {"ws", "WebSocket client and server"},
    {"xml", "XML parsing"},
    {"yaml", "YAML parsing"},
    {NULL, NULL}};

/* ======================================================================
 * Resource content (compiled-in)
 * ====================================================================== */

static const char CHEATSHEET[] =
    "# Xray Language Cheatsheet\n"
    "\n"
    "## Basics\n"
    "```xray\n"
    "let x = 1; const PI = 3.14; let name: string = \"hello\"\n"
    "fn add(a: int, b: int) -> int { return a + b }\n"
    "let double = (x: int) -> x * 2\n"
    "```\n"
    "\n"
    "## Types\n"
    "int, float, string, bool, void | Array<T>, Map<K,V>, Set<T>\n"
    "int?, string? (nullable) | int | string (union) | Json, Bytes, BigInt, Channel<T>\n"
    "\n"
    "## Control flow\n"
    "```xray\n"
    "if (cond) {} else {}\n"
    "for (i in 0..10) {} | for (item in arr) {}\n"
    "match (x) { 1 -> \"one\", _ -> \"other\" }\n"
    "```\n"
    "\n"
    "## OOP\n"
    "```xray\n"
    "class Dog extends Animal { constructor(n: string) { super(n) } }\n"
    "struct Point { x: float; y: float }\n"
    "interface Shape { area() -> float }\n"
    "enum Color { Red, Green, Blue }\n"
    "```\n"
    "\n"
    "## Collections\n"
    "```xray\n"
    "[1,2,3]  arr.map(fn).filter(fn)  arr[1:3]\n"
    "#{\"key\": val}  m.get(k)  m.set(k,v)\n"
    "#[1,2,3]  s.add(v)  s.has(v)\n"
    "```\n"
    "\n"
    "## Concurrency (core differentiator)\n"
    "```xray\n"
    "let t = go someFunc(args)       // spawn coroutine\n"
    "let result = await t             // wait for result\n"
    "shared const ch = new Channel<int>(10)\n"
    "ch.send(val); ch.recv()          // send/receive\n"
    "select { msg from ch -> handle(msg); after 1000 -> timeout(); _ -> idle() }\n"
    "shared const CFG = {...}         // immutable cross-coroutine sharing\n"
    "```\n"
    "\n"
    "## Safety: compile pass = concurrency safe\n"
    "- shared const: zero-copy read across coroutines\n"
    "- Channel: deep copy on send\n"
    "- Function params to go: deep copy\n"
    "- Regular let/const: cannot be captured by go closures\n"
    "\n"
    "## Modules\n"
    "```xray\n"
    "import http; import json; import time\n"
    "export fn helper() {}\n"
    "```\n"
    "\n"
    "## Testing\n"
    "```xray\n"
    "@test fn test_add() { assert_eq(1+1, 2) }\n"
    "```\n"
    "\n"
    "## Important notes\n"
    "- `print()` outputs with newline (no `println`)\n"
    "- Arrow function params MUST have type annotations\n"
    "- new Channel<T>(n) at construction, Channel<T> in type position\n"
    "- `this.x++` not allowed, use `this.x = this.x + 1`\n"
    "- No quotes inside `${}` interpolation — use a variable\n";

static const char CONCURRENCY_MODEL[] =
    "# Xray Concurrency Model\n"
    "\n"
    "## Golden Rule: If it compiles, it's concurrency-safe.\n"
    "\n"
    "## Three ways to share data across coroutines:\n"
    "\n"
    "### 1. Channel (communication)\n"
    "```xray\n"
    "shared const ch = new Channel<int>(10)\n"
    "ch.send(value)           // deep copies pointer types\n"
    "let val = ch.recv()      // null when closed\n"
    "```\n"
    "\n"
    "### 2. shared const (immutable sharing)\n"
    "```xray\n"
    "shared const CONFIG = { host: \"localhost\", port: 8080 }\n"
    "// Zero-copy reads from any coroutine. Cannot modify.\n"
    "```\n"
    "\n"
    "### 3. Function parameters (deep copy)\n"
    "```xray\n"
    "go processData(myArray)  // myArray is deep-copied\n"
    "```\n"
    "\n"
    "## What you CANNOT do (compile errors):\n"
    "```xray\n"
    "let x = [1,2,3]\n"
    "go fn() { print(x) }()  // COMPILE ERROR: cannot capture 'x'\n"
    "// Fix: pass as parameter\n"
    "```\n"
    "\n"
    "## Move semantics:\n"
    "```xray\n"
    "shared let data = [1,2,3]\n"
    "go fn() { let local = move data }()\n"
    "print(data)  // COMPILE ERROR: 'data' already moved\n"
    "```\n"
    "\n"
    "## Structured concurrency:\n"
    "```xray\n"
    "scope {\n"
    "    go taskA()\n"
    "    go taskB()\n"
    "}  // waits for all goroutines to complete\n"
    "```\n";

static const char STDLIB_LIST[] = "# Xray Standard Library Modules\n"
                                  "\n"
                                  "| Module | Description |\n"
                                  "|--------|-------------|\n"
                                  "| `base64` | Base64 encoding/decoding |\n"
                                  "| `cluster` | Distributed cluster communication |\n"
                                  "| `compress` | Compression/decompression (zlib) |\n"
                                  "| `crypto` | Cryptographic hashing and encryption |\n"
                                  "| `csv` | CSV parsing and generation |\n"
                                  "| `datetime` | Date and time manipulation |\n"
                                  "| `encoding` | Character encoding conversion |\n"
                                  "| `gc` | Garbage collector control interface |\n"
                                  "| `http` | HTTP client/server, REST API, routing |\n"
                                  "| `io` | File I/O operations |\n"
                                  "| `json` | JSON parse, stringify, keys, values |\n"
                                  "| `log` | Structured logging system |\n"
                                  "| `math` | Mathematical functions |\n"
                                  "| `net` | TCP/UDP/TLS networking |\n"
                                  "| `os` | Operating system interface |\n"
                                  "| `path` | File path manipulation |\n"
                                  "| `regex` | Regular expressions |\n"
                                  "| `time` | Time, timers, sleep |\n"
                                  "| `toml` | TOML configuration parsing |\n"
                                  "| `url` | URL parsing and construction |\n"
                                  "| `ws` | WebSocket client and server |\n"
                                  "| `xml` | XML parsing |\n"
                                  "| `yaml` | YAML parsing |\n"
                                  "\n"
                                  "Usage: `import <module>` then call `module.function()`\n";

/* ======================================================================
 * Implementation
 * ====================================================================== */

/* Case-insensitive substring search. */
static bool icontains(const char *haystack, const char *needle) {
    if (!haystack || !needle)
        return false;
    if (needle[0] == '\0')
        return true;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen)
        return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (strncasecmp(haystack + i, needle, nlen) == 0)
            return true;
    }
    return false;
}

XmcpKnowledge *xmcp_knowledge_new(void) {
    XmcpKnowledge *kb = xr_calloc(1, sizeof(XmcpKnowledge));
    if (!kb)
        return NULL;
    return kb;
}

void xmcp_knowledge_load(XmcpKnowledge *kb) {
    XR_DCHECK(kb != NULL, "xmcp_knowledge_load: NULL kb");

    /* Load topics */
    for (int i = 0; BUILTIN_TOPICS[i].name != NULL; i++) {
        XR_DCHECK(kb->topic_count < XMCP_MAX_TOPICS, "xmcp_knowledge_load: too many topics");
        if (kb->topic_count >= XMCP_MAX_TOPICS)
            break;
        kb->topics[kb->topic_count].name = BUILTIN_TOPICS[i].name;
        kb->topics[kb->topic_count].aliases = BUILTIN_TOPICS[i].aliases;
        kb->topics[kb->topic_count].content = BUILTIN_TOPICS[i].content;
        kb->topic_count++;
    }

    /* Load stdlib modules */
    for (int i = 0; BUILTIN_MODULES[i].name != NULL; i++) {
        XR_DCHECK(kb->module_count < XMCP_MAX_MODULES, "xmcp_knowledge_load: too many modules");
        if (kb->module_count >= XMCP_MAX_MODULES)
            break;
        kb->modules[kb->module_count] = BUILTIN_MODULES[i];
        kb->module_count++;
    }
}

void xmcp_knowledge_free(XmcpKnowledge *kb) {
    /* All strings are static, just free the struct */
    xr_free(kb);
}

const char *xmcp_knowledge_lookup_topic(XmcpKnowledge *kb, const char *query) {
    XR_DCHECK(kb != NULL, "xmcp_knowledge_lookup_topic: NULL kb");
    if (!query)
        return NULL;

    /* Exact name match first */
    for (int i = 0; i < kb->topic_count; i++) {
        if (strcasecmp(kb->topics[i].name, query) == 0) {
            return kb->topics[i].content;
        }
    }

    /* Alias match */
    for (int i = 0; i < kb->topic_count; i++) {
        if (icontains(kb->topics[i].aliases, query)) {
            return kb->topics[i].content;
        }
    }

    /* Fuzzy: partial name match */
    for (int i = 0; i < kb->topic_count; i++) {
        if (icontains(kb->topics[i].name, query)) {
            return kb->topics[i].content;
        }
    }

    return NULL;
}

char *xmcp_knowledge_search_stdlib(XmcpKnowledge *kb, const char *query, const char *module_filter,
                                   int *match_count) {
    XR_DCHECK(kb != NULL, "xmcp_knowledge_search_stdlib: NULL kb");
    if (!query)
        return NULL;
    if (match_count)
        *match_count = 0;

    /* Build result string */
    size_t cap = 4096;
    char *result = xr_malloc(cap);
    if (!result)
        return NULL;
    size_t len = 0;

    len +=
        (size_t) snprintf(result + len, cap - len, "# Standard Library Search: \"%s\"\n\n", query);

    int found = 0;
    for (int i = 0; i < kb->module_count; i++) {
        const XmcpModule *m = &kb->modules[i];

        /* Apply module filter if provided */
        if (module_filter && module_filter[0]) {
            if (strcasecmp(m->name, module_filter) != 0)
                continue;
        }

        /* Match query against module name or description */
        if (icontains(m->name, query) || icontains(m->description, query)) {
            len += (size_t) snprintf(result + len, cap - len,
                                     "## Module: %s\n\nImport: `import %s`\n\n%s\n\n", m->name,
                                     m->name, m->description);
            found++;
        }
    }
    if (match_count)
        *match_count = found;

    if (found == 0) {
        len += (size_t) snprintf(result + len, cap - len,
                                 "No modules found matching \"%s\".\n\n"
                                 "Available modules: ",
                                 query);
        for (int i = 0; i < kb->module_count; i++) {
            if (i > 0)
                len += (size_t) snprintf(result + len, cap - len, ", ");
            len += (size_t) snprintf(result + len, cap - len, "%s", kb->modules[i].name);
        }
        len += (size_t) snprintf(result + len, cap - len, "\n");
    }

    return result;
}

const char *xmcp_knowledge_get_cheatsheet(void) {
    return CHEATSHEET;
}
const char *xmcp_knowledge_get_concurrency(void) {
    return CONCURRENCY_MODEL;
}
const char *xmcp_knowledge_get_stdlib_list(void) {
    return STDLIB_LIST;
}
