/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbuiltin_method_defs.h - Single source of truth for built-in type methods
 *
 * KEY CONCEPT:
 *   All built-in type member declarations are defined here using X-macros.
 *   The Analyzer (for LSP/type-checking) consumes this file to generate
 *   XaBuiltinMember arrays. VM implementation lives in the per-type
 *   method tables under runtime/value/x{bool,int,float}_methods.{c,h},
 *   runtime/object/x{array,string,map,set,bigint,json}_methods.{c,h},
 *   and stdlib/datetime/datetime_methods.{c,h}, all registered in
 *   runtime/value/xmethod_table.c.
 *
 * USAGE:
 *   #define M(name, sig, doc, is_method) {name, sig, doc, is_method, false},
 *   static const XaBuiltinMember int_members[] = {
 *       XR_INT_METHODS(M)
 *   };
 *   #undef M
 *
 * SYNC RULE:
 *   - Adding a method here => MUST implement in the matching per-type
 *     method table (or native_type_classes for class-shaped builtins)
 *   - Adding a method in a per-type table => MUST declare here
 *
 * RELATED MODULES:
 *   - src/runtime/value/xmethod_table.{c,h}: unified per-type method registry
 *   - src/analyzer/xanalyzer_builtins.c: consumes these definitions
 *   - src/object/xstringbuilder_builtins.c: StringBuilder VM implementation
 */

#ifndef XBUILTIN_METHOD_DEFS_H
#define XBUILTIN_METHOD_DEFS_H

/*
 * M(name, signature, doc, is_method)
 *   name       - method/property name string
 *   signature  - type signature (": type" for props, "(params): ret" for methods)
 *   doc        - short documentation string
 *   is_method  - true for methods, false for properties
 */

/* ========== Array ========== */
#define XR_ARRAY_MEMBERS(M)                                                                        \
    M("length", ": int", "Number of elements", false)                                              \
    M("push", "(value: T): void", "Add element to end", true)                                      \
    M("pop", "(): T?", "Remove and return last element", true)                                     \
    M("shift", "(): T?", "Remove and return first element", true)                                  \
    M("unshift", "(value: T): void", "Add element to beginning", true)                             \
    M("slice", "(start?: int, end?: int): Array<T>", "Return a portion of the array", true)        \
    M("splice", "(start: int, deleteCount?: int, ...items: T): Array<T>", "Change array contents", \
      true)                                                                                        \
    M("concat", "(...arrays: Array<T>): Array<T>", "Merge arrays", true)                           \
    M("indexOf", "(value: T): int", "Find index of element (-1 if not found)", true)               \
    M("includes", "(value: T): bool", "Check if array contains element", true)                     \
    M("join", "(separator?: string): string", "Join elements into string", true)                   \
    M("reverse", "(): Array<T>", "Reverse array in place", true)                                   \
    M("sort", "(compareFn?: fn(a: T, b: T): int): Array<T>", "Sort array in place", true)          \
    M("map", "(fn: fn(item: T, index: int): U): Array<U>", "Transform each element", true)         \
    M("filter", "(fn: fn(item: T, index: int): bool): Array<T>", "Filter elements", true)          \
    M("reduce", "(fn: fn(acc: U, item: T): U, initial: U): U", "Reduce to single value", true)     \
    M("forEach", "(fn: fn(item: T, index: int): void): void", "Execute for each element", true)    \
    M("find", "(fn: fn(item: T): bool): T?", "Find first matching element", true)                  \
    M("findIndex", "(fn: fn(item: T): bool): int", "Find index of first match", true)              \
    M("every", "(fn: fn(item: T): bool): bool", "Test if all elements pass", true)                 \
    M("some", "(fn: fn(item: T): bool): bool", "Test if any element passes", true)                 \
    M("flat", "(depth?: int): Array<T>", "Flatten nested arrays", true)                            \
    M("fill", "(value: T, start?: int, end?: int): Array<T>", "Fill with value", true)             \
    M("copyWithin", "(target: int, start: int, end?: int): Array<T>", "Copy within array", true)

