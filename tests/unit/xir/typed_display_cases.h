/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * typed_display_cases.h - Independent builtin text and snapshot expectations
 *
 * KEY CONCEPT:
 *   Strings produced from a value remain independent of later COW writes.
 */
#ifndef TYPED_DISPLAY_CASES_H
#define TYPED_DISPLAY_CASES_H
static const char display_source[]=
    "export fn render()->string {\n"
    "const n:Array<i64>?=null\n"
    "const p:Array<i64>?=[1,2]\n"
    "const record:{a:i64,z:string}={z:\"hi\",a:3}\n"
    "const em:Map<i64,i64>=#{}\n"
    "const es:Set<i64>=#[]\n"
    "return \"${(1,\"hi\",true)}|${(7,)}|${[(1,\"x\"),(2,\"y\")]}|${#{\"k\":[1,2]}}|${#[3,2,3]}|${n}|${p}|${record}|${em}|${es}\"\n"
    "}\n"
    "export fn cow()->i64 {\n"
    "var m=#{1:10,2:20}\n"
    "const old=m\n"
    "const s=\"${m}\"\n"
    "m.clear()\n"
    "m.set(9,90)\n"
    "if(s!=\"#{1: 10, 2: 20}\" || \"${old}\"!=s || \"${m}\"!=\"#{9: 90}\"){return -1}\n"
    "var a=[1,2]\n"
    "const before=\"${a}\"\n"
    "a[0]=9\n"
    "if(before!=\"[1, 2]\" || \"${a}\"!=\"[9, 2]\"){return -2}\n"
    "var calls=0\n"
    "fn next()->Array<i64>{calls+=1\nreturn [calls]}\n"
    "const order=\"${next()}|${next()}\"\n"
    "if(calls!=2 || order!=\"[1]|[2]\"){return -3}\n"
    "return 42\n"
    "}\n";
static const char display_expected[]=
    "(1, \"hi\", true)|(7,)|[(1, \"x\"), (2, \"y\")]|#{\"k\": [1, 2]}|#[3, 2]|null|[1, 2]|{a: 3, z: \"hi\"}|#{}|#[]";
#endif // TYPED_DISPLAY_CASES_H
