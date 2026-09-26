/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_output_cases.h - Independent byte expectations and sink failure cleanup
 *
 * KEY CONCEPT:
 *   Embedded NUL and Unicode are compared by length, never as C strings.
 */
#ifndef XIR_OUTPUT_CASES_H
#define XIR_OUTPUT_CASES_H
#include "xir/xxir_output.h"
typedef struct OutputProbe { uint32_t calls, mode; XrXirCall *call; } OutputProbe;
static bool output_bytes(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    OutputProbe *probe = context;
    static const char *const expected[] = {"\n", "-9223372036854775808 A\0\xE4\xB8\xAD\n", "A\0\xE4\xB8\xAD", "true\n"};
    static const size_t sizes[] = {1, 27, 5, 5};
    CHECK(probe->calls < 4);
    CHECK(stream == (probe->calls == 2 ? XR_XIR_STDERR : XR_XIR_STDOUT));
    CHECK(length == sizes[probe->calls] && !memcmp(bytes, expected[probe->calls], length));
    CHECK(xr_xir_call_poll(probe->call).status == XR_XIR_CALL_BUSY);
    ++probe->calls;
    if (probe->mode == 3) CHECK(xr_xir_call_cancel(probe->call) == XR_XIR_CALL_CANCELLED);
    return probe->mode != 2;
}
static void output_cases(const XrXirCallEntry *entry) {
    for (uint32_t mode = 0; mode < 5; ++mode) {
        XrXirDomain *domain = NULL;
        CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
        size_t baseline = xr_xir_domain_stats(domain).live_bytes;
        XrXirValue args[2] = {{XR_XIR_I64, 0, INT64_MIN}, {0}};
        CHECK(xr_xir_string_new(domain, "A\0\xE4\xB8\xAD", 5, &args[1]) == XR_XIR_VALUE_OK);
        OutputProbe probe = {0, mode, NULL};
        XrXirOutputSink sink = {output_bytes, &probe, mode == 1 ? 26 : 65536};
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {entry, 1, NULL, 65536, 100, 10, &accounting,
            {mode == 4 ? NULL : xr_xir_output_render, &sink}};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, args, 2, &call) == XR_XIR_CALL_READY);
        probe.call = call;
        xr_xir_value_drop(&args[1]);
        XrXirCallResult result = xr_xir_call_poll(call);
        CHECK(result.status == (mode == 0 ? XR_XIR_CALL_RETURNED : mode == 3 ?
            XR_XIR_CALL_CANCELLED : XR_XIR_CALL_OUTPUT_ERROR));
        CHECK(probe.calls == (mode == 0 ? 4u : mode == 4 ? 0u : 1u));
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees);
        CHECK(xr_xir_domain_stats(domain).live_bytes == baseline);
        xr_xir_domain_drop(domain);
    }
}
#endif