/* ========== string ========== */
#define XR_STRING_MEMBERS(M)                                                                       \
    M("length", ": int", "Number of characters", false)                                            \
    M("charAt", "(index: int): string", "Get character at index", true)                            \
    M("charCodeAt", "(index: int): int", "Get character code at index", true)                      \
    M("concat", "(...strings: string): string", "Concatenate strings", true)                       \
    M("includes", "(search: string): bool", "Check if contains substring", true)                   \
    M("indexOf", "(search: string, start?: int): int", "Find substring index", true)               \
    M("lastIndexOf", "(search: string): int", "Find last substring index", true)                   \
    M("slice", "(start: int, end?: int): string", "Extract portion", true)                         \
    M("substring", "(start: int, end?: int): string", "Extract between indices", true)             \
    M("substr", "(start: int, length?: int): string", "Extract from start for length", true)       \
    M("toLowerCase", "(): string", "Convert to lowercase", true)                                   \
    M("toUpperCase", "(): string", "Convert to uppercase", true)                                   \
    M("trim", "(): string", "Remove whitespace from both ends", true)                              \
    M("trimStart", "(): string", "Remove leading whitespace", true)                                \
    M("trimEnd", "(): string", "Remove trailing whitespace", true)                                 \
    M("split", "(separator: string, limit?: int): Array<string>", "Split into array", true)        \
    M("replace", "(search: string, replacement: string): string", "Replace first occurrence",      \
      true)                                                                                        \
    M("replaceAll", "(search: string, replacement: string): string", "Replace all occurrences",    \
      true)                                                                                        \
    M("repeat", "(count: int): string", "Repeat string", true)                                     \
    M("startsWith", "(search: string): bool", "Check if starts with", true)                        \
    M("endsWith", "(search: string): bool", "Check if ends with", true)                            \
    M("padStart", "(length: int, pad?: string): string", "Pad start to length", true)              \
    M("padEnd", "(length: int, pad?: string): string", "Pad end to length", true)                  \
    M("match", "(pattern: string): Array<string>?", "Match against pattern", true)

/* ========== Map ========== */
#define XR_MAP_MEMBERS(M)                                                                          \
    M("length", ": int", "Number of key-value pairs", false)                                       \
    M("get", "(key: K): V?", "Get value by key", true)                                             \
    M("set", "(key: K, value: V): void", "Set key-value pair", true)                               \
    M("has", "(key: K): bool", "Check if key exists", true)                                        \
    M("delete", "(key: K): bool", "Delete key-value pair", true)                                   \
    M("clear", "(): void", "Remove all entries", true)                                             \
    M("keys", "(): Array<K>", "Get all keys", true)                                                \
    M("values", "(): Array<V>", "Get all values", true)                                            \
    M("entries", "(): Array<[K, V]>", "Get all entries", true)                                     \
    M("forEach", "(fn: fn(value: V, key: K): void): void", "Execute for each entry", true)

/* ========== Set ========== */
#define XR_SET_MEMBERS(M)                                                                          \
    M("length", ": int", "Number of elements", false)                                              \
    M("add", "(value: T): void", "Add element", true)                                              \
    M("has", "(value: T): bool", "Check if element exists", true)                                  \
    M("delete", "(value: T): bool", "Remove element", true)                                        \
    M("clear", "(): void", "Remove all elements", true)                                            \
    M("values", "(): Array<T>", "Get all values", true)                                            \
    M("forEach", "(fn: fn(value: T): void): void", "Execute for each element", true)

/* ========== Channel ========== */
#define XR_CHANNEL_MEMBERS(M)                                                                      \
    M("send", "(value: T): void", "Send value (blocks if full)", true)                             \
    M("recv", "(): T", "Receive value (blocks if empty)", true)                                    \
    M("trySend", "(value: T): bool", "Try to send (non-blocking)", true)                           \
    M("tryRecv", "(): (T, bool)", "Try to receive (non-blocking)", true)                           \
    M("close", "(): void", "Close channel", true)                                                  \
    M("closed", ": bool", "Check if channel is closed", false)

/* ========== EnumValue ========== */
#define XR_ENUM_VALUE_MEMBERS(M)                                                                   \
    M("name", ": string", "Member name", false)                                                    \
    M("value", ": Json", "Raw value", false)                                                       \
    M("ordinal", ": int", "Member index", false)                                                   \
    M("toString", "(): string", "String representation (EnumName.MemberName)", true)

/* ========== EnumType ========== */
#define XR_ENUM_TYPE_MEMBERS(M)                                                                    \
    M("memberCount", ": int", "Number of enum members", false)                                     \
    M("getMember", "(index: int): EnumValue", "Get member by index", true)

