/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * optional_chain_fixture.h - Nullable fields, single evaluation and managed results
 *
 * KEY CONCEPT:
 *   Exact source assertions share one independent oracle in VM and generated C.
 */
#ifndef OPTIONAL_CHAIN_FIXTURE_H
#define OPTIONAL_CHAIN_FIXTURE_H
static const char optional_chain_source[]=
    "type InnerValue={value:i64,text:string}\n"
    "type OptionalWrapper={inner:InnerValue?}\n"
    "type DirectWrapper={inner:InnerValue}\n"
    "var ticks=0\n"
    "fn receiver(present:bool)->OptionalWrapper?{ticks=ticks+1;if(present){return {inner:{value:42,text:\"A\\0B\"}}};return null}\n"
    "enum ChainError{Bad}\n"
    "fn failed() throws(ChainError)->OptionalWrapper?{ticks=ticks+1;throw ChainError.Bad}\n"
    "export fn test()->i64{\n"
    " ticks=0\n"
    " const obj:OptionalWrapper={inner:{value:42,text:\"held\"}}\n"
    " const empty:OptionalWrapper={inner:null}\n"
    " assert(obj?.inner?.value==42)\n"
    " assert(empty?.inner?.value==null)\n"
    " assert(\"optional chain: ${obj?.inner?.value}\"==\"optional chain: 42\")\n"
    " assert(\"missing chain: ${empty?.inner?.value}\"==\"missing chain: null\")\n"
    " const a:i64?=receiver(true)?.inner?.value\n"
    " assert(a==42 && ticks==1)\n"
    " const b:i64?=receiver(false)?.inner?.value\n"
    " assert(b==null && ticks==2)\n"
    " const direct:DirectWrapper?={inner:{value:9,text:\"direct\"}}\n"
    " assert(direct?.inner.value==9)\n"
    " const absent:DirectWrapper?=null\n"
    " assert(absent?.inner.value==null)\n"
    " const nullableField:{value:i64?}?={value:7}\n"
    " const flattened:i64?=nullableField?.value\n"
    " assert(flattened==7)\n"
    " ticks=0\n"
    " try{const bad=(try failed())?.inner?.value;assert(bad==99)}catch(e:ChainError){assert(e==ChainError.Bad && ticks==1)}\n"
    " return 42\n"
    "}\n"
    "export fn escape()->string?{return receiver(true)?.inner?.text}\n";
#endif
