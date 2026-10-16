/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_map_membership_input.h - Definition-site equality and ordinary Map queries
 */
#ifndef P09_MAP_MEMBERSHIP_INPUT_H
#define P09_MAP_MEMBERSHIP_INPUT_H
#include "shared/xnative_declaration.h"
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
#include "xir/xxir_map.h"
static const char membership_source[] =
    "fn find<K:Hashable,V:Equal>(m:Map<K,V>,value:V)->bool{return m.containsValue(value)}\n"
    "fn pass<V>(m:Map<i64,V>)->Map<i64,V>{return m}\n"
    "fn evaluation()->i64{\n"
    "var trace=0\n"
    "var m=#{1:1}\n"
    "fn receiver()->Map<i64,i64>{trace=trace*10+1\n"
    "return m}\n"
    "fn value()->i64{trace=trace*10+2\n"
    "m.set(1,9)\n"
    "return 1}\n"
    "if(!receiver().containsValue(value())){return -8}\n"
    "if(trace!=12){return -8}\n"
    "m=#{1:1}\n"
    "if(!m.containsValue(value())){return -9}\n"
    "if(!m.containsValue(9)){return -9}\n"
    "return 0}\n"
    "export fn entry()->i64{\n"
    "var z=0.0\n"
    "const nan=z/z\n"
    "const m:Map<i64,f64>=#{1:-0.0,2:nan}\n"
    "if(!m.containsValue(0)){return -1}\n"
    "if(m.containsValue(nan)){return -1}\n"
    "const k:Map<f64,f64>=#{nan:nan}\n"
    "if(!k.containsKey(nan)){return -2}\n"
    "if(k.containsValue(nan)){return -2}\n"
    "const arrays:Map<i64,Array<f64>>=#{1:[nan],2:[1.0]}\n"
    "if(arrays.containsValue(arrays[1])){return -3}\n"
    "if(!arrays.containsValue([1.0])){return -3}\n"
    "const n:f64?=null\n"
    "const optional:Map<i64,f64?>=#{1:n}\n"
    "if(!optional.containsValue(n)){return -4}\n"
    "const empty=Map<i64,i64>()\n"
    "if(empty.containsValue(7)){return -5}\n"
    "const strings=#{1:\"a\\0b\"}\n"
    "if(!strings.containsValue(\"a\\0b\")){return -6}\n"
    "if(strings.containsValue(\"a\\0c\")){return -6}\n"
    "const evaluated=evaluation()\n"
    "if(evaluated!=0){return evaluated}\n"
    "return 123}\n"
    "export fn late_first()->Map<i64,Array<f64>>{var m:Map<i64,Array<f64>>=#{1:[1.0,2.0]}\n"
    "const result=pass(m)\n"
    "m.clear()\n"
    "return result}\n"
    "export fn late_second()->Map<i64,Array<f64>>{var z=0.0\n"
    "return #{2:[z/z]}}\n"
    "export fn generic()->Map<i64,Array<f64>>{const m:Map<i64,Array<f64>>=#{3:[3.0]}\n"
    "if(!find(m,[3.0])){return Map<i64,Array<f64>>()}\n"
    "return m}\n"
    "export fn nested()->Map<i64,Array<f64>>{var maps:Array<Map<i64,Array<f64>>>=[#{4:[4.0]}]\n"
    "if(!maps[0].containsValue([4.0])){return Map<i64,Array<f64>>()}\n"
    "const result=maps[0]\n"
    "maps[0].clear()\n"
    "return result}\n";
static void membership_entries(const XrXirModule *module,uint32_t *entries) {
    const char *names[]={"entry","late_first","late_second","generic","nested"};
    for(uint32_t n=0;n<5;++n) {
        entries[n]=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f)
            if(module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n]))) {
                CHECK(entries[n]==UINT32_MAX);entries[n]=f;
            }
        CHECK(entries[n]!=UINT32_MAX);
    }
}
static void membership_query(const XrXirSourceView *view) {
    CHECK(view && view->complete);uint32_t methods=0;
    for(uint32_t i=0;i<view->declaration_count;++i) {
        const XrXirSourceDeclaration *d=&view->declarations[i];
        if(d->kind!=XR_XIR_SOURCE_MEMBER || strcmp(d->name,"containsValue"))continue;
        ++methods;CHECK(d->parent && view->declarations[d->parent-1].native_identity==XR_NATIVE_DECLARATION_MAP);
        CHECK(d->type.known && d->type.type==XR_XIR_BOOL && d->parameter_count==1 && d->parameters[0].known);
        CHECK(d->parameters[0].type==XR_XIR_TYPE_PARAMETER_BASE+1 && d->parameters[0].generic_owner==d->parent);
    }
    CHECK(methods==1);
}
#endif // P09_MAP_MEMBERSHIP_INPUT_H
