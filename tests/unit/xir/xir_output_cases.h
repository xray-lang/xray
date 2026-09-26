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
static bool write_bytes(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    OutputProbe *probe = context;
    CHECK(stream == (probe->mode == 5 || probe->mode == 6 ? XR_XIR_STDOUT : XR_XIR_STDERR));
    CHECK(length == 5 && !memcmp(bytes, "A\0\xE4\xB8\xAD", 5));
    CHECK(xr_xir_call_poll(probe->call).status == XR_XIR_CALL_BUSY);
    ++probe->calls;
    if (probe->mode == 3) CHECK(xr_xir_call_cancel(probe->call) == XR_XIR_CALL_CANCELLED);
    return probe->mode != 1 && !(probe->mode == 5 && probe->calls == 2) &&
        !(probe->mode == 6 && probe->calls == 1);
}
static void write_cases(const XrXirCallEntry *entries) {
    for (uint32_t mode = 0; mode < 7; ++mode) {
        XrXirDomain *domain = NULL;
        CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
        size_t baseline = xr_xir_domain_stats(domain).live_bytes;
        XrXirValue argument = {0};
        CHECK(xr_xir_string_new(domain, "A\0\xE4\xB8\xAD", 5, &argument) == XR_XIR_VALUE_OK);
        OutputProbe probe = {0, mode, NULL};
        XrXirOutputSink sink = {write_bytes, &probe, mode == 4 ? 4 : 65536};
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {entries, 3, NULL, 65536, 100, 10, &accounting,
            {mode == 2 ? NULL : xr_xir_output_render, &sink}};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, mode >= 5 ? 2 : 1, &argument, 1, &call) == XR_XIR_CALL_READY);
        probe.call = call; xr_xir_value_drop(&argument);
        XrXirCallResult result = xr_xir_call_poll(call);
        if (mode == 2) CHECK(result.status == XR_XIR_CALL_OUTPUT_ERROR);
        else if (mode == 3) CHECK(result.status == XR_XIR_CALL_CANCELLED);
        else {
            CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.type == XR_XIR_BOOL);
            CHECK(result.value.payload == (mode == 0 || mode == 6 ? 1 : 0));
        }
        CHECK(probe.calls == (mode == 2 || mode == 4 ? 0u : mode >= 5 ? 2u : 1u));
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees);
        CHECK(xr_xir_domain_stats(domain).live_bytes == baseline);
        xr_xir_domain_drop(domain);
    }
}
static XrXirAction malformed_write(XrXirCallView *view) {
    uint32_t mode = *(const uint32_t *) view->environment;
    XrXirValue *argument = view->state;
    *argument = view->arguments[0];
    XrXirAction action = {XR_XIR_ACTION_WRITE_STREAM, 2, argument, 1, {0}};
    switch (mode) {
    case 0: action.argument_count = 0; break;
    case 1: action.argument_count = 2; break;
    case 2: action.arguments = NULL; break;
    case 3: action.callee = XR_XIR_OUTPUT_LINE; break;
    case 4: action.value.reserved = 1; break;
    case 5: *argument = (XrXirValue) {XR_XIR_I64, 0, 1}; break;
    case 6: argument->reserved = 1; break;
    case 7: action.argument_count = 65537; break;
    case 8: action.callee = 0; break;
    default: action.callee = 4; break;
    }
    return action;
}
static bool unexpected_write(void *context, const XrXirOutputGroup *group) {
    (void) context; (void) group; CHECK(false); return false;
}
static void write_action_admission(void) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirValue argument = {0};
    CHECK(xr_xir_string_new(domain, "x", 1, &argument) == XR_XIR_VALUE_OK);
    size_t baseline = xr_xir_domain_stats(domain).live_bytes;
    XrXirType type = XR_XIR_STRING;
    for (uint32_t mode = 0; mode < 10; ++mode) {
        XrXirCallEntry entry = {XR_XIR_CALL_ABI_VERSION, &type, 1, XR_XIR_BOOL,
            sizeof(XrXirValue), malformed_write, NULL, &mode};
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {&entry, 1, NULL, 65536, 100, 10, &accounting, {unexpected_write, NULL}};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, &argument, 1, &call) == XR_XIR_CALL_READY);
        CHECK(xr_xir_call_poll(call).status == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees);
        CHECK(xr_xir_domain_stats(domain).live_bytes == baseline);
    }
    xr_xir_value_drop(&argument); xr_xir_domain_drop(domain);
}

#endif
