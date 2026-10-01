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
#include "xir_runtime_allocations.h"
#include "xir_error_fixture.h"
XR_DATA const XrXirCallEntry fixture_calls0_entries[5];
XR_DATA const XrXirCallEntry fixture_calls1_entries[5];
XR_DATA const XrXirCallEntry fixture_calls2_entries[5];
XR_DATA const XrXirCallEntry fixture_calls3_entries[5];
XR_DATA const XrXirCallEntry fixture_calls4_entries[5];
XR_DATA const XrXirCallEntry fixture_calls5_entries[5];
XR_DATA const XrXirCallEntry fixture_calls6_entries[5];
XR_DATA const XrXirCallEntry fixture_calls7_entries[5];
XR_DATA const XrXirCallEntry fixture_calls8_entries[5];
XR_DATA const XrXirCallEntry fixture_calls9_entries[5];
XR_DATA const XrXirCallEntry fixture_calls10_entries[5];
XR_DATA const XrXirCallEntry fixture_calls11_entries[5];
XR_DATA const XrXirCallEntry fixture_calls12_entries[5];
XR_DATA const XrXirCallEntry fixture_calls13_entries[5];

static bool native_invoke_allocation_run(const XrXirCallEntry *entries, uint32_t mode, uint32_t cancel_at) {
    XrXirDomain *domain = NULL; XrXirTypeArena *arena = NULL;
    XrXirCall *call = NULL; XrXirValue owned = {0};
    XrXirCallAccounting accounting = {0}; bool completed = false;
    XrXirCallConfig config = {entries,5,NULL,65536,100,10,&accounting,{NULL,NULL},{0}};
    XrXirValueStatus status = xr_xir_domain_new(65536,&domain);
    if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_OOM); goto done; }
    ErrorFixture fixture; error_fixture_init(&fixture,true);
    XrXirBudget budget = xr_xir_default_budget();
    status = xr_xir_type_arena_new(domain,&fixture.types,&budget,&arena);
    if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_OOM); goto done; }
    config.admission = error_fixture_admission(domain,arena);
    XrXirValue args[] = {{XR_XIR_I64,0,9},{XR_XIR_I64,0,4}};
    XrXirCallStatus admitted = xr_xir_call_new(&config,0,args,2,&call);
    if (admitted != XR_XIR_CALL_READY) { CHECK(admitted == XR_XIR_CALL_OOM); goto done; }
    XrXirCallResult result = xr_xir_call_poll(call);
    uint32_t suspensions = 0;
    while (result.status == XR_XIR_CALL_SUSPENDED) {
        CHECK(++suspensions <= 2);
        if (suspensions == cancel_at) CHECK(xr_xir_call_cancel(call) == XR_XIR_CALL_CANCELLED);
        else CHECK(xr_xir_call_resume(call,result.wake) == XR_XIR_CALL_READY);
        result = xr_xir_call_poll(call);
    }
    if (result.status == XR_XIR_CALL_OOM) goto done;
    if (cancel_at && suspensions == cancel_at) CHECK(result.status == XR_XIR_CALL_CANCELLED);
    else if (mode == 9) {
        CHECK(result.status == XR_XIR_CALL_THROWN);
        CHECK(xr_xir_call_take_result(call,&owned) == XR_XIR_CALL_THROWN);
    } else CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.type == XR_XIR_I64 &&
        result.value.payload == (mode == 10 ? 44 : 77));
    completed = true;
 done:
    CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees && !accounting.depth);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain);
    xr_xir_value_drop(&owned);
    return completed;
}
static void native_invoke_allocation_failures(const XrXirCallEntry *const *tables) {
    const uint32_t modes[] = {5,9,10,11};
    for (uint32_t m = 0; m < 4; ++m) for (uint32_t cancel = 0; cancel < 3; ++cancel) {
        runtime_fail_at = SIZE_MAX; runtime_attempts = 0;
        CHECK(native_invoke_allocation_run(tables[modes[m]],modes[m],cancel));
        size_t sites = runtime_attempts; CHECK(sites && !runtime_live && !runtime_bytes);
        for (size_t i = 0; i < sites; ++i) {
            runtime_fail_at = i; runtime_attempts = 0;
            CHECK(!native_invoke_allocation_run(tables[modes[m]],modes[m],cancel));
            CHECK(!runtime_live && !runtime_bytes);
        }
        runtime_fail_at = SIZE_MAX;
        printf("Native invoke physical release: mode=%u cancel=%u allocation failure sites=%zu\n",modes[m],cancel,sites);
    }
}

