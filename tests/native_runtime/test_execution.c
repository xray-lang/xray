/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_execution.c - Real leaf recovery and nested synchronous binding
 *
 * KEY CONCEPT:
 *   Controlled host callbacks test the common boundary, independently of VM
 *   dispatch. No standalone global is supplied to satisfy this executable.
 */
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_values.h"
#include "xray_native_values.h"
#include "xray_native_place.h"
#include "xray_native_state.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#else
#include <unistd.h>
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
typedef struct Context {
    XrtLocationOwner *owner;
    XrtExecutionFailure failure;
    unsigned visits, after;
    uint32_t panic_code, source_id;
    bool fail_inner;
    XrtExecutionStatus inner_status;
    struct Context *inner;
} Context;

static void normal_root(void *data) { ++((Context *)data)->visits; }
static void output_root(void *data) {
    Context *context = data;
    ++context->visits;
    xrt_print_i64(17, 1);
    ++context->after;
}
static void panic_root(void *data) {
    Context *context = data;
    ++context->visits;
    xrt_arithmetic_panic(context->panic_code, context->source_id);
}
static void string_root(void *data) {
    Context *context = data;
    ++context->visits;
    xrt_print_string("embedded\0text", 13, 1);
    ++context->after;
}
static void nested_root(void *data) {
    Context *context = data;
    context->inner_status = xrt_run_embedded(context->inner->owner, &context->inner->failure,
        context->inner->panic_code ? panic_root : context->fail_inner ? output_root : normal_root,
        context->inner);
    if (context->panic_code) panic_root(context);
    else output_root(context);
}

static XrtLocationOwner *locations(const char *path) {
    XrtSourceLocation location = {path, strlen(path), 7, 9, 8, 11};
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(&location, 1, &owner) == XRT_LOCATION_OK);
    return owner;
}

#if defined(XR_OS_WINDOWS)
typedef HANDLE SavedOutput;
static SavedOutput disable_output(void) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    CHECK(SetStdHandle(STD_OUTPUT_HANDLE, INVALID_HANDLE_VALUE));
    return output;
}
static void restore_output(SavedOutput output) { CHECK(SetStdHandle(STD_OUTPUT_HANDLE, output)); }
#else
typedef int SavedOutput;
static SavedOutput disable_output(void) {
    int output = dup(STDOUT_FILENO);
    CHECK(output >= 0 && close(STDOUT_FILENO) == 0);
    return output;
}
static void restore_output(SavedOutput output) {
    CHECK(dup2(output, STDOUT_FILENO) >= 0 && close(output) == 0);
    clearerr(stdout);
}
#endif

