/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_initialization_allocations.h - Detached initialization failure and budget boundaries
 */
static void initialization_region_allocations(void) {
    const XrXirType parameter=XR_XIR_STRING;
    XrXirInstruction ops[]={
        {XR_XIR_LOCAL_UNINIT,XR_XIR_STRING,{0},{0},1,{0}},
        {XR_XIR_LOCAL_WRITE,XR_XIR_UNIT,{1,0},{0},0,{0}},
        {XR_XIR_LOCAL_READ,XR_XIR_STRING,{1},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{3},{0},0,{0}},
        {XR_XIR_LOCAL_READ,XR_XIR_STRING,{1},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{5},{0},0,{0}},
        {XR_XIR_LOCAL_READ,XR_XIR_STRING,{1},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}
    };
    const XrXirBlock blocks[]={{0,4},{4,2},{6,2}};
    const XrXirFunction function={"initialize",10,&parameter,1,XR_XIR_STRING,blocks,1,ops,4,NULL,0};
    const XrXirModule module={XR_XIR_BUILT,&function,1,NULL,NULL,NULL,NULL};
    XrXirInitializationRegion outer={0},inner={0};
    outer.function=function; outer.function.instruction_count=6; outer.function.block_count=2;
    outer.first_instruction=4; outer.first_block=1; outer.checkpoint=2;
    inner.function=function; inner.function.instruction_count=8; inner.function.block_count=3;
    inner.first_instruction=6; inner.first_block=2; inner.checkpoint=5; inner.parent=&outer;
    outer.next=&inner;
    CHECK(xr_xir_verify(&module,NULL,NULL)==XR_XIR_OK && !live);
    for (unsigned mode=0;mode<3;++mode) {
        outer.checkpoint=mode==1 ? 1 : 2;
        ops[6]=mode==2 ? (XrXirInstruction){XR_XIR_LOCAL_WRITE,XR_XIR_UNIT,{1,0},{0},0,{0}} :
            (XrXirInstruction){XR_XIR_LOCAL_READ,XR_XIR_STRING,{1},{0},0,{0}};
        XrXirBudget budget=xr_xir_default_budget();
        calls=0; fail_at=SIZE_MAX;
        XrXirStatus status=xr_xir_initialization_check(&module,&function,&outer,&budget,NULL);
        CHECK(status==(mode ? XR_XIR_BAD_VALUE : XR_XIR_OK) && !live);
        size_t sites=calls; CHECK(sites>0);
        for (size_t site=0;site<sites;++site) {
            calls=0; fail_at=site; budget=xr_xir_default_budget();
            CHECK(xr_xir_initialization_check(&module,&function,&outer,&budget,NULL)==XR_XIR_OUT_OF_MEMORY);
            CHECK(!live);
        }
        fail_at=SIZE_MAX;
        printf("Detached initialization mode %u: %zu allocation failure sites\n",mode,sites);
    }
    outer.checkpoint=2; ops[6]=(XrXirInstruction){XR_XIR_LOCAL_READ,XR_XIR_STRING,{1},{0},0,{0}};
    XrXirBudget remaining=xr_xir_default_budget(), required=remaining;
    CHECK(xr_xir_initialization_check(&module,&function,&outer,&remaining,NULL)==XR_XIR_OK);
    required.work-=remaining.work; required.metadata_bytes-=remaining.metadata_bytes;
    for (unsigned kind=0;kind<3;++kind) {
        XrXirBudget budget=required;
        if (kind==1) --budget.work;
        if (kind==2) --budget.metadata_bytes;
        CHECK(xr_xir_initialization_check(&module,&function,&outer,&budget,NULL)==(kind ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(!live);
    }
}
