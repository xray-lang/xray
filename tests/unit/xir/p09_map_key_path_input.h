/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_map_key_path_input.h - Key evaluation, late root resolution and scoped selectors
 */
#ifndef P09_MAP_KEY_PATH_INPUT_H
#define P09_MAP_KEY_PATH_INPUT_H
typedef struct MapKeySourceCase { const char *name, *source; int64_t expected; } MapKeySourceCase;
static const MapKeySourceCase map_key_sources[]={
    {"basic",
        "export fn entry()->i64{var m=#{\"key\":[1,2]}\n"
        "const old=m\n"
        "m[\"key\"][0]=7\n"
        "m[\"key\"].push(3)\n"
        "return old[\"key\"][0]*100+len(m[\"key\"])*10+m[\"key\"][0]}\n"
        ,137},
    {"ordered",
        "export fn entry()->i64{var m=#{\"key\":[1]}\n"
        "const old=m\n"
        "var trace=0\n"
        "fn key()->string{trace=trace*10+1\n"
        "return \"k\"+\"ey\"}\n"
        "fn at()->i64{trace=trace*10+2\n"
        "return 0}\n"
        "fn rhs()->i64{trace=trace*10+3\n"
        "m=#{\"key\":[8,9]}\n"
        "return 7}\n"
        "m[key()][at()]=rhs()\n"
        "if(old[\"key\"][0]!=1){return -1}\n"
        "return trace*100+len(m[\"key\"])*10+m[\"key\"][0]}\n"
        ,12327},
    {"projected",
        "fn edit(row:ref Array<i64>){Coro.yield()\n"
        "row[0]=9\n"
        "row.push(10)}\n"
        "export fn entry()->i64{var m=#{\"key\":[1]}\n"
        "const old=m\n"
        "edit(ref m[\"k\"+\"ey\"])\n"
        "return old[\"key\"][0]*100+len(m[\"key\"])*10+m[\"key\"][0]}\n"
        ,129},
    {"missing",
        "export fn entry()->i64{var m=#{\"key\":[1]}\n"
        "try{m[\"missing\"][0]=9}catch panic(p){return p.code}\n"
        "return -1}\n"
        ,431},
    {"bounds",
        "export fn entry()->i64{var m=#{\"key\":[1]}\n"
        "try{m[\"key\"][9]=9}catch panic(p){return p.code}\n"
        "return -1}\n"
        ,430},
    {"late_missing",
        "export fn entry()->i64{var m=#{\"key\":[1]}\n"
        "var trace=0\n"
        "fn key()->string{trace=trace*10+1\n"
        "return \"k\"+\"ey\"}\n"
        "fn rhs()->i64{trace=trace*10+2\n"
        "m.clear()\n"
        "return 9}\n"
        "try{m[key()][0]=rhs()}catch panic(p){return trace*1000+p.code}\n"
        "return -1}\n"
        ,12431},
    {"nested_map",
        "export fn entry()->i64{var m=#{\"outer\":#{\"inner\":[1]}}\n"
        "const old=m\n"
        "m[\"outer\"][\"inner\"][0]=8\n"
        "return old[\"outer\"][\"inner\"][0]*100+m[\"outer\"][\"inner\"][0]}\n"
        ,108},
    {"ref_missing",
        "fn edit(row:ref Array<i64>){row[0]=9}\n"
        "export fn entry()->i64{var m=#{\"key\":[1]}\n"
        "try{edit(ref m[\"missing\"])}catch panic(p){return p.code}\n"
        "return -1}\n"
        ,431},
};
#endif // P09_MAP_KEY_PATH_INPUT_H
