/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XIR_CONSTRAINT_TEMPORARY_CORE_CASES_H
#define XIR_CONSTRAINT_TEMPORARY_CORE_CASES_H
typedef struct ConstraintTemporaryFixture { XrXirTypes *types;XrXirConstraint from,to; } ConstraintTemporaryFixture;
static XrXirStatus constraint_temporary_match(const XrXirCompileContext *context,void *opaque) {
    ConstraintTemporaryFixture *f=opaque;
    return xr_xir_compile_constraint_records_match(context,f->types,f->from,f->types,f->to,1);
}
static void constraint_temporary_cases(void) {
    XrXirConstraint empty={0};
    XrXirInterfaceDeclaration declaration={.module={"m",1},.name={"Evidence",8},.constraints=&empty,.parameter_count=1};
    XrXirInterfaceTable table={&declaration,1};XrXirTypes types={NULL,0,NULL,&table};
    XrXirType parameter=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirInterfaceApplication app={0,&parameter,1};XrXirConstraint constraint={0,&app,1};
    ConstraintTemporaryFixture f={&types,constraint,constraint};
    AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits);
    CHECK(xr_xir_compile_types_structure_verify(&owner.context,&types)==XR_XIR_OK);
    for(uint32_t i=0;i<32;++i) {
        XrCompileResourceStats before=allocation_compile_stats(&owner.context);
        CHECK(constraint_temporary_match(&owner.context,&f)==XR_XIR_OK);
        XrCompileResourceStats after=allocation_compile_stats(&owner.context);
        CHECK(after.live_bytes==owner.baseline.live_bytes && after.work>before.work);
    }
    XrXirType wrong=XR_XIR_I64;XrXirInterfaceApplication wrong_app={0,&wrong,1};
    f.to=(XrXirConstraint){0,&wrong_app,1};
    CHECK(constraint_temporary_match(&owner.context,&f)==XR_XIR_BAD_TYPE);
    allocation_compile_owner_drop(&owner);f.to=constraint;
    allocation_compile_operation_cases("constraint identity map",constraint_temporary_match,&f);
}
#endif // XIR_CONSTRAINT_TEMPORARY_CORE_CASES_H
