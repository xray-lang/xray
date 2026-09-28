/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_call_native.c - Independent native call and suspension expectations
 *
 * KEY CONCEPT:
 *   Only emitted C and the call runtime participate; no interpreter is linked.
 */
#include "xir/xxir_call.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_error_fixture.h"
XR_DATA const XrXirCallEntry fixture_calls0_entries[5];
XR_DATA const XrXirCallEntry fixture_calls1_entries[5];
XR_DATA const XrXirCallEntry fixture_calls2_entries[5];
XR_DATA const XrXirCallEntry fixture_calls3_entries[5];
int main(void) {
    const XrXirCallEntry *tables[] = {fixture_calls0_entries, fixture_calls1_entries, fixture_calls2_entries, fixture_calls3_entries};
    for (uint32_t mode = 0; mode < 4; ++mode) for (uint32_t cancel = 0; cancel < 2; ++cancel) {
        XrXirDomain *domain=NULL; CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
        XrXirTypeArena *arena=error_fixture_arena(domain);
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {tables[mode], 3, NULL, 65536, 100, 10, &accounting, {NULL, NULL}, {0}};
        config.admission=error_fixture_admission(domain,arena);
        XrXirValue arguments[] = {{XR_XIR_I64, 0, 9}, {XR_XIR_I64, 0, 4}};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, arguments, 2, &call) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_call_poll(call);
        if (mode != 2) {
            CHECK(result.status == XR_XIR_CALL_SUSPENDED && accounting.depth == 3);
            CHECK(xr_xir_call_poll(call).wake == result.wake);
            if (cancel) {
                CHECK(xr_xir_call_cancel(call) == XR_XIR_CALL_CANCELLED);
                CHECK(xr_xir_call_resume(call, result.wake) == XR_XIR_CALL_BAD_STATE);
            } else CHECK(xr_xir_call_resume(call, result.wake) == XR_XIR_CALL_READY);
            result = xr_xir_call_poll(call);
        }
        if (mode == 2) CHECK(result.status == XR_XIR_CALL_DIVIDE_BY_ZERO && result.value.type == XR_XIR_UNIT);
        else if (cancel) CHECK(result.status == XR_XIR_CALL_CANCELLED && result.value.type == XR_XIR_UNIT);
        else if (mode == 3) CHECK(result.status == XR_XIR_CALL_MATCH_FAILURE && result.value.type == XR_XIR_UNIT && xr_xir_fault_match_valid(result.fault));
        else if (mode == 1) CHECK(result.status == XR_XIR_CALL_THROWN && error_fixture_is_code(&result.value,domain,91));
        else CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.type == XR_XIR_I64 && result.value.payload == 4);
        XrXirValue escaped={0};
        if (mode==1 && !cancel) CHECK(xr_xir_call_take_result(call,&escaped)==XR_XIR_CALL_THROWN);
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(accounting.live_bytes == 0 && accounting.allocations == accounting.frees && accounting.depth == 0);
        xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain);
        if (escaped.type) {
            XrXirDomain *reader=NULL; CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
            CHECK(error_fixture_is_code(&escaped,reader,91));
            xr_xir_value_drop(&escaped); xr_xir_domain_drop(reader);
        }
    }
    puts("Native resumable calls, error propagation, cancellation and physical release passed");
    return 0;
}
