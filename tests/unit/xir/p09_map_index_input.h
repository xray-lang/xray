/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_map_index_input.h - Required lookup, insertion and evaluation-order input
 */
#ifndef P09_MAP_INDEX_INPUT_H
#define P09_MAP_INDEX_INPUT_H
#include "xir/xxir_types.h"
enum { INDEX_BASIC, INDEX_GENERIC, INDEX_NESTED, INDEX_SNAPSHOT, INDEX_COMMIT, INDEX_ORDER,
    INDEX_LATE, INDEX_CAUGHT, INDEX_MESSAGE, INDEX_NULL_VALUE, INDEX_INFO, INDEX_MISSING, INDEX_COUNT };
static const char index_source[] =
    "fn required<K:Hashable,V>(m:Map<K,V>,k:K)->V{return m[k]}\n"
    "export fn basic()->i64{var m=Map<string,i64>()\nm[\"a\"]=10\nm[\"b\"]=20\n"
    "const old=m\nm[\"a\"]=11\nreturn old[\"a\"]*100+m[\"a\"]*10+m[\"b\"]}\n"
    "export fn generic()->i64{return required(#{1:9},1)}\n"
    "export fn nested()->i64{var maps=[#{1:2}]\nconst saved=maps\nmaps[0][1]=3\n"
    "return saved[0][1]*100+maps[0][1]}\n"
    "export fn snapshot()->i64{var m=#{1:10}\nfn key()->i64{m=#{1:20}\nreturn 1}\n"
    "const got=m[key()]\nreturn got*100+m[1]}\n"
    "export fn commit()->i64{var m=#{1:10}\nfn rhs()->i64{m=#{1:20,2:30}\nreturn 7}\n"
    "m[1]=rhs()\nreturn m[1]*100+m[2]}\n"
    "export fn ordered()->i64{var maps=[#{1:10}]\nconst old=maps\nvar trace=0\n"
    "fn at()->i64{trace=trace*10+1\nreturn 0}\nfn key()->i64{trace=trace*10+2\nreturn 1}\n"
    "fn rhs()->i64{trace=trace*10+3\nmaps=[#{2:30}]\nreturn 7}\n"
    "maps[at()][key()]=rhs()\nif(old[0][1]!=10){return -7}\nreturn trace*10000+maps[0][1]*100+maps[0][2]}\n"
    "export fn late()->string{const m=#{\"a\":\"alive\"}\nreturn m[\"a\"]}\n"
    "export fn caught()->i64{try{return required(#{1:9},2)}catch panic(p:PanicInfo){return p.code}\nreturn -1}\n"
    "export fn message()->string{try{const m=#{1:9}\nconst v=m[2]\nreturn \"bad\"}"
    "catch panic(p){return p.message}\nreturn \"bad\"}\n"
    "export fn nullValue()->i64?{const m:Map<i64,i64?> = #{1:null}\nreturn m[1]}\n"
    "export fn info()->PanicInfo{try{const m=#{1:9}\nconst v=m[2]}catch panic(p){return p}\nreturn info()}\n"
    "export fn missing()->i64{const m=#{1:9}\nreturn m[2]}\n";
static inline void index_entries(const XrXirModule *module, uint32_t *entries) {
    const char *names[INDEX_COUNT] = {"basic", "generic", "nested", "snapshot", "commit", "ordered",
        "late", "caught", "message", "nullValue", "info", "missing"};
    for (uint32_t n = 0; n < INDEX_COUNT; ++n) {
        entries[n] = UINT32_MAX;
        for (uint32_t f = 0; f < module->function_count; ++f) {
            const XrXirFunction *function = &module->functions[f];
            if (function->name_length == strlen(names[n]) && !memcmp(function->name, names[n], strlen(names[n]))) {
                CHECK(entries[n] == UINT32_MAX); entries[n] = f;
            }
        }
        CHECK(entries[n] != UINT32_MAX);
    }
}
#endif // P09_MAP_INDEX_INPUT_H
