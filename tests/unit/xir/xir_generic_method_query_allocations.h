/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_method_query_allocations.h - Independent generic method snapshots
 *
 * KEY CONCEPT:
 *   A description owns nested constraints without becoming a source of proof.
 */
#ifndef XIR_GENERIC_METHOD_QUERY_ALLOCATIONS_H
#define XIR_GENERIC_METHOD_QUERY_ALLOCATIONS_H
#include "xir_generic_method_owned_fixture.h"
static void generic_method_query_allocations(void) {
    CHECK(!source_fixture_compile_live); size_t sites = allocation_case_begin(6);
    for(size_t attempt=allocation_first();allocation_more(attempt,sites);attempt=allocation_next(attempt)) {
        GenericMethodOwnedFixture fixture; generic_method_owned_fixture(&fixture);
        XrXirSourceView view = {0}; view.types = &fixture.types;
        XrCompileResourceLimits limits = allocation_limits();
        source_fixture_compile_attempts = 0; source_fixture_compile_fail_at = attempt ? attempt-1 : SIZE_MAX; source_fixture_compile_injected=false;
        XrXirSourceSnapshot *snapshot = NULL;
        XrXirStatus status = allocation_snapshot_copy(&view,&limits,&snapshot);
        if (!attempt) {
            CHECK(status == XR_XIR_OK && snapshot); sites = source_fixture_compile_attempts;
                allocation_snapshot_boundaries(&view,allocation_last_stats);
                const XrXirInterfaceTable *table = xr_xir_compile_source_snapshot_view(snapshot)->types->interfaces;
            CHECK(table != &fixture.table && table->declarations[1].methods != fixture.methods);
            CHECK(table->declarations[1].methods[1].constraints != fixture.own+1);
            memset(&fixture,0xCC,sizeof(fixture)); memset(&view,0xCC,sizeof(view));
            generic_method_owned_assert(table);
        } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !snapshot);
        xr_xir_compile_source_snapshot_free(snapshot); CHECK(!source_fixture_compile_live);
            if(attempt)allocation_point(attempt-1,XR_XIR_OUT_OF_MEMORY,true);
}
        allocation_case_end(sites);
    source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected=false;
    printf("Generic method query: %zu allocation failure sites\n",sites);
}
#endif // XIR_GENERIC_METHOD_QUERY_ALLOCATIONS_H