int main(void) {
    const XrXirCallEntry *tables[] = {fixture_calls0_entries, fixture_calls1_entries, fixture_calls2_entries, fixture_calls3_entries, fixture_calls4_entries, fixture_calls5_entries, fixture_calls6_entries, fixture_calls7_entries, fixture_calls8_entries, fixture_calls9_entries, fixture_calls10_entries, fixture_calls11_entries, fixture_calls12_entries, fixture_calls13_entries};
    for (uint32_t mode = 0; mode < 14; ++mode) for (uint32_t cancel = 0; cancel < 2; ++cancel) {
        uint32_t kind = mode >= 12 ? mode - 12 : mode >= 10 ? mode - 10 : mode % 4;
        XrXirDomain *domain=NULL; CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
        XrXirTypeArena *arena=error_fixture_arena(domain);
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {tables[mode], 5, NULL, 65536, 100, 10, &accounting, {NULL, NULL}, {0}};
        config.admission=error_fixture_admission(domain,arena);
        XrXirValue arguments[] = {{XR_XIR_I64, 0, 9}, {XR_XIR_I64, 0, 4}};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, arguments, 2, &call) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_call_poll(call);
        if (kind != 2) {
            CHECK(result.status == XR_XIR_CALL_SUSPENDED && accounting.depth == (mode >= 12 ? 2u : 3u));
            CHECK(xr_xir_call_poll(call).wake == result.wake);
            if (cancel) {
                CHECK(xr_xir_call_cancel(call) == XR_XIR_CALL_CANCELLED);
                CHECK(xr_xir_call_resume(call, result.wake) == XR_XIR_CALL_BAD_STATE);
            } else CHECK(xr_xir_call_resume(call, result.wake) == XR_XIR_CALL_READY);
            result = xr_xir_call_poll(call);
        }
        if (mode == 9 && !cancel) {
            CHECK(result.status == XR_XIR_CALL_SUSPENDED && accounting.depth == 1);
            CHECK(xr_xir_call_resume(call,result.wake) == XR_XIR_CALL_READY);
            result = xr_xir_call_poll(call);
        }
        if (kind == 2) CHECK(result.status == XR_XIR_CALL_DIVIDE_BY_ZERO && result.value.type == XR_XIR_UNIT);
        else if (cancel) CHECK(result.status == XR_XIR_CALL_CANCELLED && result.value.type == XR_XIR_UNIT);
        else if (kind == 3) CHECK(result.status == XR_XIR_CALL_MATCH_FAILURE && result.value.type == XR_XIR_UNIT && xr_xir_fault_match_valid(result.panic.detail));
        else if (mode == 1 || mode == 9) CHECK(result.status == XR_XIR_CALL_THROWN && error_fixture_is_code(&result.value,domain,91));
        else CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.type == XR_XIR_I64 && result.value.payload == (mode == 5 || mode == 11 || mode == 13 ? 77 : mode == 10 ? 44 : 4));
        XrXirValue escaped={0};
        if ((mode==1 || mode==9) && !cancel) CHECK(xr_xir_call_take_result(call,&escaped)==XR_XIR_CALL_THROWN);
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(accounting.live_bytes == 0 && accounting.allocations == accounting.frees && accounting.depth == 0);
        xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain);
        if (escaped.type) {
            XrXirDomain *reader=NULL; CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
            CHECK(error_fixture_is_code(&escaped,reader,91));
            xr_xir_value_drop(&escaped); xr_xir_domain_drop(reader);
        }
    }
    CHECK(!runtime_live && !runtime_bytes);
    native_invoke_allocation_failures(tables);
    puts("Native resumable calls, error propagation, cancellation and physical release passed");
    return 0;
}
