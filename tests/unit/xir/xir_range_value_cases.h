/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_range_value_cases.h - Ordinary range construction and visibility cases
 */
#ifndef XIR_RANGE_VALUE_CASES_H
#define XIR_RANGE_VALUE_CASES_H
static const char range_library_source[]=
    "export class Box {const value:Range init(value:Range){this.value=value}}\n"
    "export fn echo(r:Range)->Range{return r}\n"
    "export fn through(r:Range)->Range{const box=Box(echo(r));return box.value}\n"
    "export fn publicRange(a:i64,b:i64,closed:bool)->Range{return Range(a,b,closed)}\n"
    "export fn indexAt(r:Range,i:i64)->i64{return r[i]}\n";
static const char range_consumer_source[]=
    "import {through,publicRange,indexAt} from \"./range-lib\"\n"
    "var endpointCalls:i64=0\n"
    "fn left()->i64{endpointCalls=endpointCalls*10+1;return endpointCalls}\n"
    "fn right()->i64{endpointCalls=endpointCalls*10+2;return endpointCalls}\n"
    "export fn literal(a:i64,b:i64)->Range{return through(a..b)}\n"
    "export fn inclusive(a:i64,b:i64)->Range{return through(a..=b)}\n"
    "export fn constructed(a:i64,b:i64,closed:bool)->Range{return through(publicRange(a,b,closed))}\n"
    "export fn rangeCount(a:i64,b:i64,closed:bool)->i64{return publicRange(a,b,closed).count()}\n"
    "export fn endpoints(a:i64,b:i64,closed:bool)->bool{const r=publicRange(a,b,closed);return r.start==a && r.end==b}\n"
    "export fn once()->i64{endpointCalls=0;const r=left()..=right();return r.start*10000+r.end*100+endpointCalls}\n"
    "export fn accept(r:Range)->i64{return r.count()}\n"
    "export fn rangeLength(r:Range)->i64{return len(r)}\n"
    "fn factory(closed:bool)->Range{endpointCalls+=1;return publicRange(-2,3,closed)}\n"
    "export fn lenOnce(closed:bool)->i64{endpointCalls=0;const n=len(factory(closed));return n*100+endpointCalls}\n"
    "export fn indexed(a:i64,b:i64,closed:bool,i:i64)->i64{return indexAt(publicRange(a,b,closed),i)}\n"
    "fn indexedReceiver()->Range{endpointCalls=endpointCalls*10+1;return 7..10}\n"
    "fn indexedKey()->i64{endpointCalls=endpointCalls*10+2;return 1}\n"
    "export fn indexedOnce()->i64{endpointCalls=0;const n=indexedReceiver()[indexedKey()];return n*100+endpointCalls}\n"
    "var indexedRange:Range=7..10\n"
    "fn rebindKey()->i64{indexedRange=100..103;return 1}\n"
    "export fn indexedRebind()->i64{indexedRange=7..10;const n=indexedRange[rebindKey()];return n*1000+indexedRange.start}\n"
    "export fn indexedCode(a:i64,b:i64,closed:bool,i:i64)->i64{try{return indexed(a,b,closed,i)}catch panic(p){return p.code}}\n";
static const char range_constructor_source[]=
    "class Holder<T>{const value:T;const extra:i64;init(value:T,extra:i64=7){this.value=value;this.extra=extra}}\n"
    "export fn generic()->i64{const a=Holder<i64>(41);const b=Holder<Range>(1..=3,9);"
    "return a.value+a.extra+b.value.count()+b.extra}\n";
static const char *const range_entry_names[]={"literal","inclusive","constructed","rangeCount","endpoints","once","accept","rangeLength","lenOnce",
    "indexed","indexedOnce","indexedRebind","indexedCode"};
static const char *const range_rejections[]={
    "export fn f()->Range{return Range(1,2)}",
    "export fn f()->Range{return Range(1,2,false,1)}",
    "export fn f()->Range{return Range(1,2,3)}",
    "export fn f()->Range{return Range(1.0,2,false)}",
    "export fn f()->Range{return \"a\"..2}",
    "export fn f()->Range{return 1..true}",
    "export fn f(r:Range)->i64{return r._step}",
    "export fn f(r:Range)->bool{return r._inclusive}",
    "export fn f(r:Range){r.start=2}",
    "export fn f(r:Range){r.end=2}",
    "export fn f(r:Range){r._step=2}",
    "class Range {} export fn f(r:Range)->Range{return 1..3}",
    "class Secret {private init(){}} export fn f()->Secret{return Secret()}",
    "class Range {count()->i64{return 1}} export fn f(r:Range)->i64{return len(r)}",
    "class Holder<T>{const value:T;init(value:T){this.value=value}} export fn f(){const c=Holder<i64>(\"bad\")}",
    "class Holder<T>{const value:T;init(value:T,n:i64=7){this.value=value}} export fn f(){const c=Holder<i64>(1,\"bad\")}",
    "export fn f(r:Range)->i64{return r[true]}",
    "export fn f(r:Range)->i64{return r[1.0]}",
    "class Range{} export fn f(r:Range)->i64{return r[0]}"
};
#endif // XIR_RANGE_VALUE_CASES_H
