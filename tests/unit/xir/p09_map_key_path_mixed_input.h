/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_map_key_path_mixed_input.h - Ordinary call edges around unchanged container programs
 */
#ifndef P09_MAP_KEY_PATH_MIXED_INPUT_H
#define P09_MAP_KEY_PATH_MIXED_INPUT_H
#include "p09_map_key_path_input.h"
#include "p09_hash_worker_input.h"
static inline void key_mixed_source(uint32_t row,char *text,size_t capacity) {
    CHECK(row<9);
    const char *source=row==8?worker_source:map_key_sources[row].source;
    const char *suffix=row==8?
        "export fn xrayDriver()->i64{return entry()}\n"
        "export fn xrayDriverMap()->Task<Map<i64,Array<i64>>>{return late_map()}\n"
        "export fn xrayDriverSet()->Task<Set<i64>>{return late_set()}\n":
        "export fn xrayDriver()->i64{return entry()}\n";
    int length=snprintf(text,capacity,"%s\n%s",source,suffix);
    CHECK(length>0 && (size_t)length<capacity && !memcmp(text,source,strlen(source)));
}
static inline uint32_t key_mixed_named(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(name) &&
            !memcmp(module->functions[f].name,name,strlen(name))) {
            CHECK(found==UINT32_MAX);found=f;
        }
    CHECK(found<module->function_count);return found;
}
static inline void key_mixed_entries(const XrXirModule *module,uint32_t row,
    uint32_t *entries,uint32_t *targets) {
    const char *callers[]={"xrayDriver","xrayDriverMap","xrayDriverSet"};
    const char *callees[]={"entry","late_map","late_set"};
    CHECK(row<9);
    for(unsigned i=0;i<(row==8?3u:1u);++i) {
        entries[i]=key_mixed_named(module,callers[i]);targets[i]=key_mixed_named(module,callees[i]);
        CHECK(entries[i]!=targets[i]);
        const XrXirFunction *function=&module->functions[entries[i]];unsigned calls=0;
        for(uint32_t op=0;op<function->instruction_count;++op) {
            const XrXirInstruction *instruction=function->instructions+op;
            if(instruction->op==XR_XIR_CALL || instruction->op==XR_XIR_INVOKE) {
                CHECK(instruction->immediate==targets[i]);++calls;
            }
        }
        CHECK(calls==1 && !function->parameter_count &&
            function->result==module->functions[targets[i]].result);
    }
    if(row==8) {
        uint32_t original[3]={0},worker=UINT32_MAX;
        worker_entries(module,original,&worker);
        CHECK(!memcmp(original,targets,sizeof(original)) && worker<module->function_count);
    }
}
#endif // P09_MAP_KEY_PATH_MIXED_INPUT_H
