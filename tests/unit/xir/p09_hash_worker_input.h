/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_worker_input.h - Owned nested Map and Set cross ordinary worker boundaries
 */
#ifndef P09_HASH_WORKER_INPUT_H
#define P09_HASH_WORKER_INPUT_H
#include "xir/xxir.h"
static const char worker_source[]=
    "fn pass<K:Hashable & Sendable,V:Sendable>(input:Map<K,V>)->Map<K,V>{return input}\n"
    "fn mapWork(input:Map<i64,Array<i64>>,tags:Set<i64>)->Map<i64,Array<i64>>{\n"
    "var output=input\n"
    "var row=output[1]\n"
    "row[0]=7\n"
    "row.push(8)\n"
    "output.set(1,row)\n"
    "output.set(2,[len(tags)])\n"
    "return output}\n"
    "fn setWork(input:Set<i64>)->Set<i64>{var output=input\n"
    "output.delete(3)\n"
    "output.add(9)\n"
    "return output}\n"
    "export fn entry()->i64{\n"
    "var m=#{1:[1]}\n"
    "var tags=#[3,4]\n"
    "const task=go mapWork(m,tags)\n"
    "const setTask=go setWork(tags)\n"
    "m[1][0]=99\n"
    "tags.clear()\n"
    "const result=await task\n"
    "const resultSet=await setTask\n"
    "if(result[1][0]!=7){return -1}\n"
    "if(result[1][1]!=8){return -1}\n"
    "if(result[2][0]!=2){return -1}\n"
    "if(m[1][0]!=99){return -2}\n"
    "if(m.containsKey(2)){return -2}\n"
    "if(len(tags)!=0){return -2}\n"
    "if(resultSet.contains(3)){return -3}\n"
    "if(!resultSet.contains(4)){return -3}\n"
    "if(!resultSet.contains(9)){return -3}\n"
    "const copied=await go pass(result)\n"
    "if(copied[1][0]!=7){return -4}\n"
    "return 123}\n"
    "export fn late_map()->Task<Map<i64,Array<i64>>>{\n"
    "const task=go mapWork(#{1:[1]},#[3,4])\n"
    "const result=await task\n"
    "return task}\n"
    "export fn late_set()->Task<Set<i64>>{\n"
    "const task=go setWork(#[3,4])\n"
    "const result=await task\n"
    "return task}\n";
static void worker_entries(const XrXirModule *module,uint32_t *entries,uint32_t *worker) {
    const char *names[]={"entry","late_map","late_set","mapWork"};
    for(unsigned name=0;name<4;++name) {
        uint32_t found=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f)
            if(module->functions[f].name_length==strlen(names[name]) && !memcmp(module->functions[f].name,names[name],strlen(names[name]))) {
                CHECK(found==UINT32_MAX);found=f;
            }
        CHECK(found<module->function_count);if(name<3)entries[name]=found;else *worker=found;
    }
}
#endif // P09_HASH_WORKER_INPUT_H
