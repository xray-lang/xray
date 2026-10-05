/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XIR_IMPLEMENTATION_QUERY_ALLOCATIONS_H
#define XIR_IMPLEMENTATION_QUERY_ALLOCATIONS_H
#include "xir/xxir_implementation.h"
static void implementation_query_allocations(void) {
    size_t sites = allocation_case_begin(7);
    for(size_t site=allocation_first();allocation_more(site,sites);site=allocation_next(site)) {
        XrXirType arguments[] = {XR_XIR_I64, XR_XIR_BOOL};
        XrXirImplementationBinding bindings[] = {{{0,arguments,1},0,2},{{0,arguments+1,1},0,3}};
        XrXirImplementation record = {0,{1,NULL,0},bindings,2};
        XrXirImplementationTable table = {&record,1};
        XrXirSourceView view = {0}; view.implementations = &table;
        XrCompileResourceLimits limits = allocation_limits();
        source_fixture_compile_attempts = 0; source_fixture_compile_fail_at = site ? site - 1 : SIZE_MAX; source_fixture_compile_injected=false;
        XrXirSourceSnapshot *snapshot = NULL;
        XrXirStatus status = allocation_snapshot_copy(&view,&limits,&snapshot);
        if (!site) {
            CHECK(status == XR_XIR_OK && snapshot); sites = source_fixture_compile_attempts;
                allocation_snapshot_boundaries(&view,allocation_last_stats);
                memset(arguments,0xcc,sizeof(arguments)); memset(bindings,0xcc,sizeof(bindings));
            memset(&record,0xcc,sizeof(record)); memset(&table,0xcc,sizeof(table));
            const XrXirImplementationTable *copy = xr_xir_compile_source_snapshot_view(snapshot)->implementations;
            CHECK(copy && copy->count == 1 && copy->records[0].binding_count == 2);
            CHECK(copy->records[0].bindings[0].requirement.arguments[0] == XR_XIR_I64);
            CHECK(copy->records[0].bindings[1].requirement.arguments[0] == XR_XIR_BOOL);
            CHECK(copy->records[0].bindings[0].function == 2 && copy->records[0].bindings[1].function == 3);
        } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !snapshot);
        xr_xir_compile_source_snapshot_free(snapshot); CHECK(!source_fixture_compile_live);
            if(site)allocation_point(site-1,XR_XIR_OUT_OF_MEMORY,true);
}
        allocation_case_end(sites);
    source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected=false;
    printf("Implementation query: %zu allocation failure sites\n",sites);
}
#endif // XIR_IMPLEMENTATION_QUERY_ALLOCATIONS_H
