#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_initialization_allocations.h - Detached initialization failure and budget boundaries
 */
typedef struct InitializationAllocationFixture {
    const XrXirModule *module;const XrXirInitializationRegion *outer;
} InitializationAllocationFixture;
static XrXirStatus initialization_allocation_operation(const XrXirCompileContext *context,void *opaque) {
    InitializationAllocationFixture *f=opaque;
    XrXirDiagnostic diagnostic={XR_XIR_BAD_STRUCTURE,17,99,99,XR_XIR_DIAGNOSTIC_CLEANUP_THROW};
    XrXirStatus status=xr_xir_compile_initialization_check(context,f->module,0,f->outer,&diagnostic);
    CHECK(diagnostic.status==status && diagnostic.function==0);
    if(status==XR_XIR_OK || status==XR_XIR_OUT_OF_MEMORY || status==XR_XIR_BUDGET)
        CHECK(diagnostic.reason==XR_XIR_DIAGNOSTIC_NONE);
    return status;
}
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
    const XrXirBlock blocks[]={{0,4, 0, 0},{4,2, 0, 0},{6,2, 0, 0}};
    const XrXirFunction function={"initialize",10,&parameter,1,XR_XIR_STRING,blocks,1,ops,4,NULL,0};
    const XrXirModule module={XR_XIR_BUILT,&function,1,NULL,NULL,NULL,NULL, XR_XIR_PROGRAM, NULL};
    XrXirInitializationRegion outer={0},inner={0};
    outer.function=function; outer.function.instruction_count=6; outer.function.block_count=2;
    outer.first_instruction=4; outer.first_block=1; outer.checkpoint=2;
    inner.function=function; inner.function.instruction_count=8; inner.function.block_count=3;
    inner.first_instruction=6; inner.first_block=2; inner.checkpoint=5; inner.parent=&outer;
    outer.next=&inner;
    InitializationAllocationFixture f={&module,&outer};
    AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits);
    CHECK(xir_fixture_verify(&owner.context, &module, NULL)==XR_XIR_OK);
    allocation_compile_owner_drop(&owner);
    for(unsigned mode=0;mode<3;++mode) {
        outer.checkpoint=mode==1?1:2;
        ops[6]=mode==2?(XrXirInstruction){XR_XIR_LOCAL_WRITE,XR_XIR_UNIT,{1,0},{0},0,{0}}:
            (XrXirInstruction){XR_XIR_LOCAL_READ,XR_XIR_STRING,{1},{0},0,{0}};
        allocation_compile_owner_new(&owner,&allocation_compile_limits);
        calls=0;XrXirDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_initialization_check(&owner.context,&module,0,&outer,&diagnostic);
        CHECK(status==(mode?XR_XIR_BAD_VALUE:XR_XIR_OK));
        CHECK(diagnostic.status==status && diagnostic.function==0);
        CHECK(diagnostic.reason==(mode==1?XR_XIR_DIAGNOSTIC_UNINITIALIZED_READ:
            mode==2?XR_XIR_DIAGNOSTIC_READONLY_WRITE:XR_XIR_DIAGNOSTIC_NONE));
        if(mode)CHECK(diagnostic.block==mode && diagnostic.instruction==(mode==1?4u:6u));
        size_t sites=calls;CHECK(sites);allocation_compile_owner_drop(&owner);
        for(size_t site=0;site<sites;++site) {
            allocation_compile_owner_new(&owner,&allocation_compile_limits);
            calls=0;fail_at=site;
            CHECK(initialization_allocation_operation(&owner.context,&f)==XR_XIR_OUT_OF_MEMORY);
            fail_at=SIZE_MAX;allocation_compile_owner_drop(&owner);
        }
        printf("Detached initialization mode %u: %zu allocation failure sites\n",mode,sites);
    }
    outer.checkpoint=2;ops[6]=(XrXirInstruction){XR_XIR_LOCAL_READ,XR_XIR_STRING,{1},{0},0,{0}};
    allocation_compile_operation_cases("detached initialization",initialization_allocation_operation,&f);
    allocation_compile_owner_new(&owner,&allocation_compile_limits);
    XrXirDiagnostic diagnostic={XR_XIR_OK,17,99,99,XR_XIR_DIAGNOSTIC_READONLY_WRITE};
    CHECK(xr_xir_compile_initialization_check(&owner.context,NULL,7,&outer,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    CHECK(diagnostic.status==XR_XIR_BAD_STRUCTURE && diagnostic.function==7 && diagnostic.block==UINT32_MAX &&
        diagnostic.instruction==UINT32_MAX && diagnostic.reason==XR_XIR_DIAGNOSTIC_NONE);
    CHECK(xr_xir_compile_initialization_check(&owner.context,&module,1,&outer,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    CHECK(diagnostic.status==XR_XIR_BAD_STRUCTURE && diagnostic.function==1 && diagnostic.reason==XR_XIR_DIAGNOSTIC_NONE);
    allocation_compile_owner_drop(&owner);
}
