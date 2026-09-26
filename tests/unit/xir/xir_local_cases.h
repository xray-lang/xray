/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_local_cases.h - Independent local ownership and termination witnesses
 *
 * KEY CONCEPT:
 *   Both engines must satisfy byte expectations and physical release accounting.
 */
#ifndef XIR_LOCAL_CASES_H
#define XIR_LOCAL_CASES_H
#include "xir/xxir_call.h"
static void numeric_cleanup(const XrXirCallEntry *entry) {
    for (unsigned mode = 0; mode < 2; ++mode) {
        XrXirDomain *domain = NULL;
        CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
        XrXirDomainStats baseline = xr_xir_domain_stats(domain);
        XrXirValue args[] = {{0}, {0}, {XR_XIR_I64, 0, mode ? 3 : 0}};
        CHECK(xr_xir_string_new(domain, "a", 1, &args[0]) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_new(domain, "b", 1, &args[1]) == XR_XIR_VALUE_OK);
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {entry, 1, NULL, 65536, 100, 4, &accounting, {NULL, NULL}};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, args, 3, &call) == XR_XIR_CALL_READY);
        xr_xir_value_drop(&args[0]); xr_xir_value_drop(&args[1]);
        XrXirCallResult result = xr_xir_call_poll(call);
        CHECK(result.status == (mode ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_DIVIDE_BY_ZERO));
        CHECK(result.value.payload == (mode ? 2 : 0));
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(accounting.live_bytes == 0 && accounting.allocations == accounting.frees);
        XrXirDomainStats stats = xr_xir_domain_stats(domain);
        CHECK(stats.live_bytes == baseline.live_bytes && stats.allocations == stats.frees + 1);
        xr_xir_domain_drop(domain);
    }
}
static void local_cases(const XrXirCallEntry *entry) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        XrXirDomain *domain = NULL;
        CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
        XrXirDomainStats baseline = xr_xir_domain_stats(domain);
        XrXirValue arguments[] = {{0}, {0}, {XR_XIR_BOOL, 0, mode != 0}}, result = {0};
        CHECK(xr_xir_string_new(domain, "a", 1, &arguments[0]) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_new(domain, "b", 1, &arguments[1]) == XR_XIR_VALUE_OK);
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {entry, 1, NULL, 65536, mode == 3 ? 3 : 100, 4, &accounting, {NULL, NULL}};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, arguments, 3, &call) == XR_XIR_CALL_READY);
        XrXirCallResult outcome = xr_xir_call_poll(call);
        xr_xir_value_drop(&arguments[0]); xr_xir_value_drop(&arguments[1]);
        if (mode == 3) CHECK(outcome.status == XR_XIR_CALL_LIMIT);
        else {
            CHECK(outcome.status == XR_XIR_CALL_SUSPENDED);
            if (mode == 2) CHECK(xr_xir_call_cancel(call) == XR_XIR_CALL_CANCELLED);
            else {
                CHECK(xr_xir_call_resume(call, outcome.wake) == XR_XIR_CALL_READY);
                CHECK(xr_xir_call_poll(call).status == XR_XIR_CALL_RETURNED);
                CHECK(xr_xir_call_take_result(call, &result) == XR_XIR_CALL_RETURNED);
            }
        }
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees);
        if (mode < 2) {
            const char *bytes = NULL; size_t length = 0;
            CHECK(xr_xir_string_view(&result, &bytes, &length) && length == 2 && !memcmp(bytes, mode ? "ab" : "aa", 2));
            xr_xir_value_drop(&result);
        }
        XrXirDomainStats stats = xr_xir_domain_stats(domain);
        CHECK(stats.live_bytes == baseline.live_bytes && stats.allocations == stats.frees + 1);
        xr_xir_domain_drop(domain);
    }
}
#endif
