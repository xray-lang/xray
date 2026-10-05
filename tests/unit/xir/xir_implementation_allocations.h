/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XIR_IMPLEMENTATION_ALLOCATIONS_H
#define XIR_IMPLEMENTATION_ALLOCATIONS_H
#include "xir/xxir_implementation.h"
static XrXirStatus implementation_copy_operation(const XrXirCompileContext *context,void *opaque) {
    (void)opaque;
    XrXirType arguments[]={XR_XIR_I64,XR_XIR_BOOL};
    XrXirImplementationBinding bindings[]={{{0,arguments,1},0,2},{{0,arguments+1,1},0,3}};
    XrXirImplementation records[]={{0,{1,arguments,1},bindings,2},{1,{2,NULL,0},NULL,0}};
    XrXirImplementationTable table={records,2},*copy=NULL;
    XrXirStatus status=xr_xir_compile_implementations_copy_verified(context,&table,&copy);
    if(status==XR_XIR_OK) {
        CHECK(copy && copy->records!=records && copy->records[0].bindings!=bindings);
        CHECK(copy->records[0].interface.arguments!=copy->records[0].bindings[0].requirement.arguments);
        memset(records,0xcc,sizeof(records));memset(bindings,0xcc,sizeof(bindings));memset(arguments,0xcc,sizeof(arguments));
        CHECK(copy->count==2 && copy->records[0].interface.arguments[0]==XR_XIR_I64);
        CHECK(copy->records[0].bindings[1].requirement.arguments[0]==XR_XIR_BOOL);
        CHECK(copy->records[1].nominal_declaration==1 && !copy->records[1].bindings);
    } else CHECK(!copy);
    xr_xir_compile_implementations_free(copy);return status;
}
static void implementation_copy_allocations(void) {
    allocation_compile_operation_cases("implementation ownership",implementation_copy_operation,NULL);
    AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits);
    XrXirImplementationTable empty={0},*copy=NULL;
    CHECK(xr_xir_compile_implementations_copy_verified(&owner.context,&empty,&copy)==XR_XIR_BAD_STRUCTURE && !copy);
    CHECK(xr_xir_compile_implementations_copy_verified(&owner.context,NULL,&copy)==XR_XIR_OK && !copy);
    copy=(void *)(uintptr_t)1;
    CHECK(xr_xir_compile_implementations_copy_verified(&owner.context,&empty,&copy)==XR_XIR_BAD_STRUCTURE && copy==(void *)(uintptr_t)1);
    allocation_compile_owner_drop(&owner);
}
#endif // XIR_IMPLEMENTATION_ALLOCATIONS_H
