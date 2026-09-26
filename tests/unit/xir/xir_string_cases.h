/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_string_cases.h - Independent string execution and physical release expectations
 *
 * KEY CONCEPT:
 *   Assert independent expected values and explicit ownership at every exit.
 */

#ifndef XIR_STRING_CASES_H
#define XIR_STRING_CASES_H
#include "xir/xxir_call.h"
static const char string_left[] = "A\0\xE4\xB8\xAD";
static const char string_right[] = "\xF0\x9F\x98\x80!";
static const char string_expected[] = "A\0\xE4\xB8\xAD\xF0\x9F\x98\x80!";
typedef struct StringOutput { uint32_t calls, mode; XrXirCall *call; } StringOutput;
static void string_bytes(const XrXirValue *value, const char *expected, size_t size) {
    const char *bytes = NULL;
    size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == size && !memcmp(bytes, expected, size));
}
static bool string_output(void *context, const XrXirOutputGroup *group) {
    CHECK(group && !group->line && group->count == 1);
    XrXirOutputStream stream = group->stream;
    const XrXirValue *value = &group->values[0];
    StringOutput *output = context;
    string_bytes(value, string_expected, sizeof(string_expected) - 1);
    CHECK(stream == (output->mode == 5 || output->mode == 6 || output->calls ? XR_XIR_STDOUT : XR_XIR_STDERR));
    ++output->calls;
    XrXirValue owned = {0};
    CHECK(xr_xir_call_poll(output->call).status == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_take_result(output->call, &owned) == XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_free(output->call) == XR_XIR_CALL_BUSY);
    if (output->mode == 6 && output->calls == 3)
        CHECK(xr_xir_call_cancel(output->call) == XR_XIR_CALL_CANCELLED);
    return output->mode != 3;
}
static XrXirValue string_cases(const XrXirCallEntry *entries, uint32_t variant, uint32_t mode) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(mode == 8 ? 240 : 65536, &domain) == XR_XIR_VALUE_OK);
    XrXirDomainStats baseline = xr_xir_domain_stats(domain);
    XrXirValue arguments[2] = {{0}, {0}};
    CHECK(xr_xir_string_new(domain, string_left, sizeof(string_left) - 1, &arguments[0]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, string_right, sizeof(string_right) - 1, &arguments[1]) == XR_XIR_VALUE_OK);
    XrXirCallAccounting accounting = {0};
    StringOutput output = {0, mode, NULL};
    XrXirCallConfig config = {entries, 3, NULL, 65536, mode == 5 ? 15 : 100, 10,
        &accounting, {mode == 4 ? NULL : string_output, &output}};
    XrXirCall *call = NULL;
    CHECK(xr_xir_call_new(&config, mode == 5 || mode == 6 ? 2 : 0, arguments, 2, &call) == XR_XIR_CALL_READY);
    output.call = call;
    xr_xir_value_drop(&arguments[0]);
    xr_xir_value_drop(&arguments[1]);
    XrXirValue owned = {0};
    CHECK(xr_xir_call_take_result(call, &owned) == XR_XIR_CALL_BAD_STATE);
    XrXirCallResult result = xr_xir_call_poll(call);
    if (mode == 3 || mode == 4) CHECK(result.status == XR_XIR_CALL_OUTPUT_ERROR);
    else if (mode == 5 || mode == 8) CHECK(result.status == XR_XIR_CALL_LIMIT);
    else if (mode == 6) CHECK(result.status == XR_XIR_CALL_CANCELLED && output.calls == 3);
    else {
        CHECK(result.status == XR_XIR_CALL_SUSPENDED && output.calls == 1);
        CHECK(xr_xir_call_take_result(call, &owned) == XR_XIR_CALL_BAD_STATE);
        if (mode == 1) CHECK(xr_xir_call_cancel(call) == XR_XIR_CALL_CANCELLED);
        else if (mode != 2) {
            CHECK(xr_xir_call_resume(call, result.wake) == XR_XIR_CALL_READY);
            result = xr_xir_call_poll(call);
            if (variant) CHECK(result.status == XR_XIR_CALL_THROWN && result.value.payload == 91);
            else {
                CHECK(result.status == XR_XIR_CALL_RETURNED && output.calls == 2);
                string_bytes(&result.value, string_expected, sizeof(string_expected) - 1);
            }
            if (mode != 7) {
                CHECK(xr_xir_call_take_result(call, &owned) == result.status);
                CHECK(xr_xir_call_poll(call).status == XR_XIR_CALL_CONSUMED);
                XrXirValue second = {0};
                CHECK(xr_xir_call_take_result(call, &second) == XR_XIR_CALL_BAD_STATE);
            }
        }
    }
    CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
    CHECK(accounting.live_bytes == 0 && accounting.allocations == accounting.frees && !accounting.depth);
    if (owned.type != XR_XIR_STRING) {
        XrXirDomainStats stats = xr_xir_domain_stats(domain);
        CHECK(stats.live_bytes == baseline.live_bytes && stats.allocations == stats.frees + 1);
    } else {
        string_bytes(&owned, string_expected, sizeof(string_expected) - 1);
        CHECK(xr_xir_domain_stats(domain).live_bytes > baseline.live_bytes);
    }
    xr_xir_domain_drop(domain);
    return owned;
}
#endif
