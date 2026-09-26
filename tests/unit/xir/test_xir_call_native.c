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
XR_DATA const XrXirCallEntry fixture_calls0_entries[3];
XR_DATA const XrXirCallEntry fixture_calls1_entries[3];
XR_DATA const XrXirCallEntry fixture_calls2_entries[3];
int main(void) {
    const XrXirCallEntry *tables[] = {fixture_calls0_entries, fixture_calls1_entries, fixture_calls2_entries};
    for (uint32_t mode = 0; mode < 3; ++mode) for (uint32_t cancel = 0; cancel < 2; ++cancel) {
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {tables[mode], 3, NULL, 65536, 100, 10, &accounting};
        XrXirScalar arguments[] = {{XR_XIR_I64, 0, 9}, {XR_XIR_I64, 0, 4}};
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
        if (mode == 2) CHECK(result.status == XR_XIR_CALL_OVERFLOW && result.value.type == XR_XIR_UNIT);
        else if (cancel) CHECK(result.status == XR_XIR_CALL_CANCELLED && result.value.type == XR_XIR_UNIT);
        else if (mode == 1) CHECK(result.status == XR_XIR_CALL_THROWN && result.value.payload == 91);
        else CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.type == XR_XIR_I64 && result.value.payload == 4);
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(accounting.live_bytes == 0 && accounting.allocations == accounting.frees && accounting.depth == 0);
    }
    puts("Native resumable calls, error propagation, cancellation and physical release passed");
    return 0;
}
