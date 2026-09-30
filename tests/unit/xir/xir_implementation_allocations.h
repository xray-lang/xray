/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XIR_IMPLEMENTATION_ALLOCATIONS_H
#define XIR_IMPLEMENTATION_ALLOCATIONS_H
#include "xir/xxir_implementation.h"
static void implementation_copy_allocations(void) {
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        XrXirType arguments[] = {XR_XIR_I64,XR_XIR_BOOL};
        XrXirImplementationBinding bindings[] = {{{0,arguments,1},0,2},{{0,arguments+1,1},0,3}};
        XrXirImplementation records[] = {{0,{1,arguments,1},bindings,2},{1,{2,NULL,0},NULL,0}};
        XrXirImplementationTable table = {records,2}, *copy = NULL;
        calls = 0; fail_at = site ? site-1 : SIZE_MAX;
        XrXirStatus status = xr_xir_implementations_copy_verified(&table,&copy);
        if (!site) {
            CHECK(status == XR_XIR_OK && copy); sites = calls;
            CHECK(copy->records != records && copy->records[0].bindings != bindings);
            CHECK(copy->records[0].interface.arguments != copy->records[0].bindings[0].requirement.arguments);
            memset(records,0xcc,sizeof(records)); memset(bindings,0xcc,sizeof(bindings));
            memset(arguments,0xcc,sizeof(arguments));
            CHECK(copy->count == 2 && copy->records[0].interface.arguments[0] == XR_XIR_I64);
            CHECK(copy->records[0].bindings[1].requirement.arguments[0] == XR_XIR_BOOL);
            CHECK(copy->records[1].nominal_declaration == 1 && !copy->records[1].bindings);
        } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !copy);
        xr_xir_implementations_free(copy); CHECK(!live);
    }
    fail_at = SIZE_MAX;
    XrXirImplementationTable empty = {0}, *copy = (void *)(uintptr_t)1;
    CHECK(xr_xir_implementations_copy_verified(&empty,&copy) == XR_XIR_BAD_STRUCTURE && !copy);
    CHECK(xr_xir_implementations_copy_verified(NULL,&copy) == XR_XIR_OK && !copy);
    printf("Implementation ownership: %zu allocation failure sites\n",sites);
}
#endif // XIR_IMPLEMENTATION_ALLOCATIONS_H