typedef struct ManagedCall {
    XrtValueRoot *owner;
    XrtLocationOwner *locations;
    XrtManagedRoot *outer;
    XrtManagedHeader *escaped;
    unsigned mode, after;
} ManagedCall;
static void managed_require(XrtManagedStatus status) {
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status,1);
}
static void managed_root(void *context) {
    ManagedCall *call = context;
    XrtManagedRoot *root = xrt_native_managed_root(1);
    CHECK(root && root != call->outer && xrt_value_root_current(1) == call->owner);
    XrtManagedFrame *frame = NULL; managed_require(xrt_managed_frame_enter(root,3,&frame));
    xrt_native_string_create(frame,0,"ab\0\xc3\xa9",5,1);
    XrtManagedHeader *text = NULL; managed_require(xrt_managed_slot_borrow(frame,0,&text));
    xrt_native_string_concat_create(frame,1,text,text,1);
    XrtManagedHeader *joined = NULL; managed_require(xrt_managed_slot_borrow(frame,1,&joined));
    const XrtStringBacking *bytes = (const XrtStringBacking *)joined;
    CHECK(bytes->length == 10 && bytes->runes == 8 && !memcmp(bytes->bytes,"ab\0\xc3\xa9" "ab\0\xc3\xa9",10));
    managed_require(xrt_place_copy_replace((XrtPlace){.frame=frame,.slot=2,.nulls=XRT_PLACE_NONNULL},
        (XrtPlace){.frame=frame,.slot=1,.nulls=XRT_PLACE_NONNULL},NULL));
    if (call->mode == 1) xrt_native_string_create(frame,0,"\xff",1,1);
    if (call->mode == 2) xrt_arithmetic_panic(422,1);
    if (call->mode == 3) managed_require(xrt_managed_slot_take(frame,2,&call->escaped));
    if (call->mode == 4) {
        XrtValueRoot inner_owner = {0}; XrtExecutionFailure failure = {0};
        ManagedCall inner = {.owner=&inner_owner,.locations=call->locations,.outer=root};
        CHECK(xrt_run_embedded_owned(call->locations,&failure,managed_root,&inner,&inner_owner) == XRT_EXECUTION_COMPLETED);
        CHECK(inner.after == 1 && xrt_value_root_empty(&inner_owner) && xrt_native_managed_root(1) == root);
    }
    managed_require(xrt_managed_frame_leave(root,frame)); ++call->after;
}
static void managed_scopes(void) {
    XrtLocationOwner *map = locations("fixed-execution.xr");
    for (unsigned mode = 0; mode < 5; ++mode) {
        XrtValueRoot owner = {0}; XrtExecutionFailure failure = {0};
        ManagedCall call = {.owner=&owner,.locations=map,.mode=mode};
        XrtExecutionStatus status = xrt_run_embedded_owned(map,&failure,managed_root,&call,&owner);
        CHECK(xrt_value_root_empty(&owner) && !xrt_value_root_active());
        if (mode == 1) CHECK(status == XRT_EXECUTION_FAILURE && !call.after && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_BAD_UTF8 && failure.detail.value.source_id == 1);
        else if (mode == 2) CHECK(status == XRT_EXECUTION_FAILURE && !call.after &&
            failure.kind == XRT_FAILURE_TYPED_PANIC && failure.detail.panic.code == 422);
        else CHECK(status == XRT_EXECUTION_COMPLETED && call.after == 1);
        if (call.escaped) {
            const XrtStringBacking *value = (const XrtStringBacking *)call.escaped;
            CHECK(value->length == 10 && !memcmp(value->bytes,"ab\0\xc3\xa9" "ab\0\xc3\xa9",10));
            XrtManagedHeader *retired = call.escaped; call.escaped = NULL; xrt_managed_drop(retired);
        }
    }
    for (uint64_t limit = 1; limit <= 16384; limit *= 4) {
        XrtValueRoot owner = {.byte_limit=limit}; XrtExecutionFailure failure = {0};
        ManagedCall call = {.owner=&owner,.locations=map};
        XrtExecutionStatus status = xrt_run_embedded_owned(map,&failure,managed_root,&call,&owner);
        CHECK(xrt_value_root_empty(&owner) && owner.byte_limit == limit);
        if (status == XRT_EXECUTION_FAILURE) CHECK(!call.after && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_MEMORY_LIMIT);
        else CHECK(status == XRT_EXECUTION_COMPLETED && call.after == 1);
    }
#if defined(XR_NATIVE_VALUE_ALLOCATIONS)
    size_t allowance = 0;
    for (; allowance < 64; ++allowance) {
        XrtValueRoot owner = {0}; XrtBillingOwner *billing = NULL; XrtExecutionFailure failure = {0};
        CHECK(xrt_native_values_prepare(&owner,&billing) == XRT_MANAGED_OK && xrt_billing_owner_retain(billing));
        xrt_billing_owner_fail_after(billing,allowance);
        ManagedCall call = {.owner=&owner,.locations=map};
        XrtExecutionStatus status = xrt_run_embedded_owned(map,&failure,managed_root,&call,&owner);
        CHECK(xrt_value_root_empty(&owner) && xrt_billing_owner_stats(billing).blocks == 1);
        xrt_billing_owner_drop(billing);
        if (status == XRT_EXECUTION_COMPLETED) { CHECK(call.after == 1); break; }
        CHECK(status == XRT_EXECUTION_FAILURE && !call.after && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_OOM);
    }
    CHECK(allowance && allowance < 64);
#endif
    xrt_locations_release(map);
}
static void state_string_root(void *context, XrtModuleState *state) {
    unsigned *completed = context;
    XrtManagedRoot *root = xrt_native_managed_root(1); XrtManagedFrame *frame = NULL;
    managed_require(xrt_managed_frame_enter(root,2,&frame));
    xrt_native_string_create(frame,0,"state",5,1); XrtManagedHeader *first = NULL;
    managed_require(xrt_managed_slot_borrow(frame,0,&first));
    xrt_module_require(xrt_module_begin(state,0),1); XrtModuleRef *reference = NULL;
    xrt_module_require(xrt_module_construct(state,0,&first,&reference),1);
    xrt_module_require(xrt_module_publish(reference),1); xrt_module_require(xrt_module_ready(state,0),1);
    XrtExternalOwner source; xrt_module_require(xrt_module_external_owner(reference,0,&source),1);
    managed_require(xrt_place_copy_replace((XrtPlace){.frame=frame,.slot=1,.nulls=XRT_PLACE_NONNULL},
        (XrtPlace){.external=&source,.nulls=XRT_PLACE_NONNULL},NULL));
    managed_require(xrt_managed_slot_drop(frame,0));
    xrt_native_string_create(frame,0,"new",3,1); XrtManagedHeader *replacement = NULL;
    managed_require(xrt_managed_slot_borrow(frame,0,&replacement));
    xrt_module_require(xrt_module_replace(reference,&replacement),1);
    XrtManagedHeader *snapshot = NULL; managed_require(xrt_managed_slot_borrow(frame,1,&snapshot));
    const XrtStringBacking *old = (const XrtStringBacking *)snapshot;
    CHECK(old->length == 5 && !memcmp(old->bytes,"state",5));
    const void *binding = NULL; xrt_module_require(xrt_module_borrow(reference,&binding),1);
    CHECK(*(XrtManagedHeader *const *)binding == replacement);
    managed_require(xrt_managed_frame_leave(root,frame)); ++*completed;
}
static void standalone_state_string(void) {
    size_t owned = 0;
    XrtValueImageType type = {.kind=XRT_VALUE_STRING,.size=8,.alignment=8,.owned_count=1};
    XrtValueImage image = {.types=&type,.type_count=1,.owned_offsets=&owned,.owned_count=1};
    XrtModuleSpec module = {0}; XrtModuleSlotSpec slot = {0,1,XRT_MODULE_SLOT_MUTABLE,0,8,8};
    XrtModuleLayoutSpec spec = {&image,&module,1,NULL,0,&slot,1,8,8};
    XrtSourceLocation location = {"standalone-state.xr",19,1,1,0,0}; XrtSourceMap map = {&location,1};
    unsigned completed = 0;
    xrt_run_standalone_state(&map,state_string_root,&completed,&spec);
    CHECK(completed == 1 && !xrt_value_root_active());
}
int main(void) {
    managed_scopes();
    standalone_state_string();
    Context *outer = xr_calloc(1, sizeof(*outer));
    Context *inner = xr_calloc(1, sizeof(*inner));
    CHECK(outer && inner);
    outer->owner = locations("outer-source.xr");
    inner->owner = locations("inner-source.xr");
    outer->inner = inner;
    memset(&outer->failure, 0xa5, sizeof(outer->failure));
    unsigned char prior[sizeof(outer->failure)];
    memcpy(prior, &outer->failure, sizeof(prior));
    CHECK(xrt_run_embedded(NULL, &outer->failure, normal_root, outer) == XRT_EXECUTION_INVALID);
    CHECK(!outer->visits && !memcmp(prior, &outer->failure, sizeof(prior)));
    CHECK(xrt_run_embedded(outer->owner, NULL, normal_root, outer) == XRT_EXECUTION_INVALID);
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, NULL, outer) == XRT_EXECUTION_INVALID);
    CHECK(!outer->visits && !memcmp(prior, &outer->failure, sizeof(prior)));
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, normal_root, outer) == XRT_EXECUTION_COMPLETED);
    CHECK(outer->visits == 1 && !memcmp(prior, &outer->failure, sizeof(prior)));
    for (unsigned failure = 0; failure < 2; ++failure) {
        outer->after = inner->after = 0;
        outer->fail_inner = failure != 0;
        memset(&inner->failure, 0xa5, sizeof(inner->failure));
        SavedOutput saved = disable_output();
        XrtExecutionStatus status = xrt_run_embedded(outer->owner, &outer->failure, nested_root, outer);
        restore_output(saved);
        CHECK(status == XRT_EXECUTION_FAILURE && !outer->after && !inner->after);
        CHECK(outer->failure.kind == XRT_FAILURE_OUTPUT);
        XrtOutputFailure *output = &outer->failure.detail.output;
        CHECK(output->io.failed && output->io.code && output->source_id == 1);
        CHECK(output->io.domain != XRT_IO_NONE);
        CHECK(outer->inner_status == (failure ? XRT_EXECUTION_FAILURE : XRT_EXECUTION_COMPLETED));
        if (failure) {
            CHECK(inner->failure.kind == XRT_FAILURE_OUTPUT);
            CHECK(inner->failure.detail.output.io.failed && inner->failure.detail.output.source_id == 1);
        } else CHECK(!memcmp(prior, &inner->failure, sizeof(prior)));
        // The prior failed invocation leaves no expired recovery target behind.
        CHECK(xrt_run_embedded(outer->owner, &outer->failure, normal_root, outer) == XRT_EXECUTION_COMPLETED);
    }
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, output_root, outer) == XRT_EXECUTION_COMPLETED);
    CHECK(outer->after == 1);
    outer->after = 0;
    SavedOutput string_output = disable_output();
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, string_root, outer) == XRT_EXECUTION_FAILURE);
    restore_output(string_output);
    CHECK(!outer->after && outer->failure.kind == XRT_FAILURE_OUTPUT);
    CHECK(outer->failure.detail.output.operation == XRT_OUTPUT_WRITE);
    CHECK(outer->failure.detail.output.source_id == 1 && outer->failure.detail.output.io.failed);
    CHECK(outer->failure.detail.output.io.code && outer->failure.detail.output.io.domain != XRT_IO_NONE);
    const XrtSourceLocation *string_location = xrt_locations_get(outer->owner, 1);
    CHECK(string_location && string_location->line == 7 && string_location->column == 9);
    memcpy(prior, &outer->failure, sizeof(prior));
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, normal_root, outer) == XRT_EXECUTION_COMPLETED);
    CHECK(!memcmp(prior, &outer->failure, sizeof(prior)));
    for (uint32_t code = 420; code <= 421; ++code) {
        for (uint32_t id = 0; id <= 1; ++id) {
            outer->panic_code = code;
            outer->source_id = id;
            CHECK(xrt_run_embedded(outer->owner, &outer->failure, panic_root, outer) == XRT_EXECUTION_FAILURE);
            CHECK(outer->failure.kind == XRT_FAILURE_TYPED_PANIC);
            CHECK(outer->failure.detail.panic.code == code && outer->failure.detail.panic.source_id == id);
            memcpy(prior, &outer->failure, sizeof(prior));
            CHECK(xrt_run_embedded(outer->owner, &outer->failure, normal_root, outer) == XRT_EXECUTION_COMPLETED);
            CHECK(!memcmp(prior, &outer->failure, sizeof(prior)));
        }
    }
    inner->panic_code = 420;
    inner->source_id = 1;
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, nested_root, outer) == XRT_EXECUTION_FAILURE);
    CHECK(outer->inner_status == XRT_EXECUTION_FAILURE);
    CHECK(inner->failure.kind == XRT_FAILURE_TYPED_PANIC && inner->failure.detail.panic.code == 420);
    CHECK(inner->failure.detail.panic.source_id == 1 && outer->failure.detail.panic.code == 421);
    // A recovered panic restores the enclosing output binding and its variant.
    outer->panic_code = 0;
    SavedOutput saved = disable_output();
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, nested_root, outer) == XRT_EXECUTION_FAILURE);
    restore_output(saved);
    CHECK(outer->failure.kind == XRT_FAILURE_OUTPUT && outer->failure.detail.output.source_id == 1);
    CHECK(inner->failure.kind == XRT_FAILURE_TYPED_PANIC && outer->inner_status == XRT_EXECUTION_FAILURE);
    xrt_locations_release(inner->owner);
    xrt_locations_release(outer->owner);
    xr_free(inner);
    xr_free(outer);
    fputs("Synchronous runtime scope passed\n", stderr);
    return 0;
}
