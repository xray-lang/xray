/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XIR_ACCESS_TEMPORARY_CASES_H
#define XIR_ACCESS_TEMPORARY_CASES_H
typedef struct AccessTemporaryFixture { XrXirModule *module; XrXirType type; } AccessTemporaryFixture;
static XrXirStatus access_temporary_operation(const XrXirCompileContext *context,void *opaque) {
    AccessTemporaryFixture *f=opaque;return xr_xir_compile_type_access(context,f->module,1,f->type);
}
static XrXirStatus declarations_temporary_operation(const XrXirCompileContext *context,void *opaque) {
    AccessTemporaryFixture *f=opaque;
    return xr_xir_compile_declarations_verify(context,f->module->declarations,f->module->types,2,f->module->linkage_kind);
}
static void access_temporary_cases(void) {
    XrXirNominalDeclaration nominal={.module={"a",1},.name={"S",1}};XrXirNominalTable table={&nominal,1,NULL};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL};XrXirTypes types={&node,1,&table,NULL};
    XrXirType type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirFunctionIdentity identities[2]={{0},{0}};XrXirSourceModule source={"a",1,NULL,0,0};XrXirSlot slot={0,type,0};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.slots=&slot,.slot_count=1,.entry_function=1};
    XrXirModule module={.stage=XR_XIR_CHECKED,.function_count=2,.declarations=&declarations,.types=&types};
    AccessTemporaryFixture f={&module,type};
    AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits);
    CHECK(xr_xir_compile_types_structure_verify(&owner.context,&types)==XR_XIR_OK);
    for(uint32_t i=0;i<32;++i) {
        XrCompileResourceStats before=allocation_compile_stats(&owner.context);
        CHECK(access_temporary_operation(&owner.context,&f)==XR_XIR_OK);
        XrCompileResourceStats after=allocation_compile_stats(&owner.context);
        CHECK(after.work>before.work && after.live_bytes==owner.baseline.live_bytes);
    }
    allocation_compile_owner_drop(&owner);
    allocation_compile_operation_cases("type access",access_temporary_operation,&f);
    allocation_compile_operation_cases("declaration graph",declarations_temporary_operation,&f);
    uint32_t dependencies[2]={1,0};
    XrXirSourceModule modules[2]={{"a",1,dependencies,1,0},{"b",1,dependencies+1,1,1}};
    XrXirFunctionIdentity cycle_ids[3]={{0},{.module=1},{0}};
    XrXirDeclarations cycle={.modules=modules,.module_count=2,.functions=cycle_ids,.entry_function=2};
    allocation_compile_owner_new(&owner,&allocation_compile_limits);
    CHECK(xr_xir_compile_declarations_verify(&owner.context,&cycle,NULL,3,XR_XIR_PROGRAM)==XR_XIR_BAD_STRUCTURE);
    cycle_ids[2].module=1;cycle.root_module=1;
    XrXirModule private_scope={.stage=XR_XIR_CHECKED,.function_count=3,.declarations=&cycle,.types=&types};
    CHECK(xr_xir_compile_type_access(&owner.context,&private_scope,2,type)==XR_XIR_BAD_TYPE);
    allocation_compile_owner_drop(&owner);calls=0;
}
#endif // XIR_ACCESS_TEMPORARY_CASES_H
