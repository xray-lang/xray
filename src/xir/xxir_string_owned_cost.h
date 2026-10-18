/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_string_owned_cost.h - Checked byte-search work before owned construction
 */
#ifndef XXIR_STRING_OWNED_COST_H
#define XXIR_STRING_OWNED_COST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
static inline bool xir_string_owned_add_work(uint64_t *total,uint64_t count,uint64_t unit) {
    if(unit && count>(UINT64_MAX-*total)/unit) return false;
    *total+=count*unit;return true;
}
/* Disjoint searches visit at most one whole input per pass. Every long-pattern
 * search still pays its complete skip-table construction and lookup span. */
static inline bool xir_string_owned_search_work(size_t bytes,size_t pattern,uint64_t calls,
    uint64_t passes,uint64_t *output) {
    *output=0;
    if(!pattern || pattern>bytes) return true;
    uint64_t visits=(uint64_t)bytes-pattern+1,span=2+(pattern>8?sizeof(size_t):0);
    if(pattern>UINT64_MAX-span || !xir_string_owned_add_work(output,visits,(uint64_t)pattern+span)) return false;
    if(!xir_string_owned_add_work(output,bytes,1) || (passes && *output>UINT64_MAX/passes)) return false;
    *output*=passes;
    uint64_t setup=0;
    if(pattern>8 && (!xir_string_owned_add_work(&setup,256,sizeof(size_t)) ||
        !xir_string_owned_add_work(&setup,pattern,sizeof(size_t)+1))) return false;
    return xir_string_owned_add_work(output,calls,setup);
}
#endif
