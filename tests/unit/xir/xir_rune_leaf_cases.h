/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * KEY CONCEPT:
 *   Fixed Unicode scalar expectations exercise the canonical finite pipeline.
 */
#ifndef XIR_RUNE_LEAF_CASES_H
#define XIR_RUNE_LEAF_CASES_H
typedef XrXirRunStatus (*RuneLeafRun)(void *,uint32_t,XrXirRunContext *,const XrXirValue *,uint32_t,XrXirValue *);
static void rune_leaf_cases(RuneLeafRun run,void *owner) {
    static const int64_t good[]={0,0x41,0x7f,0x80,0x7ff,0x800,0xd7ff,0xe000,0xffff,0x10000,0x1f600,0x10ffff};
    static const int64_t bad[]={-1,INT64_MIN,0xd800,0xdbff,0xdc00,0xdfff,0x110000,INT64_MAX,INT64_C(0x100000041)};
    for(size_t i=0;i<sizeof(good)/sizeof(good[0]);++i)for(uint32_t f=0;f<3;++f) {
        XrXirRunContext context={1000,65536,0,0,0,0};
        XrXirValue input={f?XR_XIR_RUNE:XR_XIR_I64,0,good[i]},result={0};
        CHECK(run(owner,f,&context,&input,1,&result)==XR_XIR_RUN_OK);
        CHECK(result.type==(uint32_t)(f==2?XR_XIR_U32:XR_XIR_I64) && !result.reserved && result.payload==good[i]);
        CHECK(!context.live_bytes && context.allocations==context.frees);xr_xir_value_drop(&result);
    }
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i)for(uint32_t f=0;f<3;++f) {
        XrXirRunContext context={1000,65536,0,0,0,0};
        XrXirValue input={f?XR_XIR_RUNE:XR_XIR_I64,0,bad[i]},result={0};
        CHECK(run(owner,f,&context,&input,1,&result)==(f?XR_XIR_RUN_BAD_ARGUMENT:XR_XIR_RUN_NUMERIC_RANGE));
        CHECK(!result.type && !result.reserved && !result.payload);
        CHECK(!context.live_bytes && context.allocations==context.frees);
    }
    XrXirRunContext context={1000,65536,0,0,0,0};XrXirValue result={0};
    CHECK(run(owner,3,&context,NULL,0,&result)==XR_XIR_RUN_OK);
    CHECK(result.type==XR_XIR_RUNE && !result.reserved && result.payload==0x1f600);
    CHECK(!context.live_bytes && context.allocations==context.frees);xr_xir_value_drop(&result);
}
#endif // XIR_RUNE_LEAF_CASES_H
