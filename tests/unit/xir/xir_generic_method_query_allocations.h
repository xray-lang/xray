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
    CHECK(!live); size_t sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        GenericMethodOwnedFixture fixture; generic_method_owned_fixture(&fixture);
        XrXirSourceView view = {0}; view.types = &fixture.types;
        XrXirBudget initial = xr_xir_default_budget(), budget = initial;
        attempts = 0; fail_at = attempt ? attempt-1 : SIZE_MAX;
        XrXirSourceSnapshot *snapshot = NULL;
        XrXirStatus status = xr_xir_source_snapshot_copy(&view,&budget,&snapshot);
        if (!attempt) {
            CHECK(status == XR_XIR_OK && snapshot); sites = attempts;
            XrXirBudget required = initial;
            required.work -= budget.work; required.metadata_bytes -= budget.metadata_bytes;
            CHECK(required.work && required.metadata_bytes);
            for (unsigned boundary = 0; boundary < 3; ++boundary) {
                XrXirBudget limited = required; XrXirSourceSnapshot *bounded = NULL;
                if (!boundary) --limited.work;
                if (boundary == 1) --limited.metadata_bytes;
                CHECK(xr_xir_source_snapshot_copy(&view,&limited,&bounded) ==
                    (boundary == 2 ? XR_XIR_OK : XR_XIR_BUDGET));
                CHECK((bounded != NULL) == (boundary == 2)); xr_xir_source_snapshot_free(bounded);
            }
            const XrXirInterfaceTable *table = xr_xir_source_snapshot_view(snapshot)->types->interfaces;
            CHECK(table != &fixture.table && table->declarations[1].methods != fixture.methods);
            CHECK(table->declarations[1].methods[1].constraints != fixture.own+1);
            memset(&fixture,0xCC,sizeof(fixture)); memset(&view,0xCC,sizeof(view));
            generic_method_owned_assert(table);
        } else CHECK(status == XR_XIR_OUT_OF_MEMORY && !snapshot);
        xr_xir_source_snapshot_free(snapshot); CHECK(!live);
    }
    fail_at = SIZE_MAX;
    printf("Generic method query: %zu allocation failure sites\n",sites);
}
#endif // XIR_GENERIC_METHOD_QUERY_ALLOCATIONS_H