/* ========== int ========== */
#define XR_INT_MEMBERS(M)                                                                          \
    M("abs", "(): int", "Absolute value", true)                                                    \
    M("toString", "(): string", "Convert to string", true)                                         \
    M("toBigInt", "(): BigInt", "Convert to BigInt", true)                                         \
    M("toFloat", "(): float", "Convert to float", true)                                            \
    M("toHex", "(): string", "Convert to hexadecimal string", true)                                \
    M("max", "(other: int): int", "Return the larger value", true)                                 \
    M("min", "(other: int): int", "Return the smaller value", true)                                \
    M("floor", "(): int", "Round down (identity for int)", true)                                   \
    M("ceil", "(): int", "Round up (identity for int)", true)                                      \
    M("round", "(): int", "Round (identity for int)", true)                                        \
    M("sqrt", "(): float", "Square root", true)                                                    \
    M("pow", "(exp: float): float", "Power", true)

/* ========== float ========== */
#define XR_FLOAT_MEMBERS(M)                                                                        \
    M("abs", "(): float", "Absolute value", true)                                                  \
    M("toString", "(): string", "Convert to string", true)                                         \
    M("toFixed", "(decimals?: int): string", "Format with fixed decimal places", true)             \
    M("toInt", "(): int", "Truncate to integer", true)                                             \
    M("floor", "(): int", "Round down", true)                                                      \
    M("ceil", "(): int", "Round up", true)                                                         \
    M("round", "(): int", "Round to nearest integer", true)                                        \
    M("sqrt", "(): float", "Square root", true)                                                    \
    M("pow", "(exp: float): float", "Power", true)

/* ========== bool ========== */
#define XR_BOOL_MEMBERS(M) M("toString", "(): string", "Convert to string", true)

/* ========== BigInt ========== */
#define XR_BIGINT_MEMBERS(M)                                                                       \
    M("abs", "(): BigInt", "Absolute value", true)                                                 \
    M("toString", "(): string", "Convert to string", true)                                         \
    M("sign", "(): int", "Sign value (-1, 0, 1)", true)                                            \
    M("isZero", "(): bool", "Check if zero", true)                                                 \
    M("isNegative", "(): bool", "Check if negative", true)                                         \
    M("isPositive", "(): bool", "Check if positive", true)                                         \
    M("toInt", "(): int?", "Convert to int (null on overflow)", true)                              \
    M("toFloat", "(): float", "Convert to float", true)

/* ========== Json ========== */
#define XR_JSON_MEMBERS(M)                                                                         \
    M("keys", "(): Array<string>", "Get all field names", true)                                    \
    M("values", "(): Array<Json>", "Get all values", true)                                         \
    M("entries", "(): Array<[string, Json]>", "Get all key-value pairs", true)                     \
    M("has", "(key: string): bool", "Check if field exists", true)                                 \
    M("get", "(key: string): Json", "Get value by key", true)                                      \
    M("isEmpty", "(): bool", "Check if empty", true)                                               \
    M("delete", "(key: string): void", "Delete a field", true)                                     \
    M("clear", "(): void", "Remove all fields", true)                                              \
    M("toString", "(): string", "Convert to string", true)

/* ========== Regex ========== */
#define XR_REGEX_MEMBERS(M)                                                                        \
    M("test", "(text: string): bool", "Test if pattern matches", true)                             \
    M("find", "(text: string): string?", "Find first match", true)                                 \
    M("findAll", "(text: string): Array<string>", "Find all matches", true)                        \
    M("replace", "(text: string, replacement: string): string", "Replace matches", true)           \
    M("split", "(text: string): Array<string>", "Split by pattern", true)

/* ========== Exception ========== */
#define XR_EXCEPTION_MEMBERS(M)                                                                    \
    M("message", ": string", "Error message", false)                                               \
    M("stack", ": string", "Stack trace", false)                                                   \
    M("toString", "(): string", "String representation", true)

/* ========== Coroutine (Task) ========== */
#define XR_COROUTINE_MEMBERS(M)                                                                    \
    M("done", ": bool", "Check if task completed", false)                                          \
    M("cancelled", ": bool", "Check if task was cancelled", false)                                 \
    M("result", ": Json", "Task return value (after await)", false)                                \
    M("error", ": string?", "Task error message", false)                                           \
    M("cancel", "(): void", "Cancel the task", true)

/* ========== StringBuilder ========== */
#define XR_STRINGBUILDER_MEMBERS(M)                                                                \
    M("length", ": int", "Current buffer length", false)                                           \
    M("append", "(value: Json): StringBuilder", "Append value (supports chaining)", true)          \
    M("toString", "(): string", "Build final string", true)                                        \
    M("clear", "(): StringBuilder", "Clear buffer (supports chaining)", true)

#endif  // XBUILTIN_METHOD_DEFS_H
