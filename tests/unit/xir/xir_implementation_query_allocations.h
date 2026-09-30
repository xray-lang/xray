/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XIR_IMPLEMENTATION_QUERY_ALLOCATIONS_H
#define XIR_IMPLEMENTATION_QUERY_ALLOCATIONS_H
#include "xir/xxir_implementation.h"
static void implementation_query_allocations(void) {
    size_t sites = 0;
    for (size_t site = 0; site <= sites; ++site) {
        XrXirType arguments[] = {XR_XIR_I64, XR_XIR_BOOL};
        XrXirImplementationBinding bindings[] = {{{0,arguments,1},0,2},{{0,arguments+1,1},0,3}};
        XrXirImplementation record = {0,{1,NULL,0},bindings,2};
        XrXirImplementationTable table = {&record,1};
        XrXirSourceView view = {0}; view.implementations = &table;
        XrXirBudget budget = xr_xir_default_budget();
        attempts = 0; fail_at = site ? site - 1 : SIZE_MAX;
        XrXirSourceSnapshot *snapshot = NULL;
        XrXirStatus status = xr_xir_source_snapshot_copy(&view,&budget,&snapshot);
        if (!site) {
            CHECK(status == XR_XIR_OK && snapshot); sites = attempts;
            XrXirBudget required = xr_xir_default_budget();
            required.work -= budget.work; required.metadata_bytes -= budget.metadata_bytes;
            for (unsigned boundary = 0; boundary < 3; ++boundary) {
                XrXirBudget limit = required; XrXirSourceSnapshot *bounded = NULL;
                if (boundary == 0) --limit.work;
                if (boundary == 1) --limit.metadata_bytes;
                CHECK(xr_xir_source_snapshot_copy(&view,&limit,&bounded) ==
                    (boundary == 2 ? XR_XIR_OK : XR_XIR_BUDGET));
                CHECK((bounded != NULL) == (boundary == 2)); xr_xir_source_snapshot_free(bounded);
            }
            memset(arguments,0xcc,sizeof(arguments)); memset(bindings,0xcc,sizeof(bindings));
            memset(&record,0xcc,sizeof(record)); memset(&table,0xcc,sizeof(table));
            const XrXirImplementationTable *copy = xr_xir_source_snapshot_view(snapshot)->implementations;
            CHECK(copy && copy->count == 1 && copy->records[0].binding_count == 2);
            CHECK(copy->records[0].bindings[0].requirement.arguments[0] == XR_XIR_I64);
            CHECK(copy->records[0].bindings[1].requirement.arguments[0] == XR_XIR_BOOL);
            CHECK(copy->records[0].bindings[0].function == 2 && copy->records[0].bindings[1].function == 3);
        } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !snapshot);
        xr_xir_source_snapshot_free(snapshot); CHECK(!live);
    }
    fail_at = SIZE_MAX;
    printf("Implementation query: %zu allocation failure sites\n",sites);
}
#endif // XIR_IMPLEMENTATION_QUERY_ALLOCATIONS_H
