/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_foreach_input.h - Ordered snapshots and ordinary suspending callbacks
 */
#ifndef P09_HASH_FOREACH_INPUT_H
#define P09_HASH_FOREACH_INPUT_H
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
#include "xir/xxir_map.h"
static const char foreach_source[] =
    "fn walkMap<K:Hashable,V>(m:Map<K,V>,visit:fn(K,V)){m.forEach(visit)}\n"
    "fn walkSet<T:Hashable>(s:Set<T>,visit:fn(T)){s.forEach(visit)}\n"
    "export fn entry()->i64{\n"
    "var sum=0\n"
    "walkSet(#[1,2,3,4,5],fn(v){Coro.yield()\n"
    "sum+=v})\n"
    "var m=#{1:2}\n"
    "walkMap(m,fn(k,v){sum+=k+v\n"
    "m.clear()})\n"
    "if(sum!=18){return -1}\n"
    "if(len(m)!=0){return -2}\n"
    "return 123}\n"
    "fn collect(m:Map<i64,Array<f64>>)->Map<i64,Array<f64>>{\n"
    "var copy=m\n"
    "var result=Map<i64,Array<f64>>()\n"
    "copy.forEach(fn(k,v){Coro.yield()\n"
    "result.set(k,v)\n"
    "copy.clear()})\n"
    "return result}\n"
    "export fn late_first()->Map<i64,Array<f64>>{var m:Map<i64,Array<f64>>=#{1:[1.0,2.0]}\n"
    "const result=collect(m)\n"
    "m.clear()\n"
    "return result}\n"
    "export fn late_second()->Map<i64,Array<f64>>{var z=0.0\n"
    "return collect(#{2:[z/z]})}\n"
    "export fn generic()->Map<i64,Array<f64>>{\n"
    "const m:Map<i64,Array<f64>>=#{3:[3.0]}\n"
    "var seen=0\n"
    "walkMap(m,fn(k,v){Coro.yield()\n"
    "seen=k+len(v)})\n"
    "if(seen!=4){return Map<i64,Array<f64>>()}\n"
    "return m}\n"
    "export fn nested()->Map<i64,Array<f64>>{\n"
    "var maps:Array<Map<i64,Array<f64>>>=[#{4:[4.0]}]\n"
    "var result=Map<i64,Array<f64>>()\n"
    "maps[0].forEach(fn(k,v){Coro.yield()\n"
    "result.set(k,v)\n"
    "maps[0].clear()})\n"
    "return result}\n";
static void foreach_entries(const XrXirModule *module,uint32_t *entries) {
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
static void foreach_query(const XrXirSourceView *view) {
    CHECK(view && view->complete);uint32_t methods=0;
    for(uint32_t i=0;i<view->declaration_count;++i) {
        const XrXirSourceDeclaration *d=&view->declarations[i];
        if(d->kind!=XR_XIR_SOURCE_MEMBER || strcmp(d->name,"forEach"))continue;
        CHECK(d->parent && d->type.known && d->type.type==XR_XIR_UNIT && d->parameter_count==1 && d->parameters[0].known);
        uint32_t identity=view->declarations[d->parent-1].native_identity;CHECK(identity==6 || identity==7);
        const XrXirTypeNode *callback=xr_xir_callable_signature(view->types,d->parameters[0].type);
        CHECK(callback && callback->parameter_count==(identity==6?2u:1u) && callback->result==XR_XIR_UNIT);
        for(uint32_t p=0;p<callback->parameter_count;++p)
            CHECK(callback->parameters[p].mode==XR_PARAM_READ && callback->parameters[p].type==(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p));
        CHECK(d->parameters[0].generic_owner==d->parent);++methods;
    }
    CHECK(methods==2);
}
#endif // P09_HASH_FOREACH_INPUT_H
