/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * initialization_cycle_fixture.h - Real generated contracts for product consumers
 */
#ifndef XR_TEST_INITIALIZATION_CYCLE_FIXTURE_H
#define XR_TEST_INITIALIZATION_CYCLE_FIXTURE_H
static const char initialization_pure_source[]=
    "struct Loop{value:i64=Loop().value}\n"
    "const factory=fn()->Loop{return Loop()}\n"
    "export fn answer()->i64{return 42}\n";
static const char initialization_error_source[]=
    "enum ReadErr{Bad{code:i64}}\n"
    "fn read() throws(ReadErr)->i64{throw ReadErr.Bad{code:42}}\n"
    "struct Node{first:i64=try read()\nvalue:i64=try Node().value}\n"
    "const factory=fn()->Node{return try Node()}\n"
    "export fn answer()->i64{try{const n=factory();return n.value}"
    "catch(e:ReadErr){return match(e){ReadErr.Bad{code}->code}}}\n"
    "print(answer())\n";
#endif
