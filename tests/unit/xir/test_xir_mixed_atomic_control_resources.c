/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_atomic_control_resources.c - Exact original-owner control budgets
 */
#include "xir_mixed_atomic_control_resources.h"
static unsigned atomic_budget_number(const char *text) {
    char *end=NULL;unsigned long n=strtoul(text,&end,10);CHECK(text[0] && end && !*end && n<20000);return(unsigned)n;
}
int main(int argc,char **argv) {(void)atomic_runtime_before_free;(void)atomic_resource_baseline;
    CHECK(argc==11 || argc==13);(void)atomic_resource_original_main;(void)atomic_resource_axes;
    const char *names[]={"0","1","vm","native"};unsigned backend=0;
    while(backend<4 && strcmp(argv[1],names[backend]))++backend;CHECK(backend<4);
    AtomicControlSpec spec={.backend=backend,.victim=atomic_budget_number(argv[3]),.call=atomic_budget_number(argv[4]),.entry=atomic_budget_number(argv[5]),.prefix=atomic_budget_number(argv[6]),.polls=atomic_budget_number(argv[7]),.mode=atomic_budget_number(argv[8]),.close=atomic_budget_number(argv[9]),.drop=atomic_budget_number(argv[10])};
    if(!strcmp(argv[2],"range") || !strcmp(argv[2],"range-axes")) {
        CHECK(argc==13);unsigned begin=atomic_budget_number(argv[11]),end=atomic_budget_number(argv[12]);CHECK(begin<end && end<=spec.polls+1);
        for(unsigned prefix=begin;prefix<end;++prefix) {
            spec.prefix=prefix;AtomicResourceResult normal=atomic_budget_baseline(&spec);
            if(!strcmp(argv[2],"range-axes"))atomic_budget_axes(&spec,&normal);
        }
        return 0;
    }
    CHECK(argc==11);AtomicResourceResult normal=atomic_budget_baseline(&spec);
    if(!strcmp(argv[2],"baseline"))return 0;
    CHECK(!strcmp(argv[2],"axes"));atomic_budget_axes(&spec,&normal);return 0;
}
