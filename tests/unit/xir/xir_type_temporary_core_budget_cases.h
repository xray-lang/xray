/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XIR_TYPE_TEMPORARY_CORE_BUDGET_CASES_H
#define XIR_TYPE_TEMPORARY_CORE_BUDGET_CASES_H
typedef struct TypeTemporaryFixture { XrXirTypeNode node; XrXirTypes from,to; } TypeTemporaryFixture;
static void type_temporary_fixture(TypeTemporaryFixture *f) {
    *f=(TypeTemporaryFixture){0};
    f->node=(XrXirTypeNode){XR_XIR_TYPE_ARRAY,XR_XIR_I64,NULL,0,XR_XIR_UNIT,0,0,{0}};
    f->from=(XrXirTypes){&f->node,1,NULL,NULL};f->to=f->from;
}
static XrXirStatus type_temporary_shape(const XrXirCompileContext *context,void *opaque) {
    TypeTemporaryFixture *f=opaque;
    return xr_xir_compile_type_expression_shape(context,&f->from,(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,0);
}
static XrXirStatus type_temporary_match(const XrXirCompileContext *context,void *opaque) {
    TypeTemporaryFixture *f=opaque;
    return xr_xir_compile_type_substitution_matches_between(context,&f->from,&f->to,NULL,0,
        (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE);
}
static void type_temporary_budget_cases(void) {
    TypeTemporaryFixture f;type_temporary_fixture(&f);
    AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits);
    for(unsigned mode=0;mode<2;++mode)for(uint32_t i=0;i<32;++i) {
        XrCompileResourceStats before=allocation_compile_stats(&owner.context);
        CHECK((mode?type_temporary_match:type_temporary_shape)(&owner.context,&f)==XR_XIR_OK);
        XrCompileResourceStats after=allocation_compile_stats(&owner.context);
        CHECK(after.live_bytes==owner.baseline.live_bytes && after.work>before.work && after.allocated_bytes>before.allocated_bytes);
    }
    XrXirTypeNode wrong=f.node;wrong.element=XR_XIR_BOOL;f.to.nodes=&wrong;
    CHECK(type_temporary_match(&owner.context,&f)==XR_XIR_BAD_TYPE);
    allocation_compile_owner_drop(&owner);
    type_temporary_fixture(&f);
    allocation_compile_operation_cases("type shape",type_temporary_shape,&f);
    allocation_compile_operation_cases("cross-pool type match",type_temporary_match,&f);
}
static void type_temporary_allocation_failures(void) {
    /* Both complete actual allocation scans are part of the budget cases above.
     * Retain the original independent entry point without duplicating injection. */
    CHECK(!live && !class_live_bytes);
}
#endif // XIR_TYPE_TEMPORARY_CORE_BUDGET_CASES_H
