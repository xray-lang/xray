/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_panic_carrier.c - Independent owned message and failure-channel witnesses
 */
#include "xir/xxir_program.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "base/xcompile_resources.c"

static const char expected_bytes[] = "a\0\xe4\xb8\xad" "z";
static void expect_bytes(const XrXirValue *value, const char *expected, size_t size) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == size && (!size || !memcmp(bytes, expected, size)));
}
static XrXirAction control(XrXirActionKind kind) {
    return (XrXirAction){kind, 0, NULL, 0, {0}, {0}, 0};
}
static void physical_empty(void) { CHECK(!runtime_live && !runtime_bytes); }
typedef struct CarrierWitness {
    XrXirDomain *domain;
    XrXirCallResult held;
    XrXirValue info;
    uint32_t mode, releases, child_releases, cleanup_calls, callbacks;
} CarrierWitness;
typedef struct CarrierFrame { XrXirValue message; int64_t info_payload; uint32_t phase; } CarrierFrame;
static void carrier_release(XrXirCallView *view, XrXirCallStatus reason) {
    CarrierWitness *w = view->instance; CarrierFrame *frame = view->state;
    CHECK(reason != XR_XIR_CALL_READY && reason != XR_XIR_CALL_SUSPENDED);
    ++w->releases;
    if ((uintptr_t)view->environment == 1) ++w->child_releases;
    xr_xir_value_drop(&frame->message);
}
static XrXirAction carrier_child(XrXirCallView *view) {
    CarrierWitness *w = view->instance; CarrierFrame *frame = view->state;
    ++w->callbacks;
    XrXirValueStatus status = xr_xir_string_new(w->domain, expected_bytes, sizeof(expected_bytes), &frame->message);
    if (status != XR_XIR_VALUE_OK) return xr_xir_call_fault(XR_XIR_RUN_OUT_OF_MEMORY);
    XrXirAction action = xr_xir_call_assertion(&frame->message);
    if (w->mode == 5) { action.panic.detail.reserved = 1; }
    if (w->mode == 6) { action.panic.message = (XrXirValue){0}; }
    if (w->mode == 7) { action.panic.detail.code = XR_XIR_PANIC_BOUNDS; action.panic.detail.index = -1; }
    if (w->mode == 8) { action.value.payload = XR_XIR_CALL_OOM; }
    if (w->mode == 9) { action.kind = XR_XIR_ACTION_RETURN; action.value = (XrXirValue){0}; }
    if (w->mode == 10) { action.flags = XR_XIR_ACTION_PROTECTED; }
    if (w->mode == 11) { CHECK(xr_xir_call_request_cancel(view->activation) == XR_XIR_CALL_CANCEL_REQUESTED); }
    return action;
}
static XrXirAction carrier_cleanup(XrXirCallView *view) {
    CarrierWitness *w = view->instance; ++w->cleanup_calls;
    if (w->mode == 12) {
        CarrierFrame *frame = view->state;
        CHECK(xr_xir_string_new(w->domain, expected_bytes, sizeof(expected_bytes), &frame->message) == XR_XIR_VALUE_OK);
        return xr_xir_call_assertion(&frame->message);
    }
    return control(XR_XIR_ACTION_RETURN);
}
static XrXirAction carrier_parent(XrXirCallView *view) {
    CarrierWitness *w = view->instance; CarrierFrame *frame = view->state;
    if (view->phase == XR_XIR_CALL_EXIT) {
        if (view->exit.status == XR_XIR_CALL_RETURNED || view->exit.status == XR_XIR_CALL_CANCELLED) {
            CHECK(xr_xir_panic_empty(&view->exit.panic));
            return control(XR_XIR_ACTION_EXIT_DONE);
        }
        CHECK(view->exit.status == XR_XIR_CALL_ASSERTION && !view->exit.value.type);
        expect_bytes(&view->exit.panic.message, expected_bytes, sizeof(expected_bytes));
        if (w->mode == 3) CHECK(xr_xir_call_request_cancel(view->activation) == XR_XIR_CALL_CANCEL_REQUESTED);
        if (frame->phase++ == 2 || w->mode == 12)
            return (XrXirAction){XR_XIR_ACTION_CALL, 2, NULL, 0, {0}, {0}, XR_XIR_ACTION_CLEANUP};
        return control(XR_XIR_ACTION_EXIT_DONE);
    }
    if (!frame->phase++)
        return (XrXirAction){XR_XIR_ACTION_CALL, 1, NULL, 0, {0}, {0},
            w->mode == 4 || w->mode >= 5 ? 0 : XR_XIR_ACTION_PROTECTED};
    CHECK(view->inbox.status == XR_XIR_CALL_ASSERTION && !view->inbox.value.type && w->child_releases == 1);
    expect_bytes(&view->inbox.panic.message, expected_bytes, sizeof(expected_bytes));
    if (w->mode >= 2 && frame->phase == 2)
        return (XrXirAction){XR_XIR_ACTION_LEAVE, 0, NULL, 0,
            {XR_XIR_I64, 0, view->inbox.status}, view->inbox.panic, XR_XIR_ACTION_LEAVE_PANIC};
    if (xr_xir_call_result_copy(&view->inbox, &w->held) != XR_XIR_VALUE_OK)
        return xr_xir_call_fault(XR_XIR_RUN_STEP_LIMIT);
    if (xr_xir_panic_info_new(w->domain, &view->inbox.panic, &w->info) != XR_XIR_VALUE_OK)
        return xr_xir_call_fault(XR_XIR_RUN_OUT_OF_MEMORY);
    return control(XR_XIR_ACTION_RETURN);
}
static XrXirCallStatus carrier_run(uint32_t mode) {
    CarrierWitness witness = {0}; witness.mode = mode;
    XrXirValueStatus created = xr_xir_domain_new(1048576, &witness.domain);
    if (created != XR_XIR_VALUE_OK) { physical_empty(); return XR_XIR_CALL_OOM; }
    const XrXirCallEntry entries[] = {
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT, sizeof(CarrierFrame), carrier_parent,
            carrier_release, NULL, (mode >= 2 && mode <= 4) || mode == 12 ? XR_XIR_ENTRY_EXIT : 0, 0},
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT, sizeof(CarrierFrame), carrier_child,
            carrier_release, (const void *)(uintptr_t)1, 0, 0},
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT, sizeof(CarrierFrame), carrier_cleanup,
            carrier_release, NULL, 0, 1}
    };
    XrXirCallAccounting accounting = {0};
    XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.entries = entries; config.entry_count = (mode >= 2 && mode <= 4) || mode == 12 ? 3u : 2u; config.instance = &witness; config.byte_limit = 1048576; config.poll_limit = 100; config.depth_limit = 8; config.accounting = &accounting; config.output = (XrXirOutputProvider) {0}; config.admission = (XrXirValueAdmission) {0};
    XrXirCall *call = NULL;
    XrXirCallStatus status = xr_xir_call_new(&config, 0, NULL, 0, &call);
    if (status == XR_XIR_CALL_READY) {
        XrXirCallResult result = xr_xir_call_poll_bounded(call, UINT64_MAX);
        status = result.status;
        CHECK(xr_xir_call_result_valid(&result) && result.value.type == XR_XIR_UNIT);
        size_t attempts = runtime_attempts;
        CHECK(xr_xir_call_poll_bounded(call, UINT64_MAX).status == status && runtime_attempts == attempts);
        if (status == XR_XIR_CALL_ASSERTION) {
            CHECK(mode == 4);
            CHECK(xr_xir_call_result_copy(&result, &witness.held) == XR_XIR_VALUE_OK);
            expect_bytes(&witness.held.panic.message, expected_bytes, sizeof(expected_bytes));
        }
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(!accounting.live_bytes && !accounting.depth && accounting.allocations == accounting.frees);
        if (status == XR_XIR_CALL_RETURNED) CHECK(witness.releases == (mode == 2 ? 3u : 2u));
    }
    xr_xir_domain_drop(witness.domain);
    if (witness.held.status == XR_XIR_CALL_ASSERTION)
        expect_bytes(&witness.held.panic.message, expected_bytes, sizeof(expected_bytes));
    if (witness.info.type) {
        XrXirValue message = {0}; XrXirFaultDetail detail = {0};
        CHECK(xr_xir_panic_info_detail(&witness.info, &detail) && detail.code == 445);
        CHECK(xr_xir_panic_info_message(&witness.info, &message) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&witness.info);
        expect_bytes(&message, expected_bytes, sizeof(expected_bytes));
        xr_xir_value_drop(&message);
    }
    xr_xir_call_result_drop(&witness.held);
    physical_empty();
    return status;
}
#include "xir_panic_carrier_value_cases.inc.c"
#include "xir_panic_carrier_instance_cases.inc.c"
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "fatal-driver")) return (int)carrier_run(12);
    if (argc == 2 && (!strcmp(argv[1], "fatal") || !strcmp(argv[1], "fatal-lines") || !strcmp(argv[1], "fatal-empty"))) {
        XrXirDomain *domain = NULL; XrXirValue message = {0};
        CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
        static const char lines[] = {'l', '\n', 'r', '\r', '\0', 't', '\t'};
        bool empty = !strcmp(argv[1], "fatal-empty");
        const char *bytes = empty ? NULL : !strcmp(argv[1], "fatal") ? expected_bytes : lines;
        size_t length = empty ? 0 : !strcmp(argv[1], "fatal") ? sizeof(expected_bytes) : sizeof(lines);
        CHECK(xr_xir_string_new(domain, bytes, length, &message) == XR_XIR_VALUE_OK);
        XrXirPanicPayload panic = {{445, 0, 0, 0}, message};
        XrErrorCoreMessageView view = {0};
        CHECK(xr_xir_panic_message_borrow(&panic, NULL, 0, &view));
        xr_error_core_defer_throw_abort(443, view, &view);
    }
    CHECK(argc == 1);
    value_cases(); abi_cases(); instance_cases(); handler_cases();
    const XrXirCallStatus expected[] = {XR_XIR_CALL_RETURNED, XR_XIR_CALL_RETURNED, XR_XIR_CALL_RETURNED,
        XR_XIR_CALL_CANCELLED, XR_XIR_CALL_ASSERTION, XR_XIR_CALL_BAD_STATE, XR_XIR_CALL_BAD_STATE,
        XR_XIR_CALL_BAD_STATE, XR_XIR_CALL_BAD_STATE, XR_XIR_CALL_BAD_STATE, XR_XIR_CALL_BAD_STATE, XR_XIR_CALL_CANCELLED};
    for (uint32_t mode = 0; mode < sizeof(expected)/sizeof(*expected); ++mode) CHECK(carrier_run(mode) == expected[mode]);
    runtime_attempts = 0; CHECK(carrier_run(2) == XR_XIR_CALL_RETURNED);
    size_t sites = runtime_attempts;
    for (size_t point = 0; point < sites; ++point) {
        runtime_attempts = 0; runtime_fail_at = point;
        XrXirCallStatus status = carrier_run(2);
        CHECK(status == XR_XIR_CALL_OOM || status == XR_XIR_CALL_LIMIT);
        runtime_fail_at = SIZE_MAX;
    }
    physical_empty();
    printf("Carrier native ownership, cancellation, malformed payload and OOM physical gates PASS; runtime sites=%zu\n", sites);
    return 0;
}
