/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * structural_copy_cases.h - Independent structural-list copy work and ownership
 */
static const XrXirStructuralRequirement structural_copy_requirements[]={
    {(XrXirType)256,XR_XIR_STRUCTURAL_READONLY_RECEIVER},
    {(XrXirType)257,XR_XIR_STRUCTURAL_READONLY_RECEIVER}
};
static const XrXirConstraint structural_copy_constraint={
    .interfaces=&copy_application,.interface_count=1,
    .structural_requirements=structural_copy_requirements,.structural_count=2
};
static void structural_copy_owned(const XrXirConstraint *copy) {
    CHECK(copy && copy!=&structural_copy_constraint && copy->interfaces!=&copy_application);
    CHECK(copy->interfaces[0].arguments!=copy_arguments && copy->interfaces[0].arguments[1]==XR_XIR_STRING);
    CHECK(copy->structural_count==2 && copy->structural_requirements!=structural_copy_requirements);
    for (uint32_t r=0;r<2;++r) {
        CHECK(copy->structural_requirements[r].shape==(XrXirType)(256+r));
        CHECK(copy->structural_requirements[r].policy==XR_XIR_STRUCTURAL_READONLY_RECEIVER);
    }
}
static void structural_copy_formula(void) {
    /* Two table visits, four allocator calls, two clears and two owned copies,
     * plus the allocator call that creates the original ledger. */
    const uint64_t work=1+2+4+sizeof(XrXirConstraint)+sizeof(XrXirInterfaceApplication)+
        sizeof(copy_arguments)+sizeof(structural_copy_requirements);
    const XrCompileResourceLimits caps={1048576,1048576,1048576};
    reset(SIZE_MAX);XrCompileResources *owner=NULL;
    CHECK(xr_compile_resources_new(&caps,&owner)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={owner,xr_xir_compile_default_limits()};XrXirConstraint *copy=NULL;
    CHECK(xr_xir_compile_constraint_array_copy_verified(&context,&structural_copy_constraint,1,&copy)==XR_XIR_OK);
    CHECK(attempts==5 && stats(owner).work==work);structural_copy_owned(copy);
    XrCompileResourceStats normal=stats(owner);
    xr_compile_resources_release(owner);structural_copy_owned(copy);
    xr_xir_compile_constraint_array_free(copy,1);CHECK(!live && !live_count);
    for (size_t failure=0;failure<5;++failure) {
        reset(failure);owner=NULL;
        XrCompileResourceStatus opened=xr_compile_resources_new(&caps,&owner);
        if (!failure) {CHECK(opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !owner && !live_count);continue;}
        CHECK(opened==XR_COMPILE_RESOURCE_OK);context.resources=owner;
        XrXirConstraint sentinel={0};copy=&sentinel;
        CHECK(xr_xir_compile_constraint_array_copy_verified(&context,&structural_copy_constraint,1,&copy)==XR_XIR_OUT_OF_MEMORY);
        CHECK(copy==&sentinel && live_count==1);
        xr_compile_resources_release(owner);CHECK(!live && !live_count);
    }
    for (uint64_t cap=0;cap<work;++cap) {
        reset(SIZE_MAX);owner=NULL;XrCompileResourceLimits short_work=caps;short_work.work=cap;
        XrCompileResourceStatus opened=xr_compile_resources_new(&short_work,&owner);
        if (!cap) {CHECK(opened==XR_COMPILE_RESOURCE_BUDGET && !owner && !live_count);continue;}
        CHECK(opened==XR_COMPILE_RESOURCE_OK);context.resources=owner;
        XrXirConstraint sentinel={0};copy=&sentinel;
        CHECK(xr_xir_compile_constraint_array_copy_verified(&context,&structural_copy_constraint,1,&copy)==XR_XIR_BUDGET);
        CHECK(copy==&sentinel && stats(owner).work<=cap && live_count==1);
        uint64_t spent=stats(owner).work;
        CHECK(xr_xir_compile_constraint_array_copy_verified(&context,&structural_copy_constraint,1,&copy)==XR_XIR_BUDGET);
        CHECK(copy==&sentinel && stats(owner).work>=spent && stats(owner).work<=cap && live_count==1);
        xr_compile_resources_release(owner);CHECK(!live && !live_count);
    }
    for (uint32_t axis=0;axis<4;++axis) {
        reset(SIZE_MAX);owner=NULL;
        XrCompileResourceLimits exact={normal.allocated_bytes,normal.peak_bytes,work};
        if (axis==1) --exact.allocated_bytes;
        if (axis==2) --exact.live_bytes;
        if (axis==3) --exact.work;
        CHECK(xr_compile_resources_new(&exact,&owner)==XR_COMPILE_RESOURCE_OK);context.resources=owner;
        XrXirConstraint sentinel={0};copy=&sentinel;
        XrXirStatus result=xr_xir_compile_constraint_array_copy_verified(&context,&structural_copy_constraint,1,&copy);
        CHECK(result==(axis ? XR_XIR_BUDGET : XR_XIR_OK));
        if (axis) CHECK(copy==&sentinel);
        else {structural_copy_owned(copy);CHECK(stats(owner).work==work);xr_xir_compile_constraint_array_free(copy,1);}
        CHECK(live_count==1);xr_compile_resources_release(owner);CHECK(!live && !live_count);
    }
    printf("structural copy: independent work=%llu; five physical fault points, every work prefix, three axes and retained failed work PASS\n",
        (unsigned long long)work);
}
