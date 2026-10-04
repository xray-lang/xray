/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * KEY CONCEPT:
 *   Fixed Unicode scalar expectations exercise the canonical finite pipeline.
 */
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
#include "xir_runtime_allocations.h"
#include "xir/xxir_scalar.h"
#include "xir_rune_leaf_cases.h"
#define DECLARE(n) XR_FUNC XrXirRunStatus rune_leaf_f##n(XrXirRunContext *,const XrXirValue *,uint32_t,XrXirValue *);
DECLARE(0) DECLARE(1) DECLARE(2) DECLARE(3)
#undef DECLARE
static XrXirRunStatus native_run(void *owner,uint32_t f,XrXirRunContext *context,const XrXirValue *args,uint32_t count,XrXirValue *result) {
    const XrXirLeafEntry entries[]={rune_leaf_f0,rune_leaf_f1,rune_leaf_f2,rune_leaf_f3};
    (void)owner;CHECK(f<4);return entries[f](context,args,count,result);
}
int main(void) {rune_leaf_cases(native_run,NULL);CHECK(!runtime_live && !runtime_bytes);return 0;}
