/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_atomic_control.c - Every real Atomic Source call control prefix
 */
#include "xir_mixed_atomic_control.h"
static unsigned atomic_control_number(const char *text) {
    char *end=NULL;unsigned long n=strtoul(text,&end,10);CHECK(text[0] && end && !*end && n<20000);return(unsigned)n;
}
static void atomic_control_peer(const AtomicControlSpec *spec,const AtomicResourceResult *normal,const AtomicResourceResult *actual) {
    unsigned peer=1-spec->victim;CHECK(actual->complete && actual->unaffected[peer]);
    CHECK(actual->cost[peer].requested_bytes==normal->cost[peer].requested_bytes && actual->cost[peer].requested_call_bytes==normal->cost[peer].requested_call_bytes && actual->cost[peer].work==normal->cost[peer].work);
}
static AtomicResourceResult atomic_control_baseline(AtomicControlSpec spec) {
    spec.victim=0;spec.call=0;spec.entry=0;spec.prefix=0;spec.polls=48;spec.mode=3;return atomic_control_transaction(&spec);
}
int main(int argc,char **argv) {
    CHECK(argc==10 || argc==11);(void)atomic_resource_original_main;(void)atomic_resource_axes;
    const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    AtomicControlSpec spec={.backend=backend,.victim=atomic_control_number(argv[3]),.call=atomic_control_number(argv[4]),.entry=atomic_control_number(argv[5])};
    if(!strcmp(argv[2],"suite")) {
        CHECK(argc==10);spec.polls=atomic_control_number(argv[6]);spec.mode=atomic_control_number(argv[7]);spec.close=atomic_control_number(argv[8]);spec.drop=atomic_control_number(argv[9]);
        AtomicResourceResult normal=atomic_control_baseline(spec);
        for(unsigned prefix=0;prefix<=spec.polls;++prefix){spec.prefix=prefix;AtomicResourceResult actual=atomic_control_transaction(&spec);atomic_control_peer(&spec,&normal,&actual);}return 0;
    }
    CHECK(argc==11 && !strcmp(argv[2],"prefix"));spec.prefix=atomic_control_number(argv[6]);spec.polls=atomic_control_number(argv[7]);spec.mode=atomic_control_number(argv[8]);spec.close=atomic_control_number(argv[9]);spec.drop=atomic_control_number(argv[10]);AtomicResourceResult normal=atomic_control_baseline(spec);AtomicResourceResult actual=atomic_control_transaction(&spec);atomic_control_peer(&spec,&normal,&actual);return 0;
}
