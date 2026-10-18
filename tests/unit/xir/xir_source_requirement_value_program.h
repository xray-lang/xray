/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_requirement_value_program.h - Shared exact callable capacity input
 */
#ifndef XIR_SOURCE_REQUIREMENT_VALUE_PROGRAM_H
#define XIR_SOURCE_REQUIREMENT_VALUE_PROGRAM_H
static const char requirement_value_mixed_source[] =
    "interface I{map<U:Sendable>(value:U)->U}\n"
    "struct S implements I{value:i64=0;map<V:Sendable>(value:V)->V{return value}}\n"
    "fn mixed<T:I>(receiver:T,factory:fn(T)->fn(i64)->i64=fn(value:T)->fn(i64)->i64{return value.map<i64>})->i64{"
    "const initial=factory(receiver);"
    "const run=fn(value:i64)->i64{const saved=receiver.map<i64>;return saved(value)};"
    "defer {const saved=receiver.map<i64>;};return initial(run(41))}\n"
    "const initialized=mixed<S>(S())\n"
    "export fn genericMethodNumber()->i64{return initialized}\n"
    "export fn genericMethodText()->string{return \"mapped\"}\n";
#endif // XIR_SOURCE_REQUIREMENT_VALUE_PROGRAM_H
