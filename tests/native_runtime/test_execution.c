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
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_array_backing.h"
#include "runtime/native/xnative_host_environment.h"
#include "runtime/native/xnative_host_file.h"
#include "runtime/native/xnative_host_io.h"
#include "runtime/native/xnative_host_stat.h"
#include "runtime/native/xnative_values.h"
#include "runtime/native/xnative_string_backing_ops.h"
#include "xray_native_values.h"
#include "xray_native_array.h"
#include "xray_native_place.h"
#include "xray_native_state.h"
#include "xray_native_map.h"
#include "base/xmalloc.h"
#include "shared/xnative_declaration.h"
#include <stdatomic.h>
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

typedef struct HostPathCall {
    XrtBillingOwner *billing;
    size_t allowance;
} HostPathCall;

static void host_path_root(void *context) {
    HostPathCall *call = context;
    XrtManagedFrame *frame = NULL;
    managed_require(xrt_managed_frame_enter(xrt_native_managed_root(1), 1, &frame));
    const char path[] = "native-execution-test-missing-parent/child";
    xrt_native_string_create(frame, 0, path, sizeof(path) - 1, 1);
    XrtManagedHeader *text = NULL;
    managed_require(xrt_managed_slot_borrow(frame, 0, &text));
    xrt_billing_owner_fail_after(call->billing, call->allowance);
    (void)xrt_native_host_path_bool(XR_NATIVE_CALLABLE_IO_MKDIR, (const XrtStringBacking *)text, 1);
}

static void managed_host_path_scratch(void) {
    XrtLocationOwner *owner = locations("host-path.xr");
    size_t allocations = 1;
#if defined(XR_OS_WINDOWS)
    allocations = 2; /* Terminated UTF-8 and the wide OS path. */
#endif
    for (size_t allowance = 0; allowance < allocations; ++allowance) {
        XrtValueRoot root = {0};
        HostPathCall call = {.allowance=allowance};
        XrtExecutionFailure failure = {0};
        CHECK(xrt_native_values_prepare(&root, &call.billing) == XRT_MANAGED_OK);
        CHECK(xrt_billing_owner_retain(call.billing));
        CHECK(xrt_run_embedded_owned(owner, &failure, host_path_root, &call, &root) == XRT_EXECUTION_FAILURE);
        CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_OOM &&
            failure.detail.value.source_id == 1);
        CHECK(xrt_value_root_empty(&root) && xrt_billing_owner_stats(call.billing).blocks == 1);
        xrt_billing_owner_drop(call.billing);
    }
    xrt_locations_release(owner);
}

typedef struct MetadataRetirement {
    XrtValueRoot *root;
    XrtBillingOwner *billing;
    XrOsFileMetadataOwner *metadata;
    size_t cold_bytes;
    unsigned visits, closes;
    bool fail_guest;
} MetadataRetirement;

static MetadataRetirement *metadata_retirement;

static void metadata_root(void *context) {
    MetadataRetirement *call = context;
    ++call->visits;
    CHECK(xrt_value_root_active() == call->root);
    managed_require(xrt_native_values_stat_prepare(call->root, &call->billing, &call->metadata));
    CHECK(call->metadata && !call->metadata->handle && xrt_billing_owner_retain(call->billing));
    call->cold_bytes = xrt_billing_owner_stats(call->billing).bytes;
    const XrtBackingTypes *types = NULL;
    XrtManagedFrame *frame = NULL;
    managed_require(xrt_native_values_backings(call->root, &types));
    managed_require(xrt_managed_frame_enter(xrt_native_managed_root(1), 1, &frame));
    managed_require(xrt_string_backing_create(frame, 0, types, "live", 4, 0));
    CHECK(xrt_billing_owner_stats(call->billing).bytes > call->cold_bytes);
    call->metadata->handle = 17; /* Token used only with the injected closer. */
    if (call->fail_guest) xrt_value_failure(XR_XIR_VALUE_BAD_UTF8, 1);
}

static XrOsIoStatus metadata_close(XrOsFileMetadataOwner *owner) {
    MetadataRetirement *call = metadata_retirement;
    CHECK(call && call->visits == 1 && owner == call->metadata && owner->handle == 17);
    CHECK(xrt_billing_owner_stats(call->billing).bytes == call->cold_bytes);
    CHECK(xrt_value_root_drain(call->root) == XR_OS_IO_BAD_ARGUMENT);
    if (++call->closes <= 2) return XR_OS_IO_IO;
    owner->handle = 0;
    return XR_OS_IO_OK;
}

static void implicit_metadata_root(void *context) {
    XrtValueRoot *root = xrt_value_root_active();
    XrtBillingOwner *billing = NULL;
    XrOsFileMetadataOwner *metadata = NULL;
    CHECK(xrt_native_values_stat_prepare(root, &billing, &metadata) == XRT_MANAGED_ARGUMENT);
    CHECK(!billing && !metadata && xrt_value_root_empty(root));
    ++*(unsigned *)context;
}

static void managed_metadata_retirement(void) {
    XrtLocationOwner *where = locations("metadata-close.xr");
    XrtValueRoot *previous = xrt_value_root_active();
    for (unsigned fail_guest = 0; fail_guest < 2; ++fail_guest) {
        XrtValueRoot root = {.byte_limit=1024 * 1024};
        MetadataRetirement call = {.root=&root, .fail_guest=fail_guest != 0};
        XrtExecutionFailure failure = {0};
        metadata_retirement = &call;
        CHECK(xrt_run_embedded_owned_with_metadata_close(where, &failure, metadata_root, &call, &root, metadata_close) ==
            XRT_EXECUTION_FAILURE);
        CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.source_id == fail_guest);
        CHECK(failure.detail.value.status == (fail_guest ? XR_XIR_VALUE_BAD_UTF8 : XR_XIR_VALUE_IO));
        unsigned char original_failure[sizeof(failure)];
        memcpy(original_failure, &failure, sizeof(failure));
        CHECK(call.closes == 1 && call.metadata->handle == 17 && !xrt_value_root_empty(&root));
        CHECK(xrt_value_root_active() == previous);
        Context refused = {0};
        CHECK(xrt_run_embedded_owned(where, &failure, normal_root, &refused, &root) == XRT_EXECUTION_INVALID);
        XrOsFileMetadataOwner *unused = NULL;
        XrtBillingOwner *billing = NULL;
        const XrtBackingTypes *types = NULL;
        CHECK(xrt_native_values_stat_prepare(&root, &billing, &unused) == XRT_MANAGED_ARGUMENT);
        CHECK(xrt_native_values_backings(&root, &types) == XRT_MANAGED_ARGUMENT);
        CHECK(!refused.visits && !billing && !unused && !types);

        CHECK(xrt_value_root_drain_with(&root, metadata_close) == XR_OS_IO_IO);
        CHECK(call.closes == 2 && call.metadata->handle == 17 && !xrt_value_root_empty(&root));
        CHECK(xrt_value_root_drain_with(&root, metadata_close) == XR_OS_IO_OK);
        CHECK(call.closes == 3 && call.visits == 1 && xrt_value_root_empty(&root));
        CHECK(root.byte_limit == 1024 * 1024 && xrt_billing_owner_stats(call.billing).blocks == 1);
        CHECK(xrt_value_root_drain_with(&root, metadata_close) == XR_OS_IO_OK && call.closes == 3);
        CHECK(xrt_run_embedded_owned(where, &failure, normal_root, &refused, &root) == XRT_EXECUTION_COMPLETED);
        CHECK(refused.visits == 1 && xrt_value_root_empty(&root));
        CHECK(!memcmp(original_failure, &failure, sizeof(failure)));
        metadata_retirement = NULL;
        xrt_billing_owner_drop(call.billing);
    }
    unsigned visits = 0;
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(where, &failure, implicit_metadata_root, &visits) == XRT_EXECUTION_COMPLETED);
    CHECK(visits == 1 && xrt_value_root_active() == previous);
    xrt_locations_release(where);
}

typedef struct HostStatControl {
    atomic_bool cancelled;
    unsigned polls;
    bool bad_path;
} HostStatControl;

static bool stat_control_current(void *context) {
    HostStatControl *request = context;
    ++request->polls;
    return !atomic_load_explicit(&request->cancelled, memory_order_acquire);
}

static void controlled_metadata_root(void *context) {
    HostStatControl *request = context;
    const XrtHostExecutionControl *control = xrt_execution_host_control();
    CHECK(control && control->context == request);
    XrtValueRoot *root = xrt_value_root_active();
    XrtHostStatQuery query = {.current=control->current, .context=control->context};
    managed_require(xrt_native_values_stat_prepare(root, &query.billing, &query.owner));
    const XrtBackingTypes *types = NULL;
    XrtManagedFrame *frame = NULL;
    XrtManagedHeader *path = NULL;
    managed_require(xrt_native_values_backings(root, &types));
    managed_require(xrt_managed_frame_enter(xrt_native_managed_root(1), 1, &frame));
    const char name[] = ".\0invalid";
    managed_require(xrt_string_backing_create(frame, 0, types, name, request->bad_path ? sizeof(name) - 1 : 1, 0));
    managed_require(xrt_managed_slot_borrow(frame, 0, &path));
    XrOsFileMetadata result = {.size=77};
    XrOsIoStatus status = xrt_host_stat_query(&query, path, &result);
    if (status != XR_OS_IO_OK) {
        CHECK(result.size == 77 && !result.present);
        if (request->bad_path) {
            CHECK(status == XR_OS_IO_BAD_ARGUMENT);
            atomic_store_explicit(&request->cancelled, true, memory_order_release);
        }
        xrt_value_failure(xrt_host_io_failure(status), 1);
    }
    CHECK(result.present && result.is_dir);
}

static void metadata_without_control(void *context) {
    CHECK(!xrt_execution_host_control());
    ++*(unsigned *)context;
}

typedef struct NestedStatControl {
    HostStatControl outer, inner;
    const XrtLocationOwner *where;
} NestedStatControl;

static void nested_metadata_control(void *context) {
    NestedStatControl *calls = context;
    const XrtHostExecutionControl *outer = xrt_execution_host_control();
    CHECK(outer && outer->context == &calls->outer);
    unsigned ordinary = 0;
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(calls->where, &failure, metadata_without_control, &ordinary) == XRT_EXECUTION_COMPLETED);
    CHECK(ordinary == 1 && xrt_execution_host_control() == outer);
    for (unsigned bad_path = 0; bad_path <= 1; ++bad_path) {
        XrtValueRoot owned = {0};
        calls->inner.bad_path = bad_path != 0;
        atomic_store_explicit(&calls->inner.cancelled, !bad_path, memory_order_release);
        const XrtHostExecutionControl inner = {stat_control_current, &calls->inner};
        CHECK(xrt_run_embedded_owned_controlled(calls->where, &failure, controlled_metadata_root, &calls->inner,
            &owned, &inner) == XRT_EXECUTION_FAILURE);
        CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.source_id == 1 &&
            failure.detail.value.status == (bad_path ? XR_XIR_VALUE_BAD_ARGUMENT : XR_XIR_VALUE_CANCELLED));
        CHECK(xrt_value_root_empty(&owned) && xrt_execution_host_control() == outer);
    }
    controlled_metadata_root(&calls->outer);
}

static void managed_host_stat_control(void) {
    XrtLocationOwner *where = locations("metadata-control.xr");
    NestedStatControl calls = {.where=where};
    atomic_init(&calls.outer.cancelled, false);
    atomic_init(&calls.inner.cancelled, false);
    const XrtHostExecutionControl control = {stat_control_current, &calls.outer};
    XrtValueRoot root = {0};
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded_owned_controlled(where, &failure, nested_metadata_control, &calls, &root, &control) ==
        XRT_EXECUTION_COMPLETED);
    CHECK(calls.outer.polls && calls.inner.polls && !xrt_execution_host_control() && xrt_value_root_empty(&root));
    xrt_locations_release(where);
}

typedef enum HostFileMode {
    HOST_FILE_VALID, HOST_FILE_INVALID_HOME, HOST_FILE_WRONG_BACKING, HOST_FILE_INVALID_LENGTH,
} HostFileMode;
typedef struct HostFileCall {
    const char *path;
    HostFileMode mode;
} HostFileCall;

static const size_t host_file_image_offset = 0;
static const XrtValueImageType host_file_image_types[] = {
    {.kind=XRT_VALUE_RECORD,.size=1,.alignment=1},
    {.kind=XRT_VALUE_CLASS,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.element=1,
        .class_storage=XR_NATIVE_DECLARATION_FILE_STORAGE},
};
static const XrtValueImage host_file_image = {.types=host_file_image_types,.type_count=2,
    .owned_offsets=&host_file_image_offset,.owned_count=1};

static void host_file_root(void *context) {
    const HostFileCall *call = context;
    const size_t owner_offset = offsetof(XrtHostFileOptional, owner);
    const size_t frame_offsets[] = {owner_offset, SIZE_MAX, SIZE_MAX, SIZE_MAX};
    const XrtManagedFrameSpec spec = {4,sizeof(XrtHostFileOptional),_Alignof(XrtHostFileOptional),frame_offsets};
    XrtManagedRoot *root = xrt_native_managed_root(1);
    XrtManagedFrame *frame = NULL;
    void *payload = NULL;
    managed_require(xrt_managed_frame_enter_payload(root, &spec, &frame, &payload));
    xrt_native_string_create(frame, 2, call->path, strlen(call->path), 1);
    XrtManagedHeader *text = NULL;
    managed_require(xrt_managed_slot_borrow(frame, 2, &text));
    bool invalid_data = call->mode == HOST_FILE_WRONG_BACKING || call->mode == HOST_FILE_INVALID_LENGTH;
    const XrtNativeFileOpen open = {
        .output={frame,0,1,call->mode == HOST_FILE_INVALID_HOME ? owner_offset : 0},
        .image=&host_file_image,.type=2,.path=(const XrtStringBacking *)text,.append=invalid_data,
    };
    xrt_native_host_file_open_write(XR_NATIVE_CALLABLE_IO_FILE_OPEN_WRITE, &open, 1);
    CHECK(call->mode != HOST_FILE_INVALID_HOME && *(const uint8_t *)payload == 1);
    if (invalid_data) {
        XrtManagedHeader *file = NULL;
        managed_require(xrt_managed_slot_borrow(frame, 0, &file));
        XrtManagedHeader *data = text;
        if (call->mode == HOST_FILE_INVALID_LENGTH) {
            const uint8_t byte = 0xff;
            const XrtArrayInput input = {XRT_BACKING_ARRAY_U8,&byte,1};
            xrt_native_array_create(frame, 3, &input, 1);
            data = NULL;
            managed_require(xrt_managed_slot_borrow(frame, 3, &data));
            XrtArrayBacking *array = (XrtArrayBacking *)data;
            array->length = array->capacity + 1;
        }
        const XrtNativeFileWrite forged = {.image=&host_file_image,.type=2,.file=file,.data=data};
        (void)xrt_native_host_file_write(XR_NATIVE_CALLABLE_IO_FILE_WRITE, &forged, 1);
        CHECK(false);
    }
    const XrtPlace owner = {.frame=frame,.slot=0,.nulls=XRT_PLACE_NONNULL};
    const XrtPlace alias = {.frame=frame,.slot=3,.nulls=XRT_PLACE_NONNULL};
    managed_require(xrt_place_copy_replace(alias, owner, NULL));
    managed_require(xrt_managed_slot_drop(frame, 0));
#if defined(XR_OS_WINDOWS)
    HANDLE probe = CreateFileA(call->path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(probe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
#endif
    managed_require(xrt_managed_slot_drop(frame, 3));
#if defined(XR_OS_WINDOWS)
    probe = CreateFileA(call->path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(probe != INVALID_HANDLE_VALUE && CloseHandle(probe));
#endif
    managed_require(xrt_managed_frame_leave(root, frame));
}

static void managed_host_file(void) {
    XrtLocationOwner *owner = locations("host-file.xr");
    char path[64];
#if defined(XR_OS_WINDOWS)
    unsigned long process = GetCurrentProcessId();
#else
    unsigned long process = (unsigned long)getpid();
#endif
    CHECK(snprintf(path, sizeof(path), "native-execution-owned-file-%lu.dat", process) < (int)sizeof(path));
    FILE *seed = fopen(path, "wbx");
    CHECK(seed && fputs("keep", seed) >= 0 && fclose(seed) == 0);
    XrtValueRoot root = {0};
    const HostFileMode invalid[] = {HOST_FILE_INVALID_HOME, HOST_FILE_WRONG_BACKING, HOST_FILE_INVALID_LENGTH};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        HostFileCall call = {path,invalid[i]};
        XrtExecutionFailure failure = {0};
        CHECK(xrt_run_embedded_owned(owner, &failure, host_file_root, &call, &root) == XRT_EXECUTION_FAILURE);
        CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(xrt_value_root_empty(&root));
        seed = fopen(path, "rb");
        char content[5] = {0};
        CHECK(seed && fread(content, 1, 4, seed) == 4 && !strcmp(content, "keep") &&
            fgetc(seed) == EOF && fclose(seed) == 0);
    }
    HostFileCall valid = {path,HOST_FILE_VALID};
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded_owned(owner, &failure, host_file_root, &valid, &root) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&root) && remove(path) == 0);
    xrt_locations_release(owner);
}

static XrtManagedHeader *host_file_borrow(XrtManagedFrame *frame, size_t slot) {
    XrtManagedHeader *owner = NULL;
    CHECK(xrt_managed_slot_borrow(frame, slot, &owner) == XRT_MANAGED_OK);
    return owner;
}

static void check_file_read_result(XrtHostFileRead read, bool present) {
    CHECK(read.managed == XRT_MANAGED_OK && read.host == XR_OS_IO_OK && read.present == present);
}

static void check_file_read_bytes(XrtManagedFrame *frame, size_t slot, const uint8_t *bytes,
    size_t length, size_t capacity) {
    const XrtManagedHeader *owner = host_file_borrow(frame, slot);
    CHECK(xrt_backing_array_matches(owner, XRT_BACKING_ARRAY_U8));
    const XrtArrayBacking *array = (const XrtArrayBacking *)owner;
    CHECK(array->length == length && array->capacity == capacity);
    CHECK(!length || !memcmp(array->elements, bytes, length));
}

typedef struct HostFileReadPublicationCall {
    const char *path;
    const uint8_t *bytes;
    size_t length;
} HostFileReadPublicationCall;

static void host_file_read_publication(void *context) {
    const HostFileReadPublicationCall *call = context;
    enum { READER, OPEN_STAGE, RESULT, READ_STAGE, PATH, MISSING, SLOT_COUNT };
    const size_t owner_offset = offsetof(XrtHostFileOptional, owner);
    const size_t homes[] = {0, sizeof(XrtHostFileOptional), 2 * sizeof(XrtHostFileOptional),
        3 * sizeof(XrtHostFileOptional)};
    const size_t offsets[] = {homes[0] + owner_offset, homes[1] + owner_offset, homes[2] + owner_offset,
        homes[3] + owner_offset, SIZE_MAX, SIZE_MAX};
    const XrtManagedFrameSpec spec = {SLOT_COUNT, 4 * sizeof(XrtHostFileOptional), _Alignof(XrtHostFileOptional), offsets};
    XrtManagedRoot *root = xrt_native_managed_root(1);
    XrtValueRoot *execution = xrt_value_root_current(1);
    XrtBillingOwner *billing = NULL;
    const XrtBackingTypes *backings = NULL;
    managed_require(xrt_native_values_prepare(execution, &billing));
    managed_require(xrt_native_values_backings(execution, &backings));
    XrtValueImagePlans *plans = NULL;
    XrtBoundClass binding = {0};
    managed_require(xrt_value_image_bind(billing, &host_file_image, (XrtManagedLease){0}, &plans));
    managed_require(xrt_value_image_class(plans, 2, &binding));
    XrtManagedFrame *frame = NULL;
    void *payload = NULL;
    managed_require(xrt_managed_frame_enter_payload(root, &spec, &frame, &payload));
    uint8_t *open_tag = (uint8_t *)payload + homes[0], *tag = (uint8_t *)payload + homes[2];
    managed_require(xrt_string_backing_create(frame, PATH, backings, call->path, strlen(call->path), 0));
    const XrtStringBacking *path = (const XrtStringBacking *)host_file_borrow(frame, PATH);
    const XrtHostFileStagedResult opened_home = {{frame, READER, OPEN_STAGE, homes[0]}, offsets[OPEN_STAGE]};
    const XrtHostFileStagedResult read_home = {{frame, RESULT, READ_STAGE, homes[2]}, offsets[READ_STAGE]};
    XrtHostFileOpen opened = xrt_host_file_open_read_publish(&opened_home, binding, path, 1);
    CHECK(opened.managed == XRT_MANAGED_OK && opened.host == XR_OS_IO_OK && opened.opened && *open_tag == 1);
    const XrtManagedHeader *reader = host_file_borrow(frame, READER);
    CHECK(reader && !host_file_borrow(frame, OPEN_STAGE));

    managed_require(xrt_array_backing_i64_create(frame, RESULT, backings, NULL, 0, 0));
    const XrtManagedHeader *wrong_family = host_file_borrow(frame, RESULT);
    *tag = 1;
    XrtBillingStats before = xrt_billing_owner_stats(billing);
    XrtHostFileRead rejected = xrt_host_file_read_publish(&read_home, binding, reader, backings, 1, 1);
    CHECK(rejected.managed == XRT_MANAGED_ARGUMENT && *tag == 1 && host_file_borrow(frame, RESULT) == wrong_family);
    CHECK(xrt_billing_owner_stats(billing).allocations == before.allocations);
    managed_require(xrt_managed_slot_drop(frame, RESULT));
    rejected = xrt_host_file_read_publish(&read_home, binding, reader, backings, 1, 1);
    CHECK(rejected.managed == XRT_MANAGED_ARGUMENT && *tag == 1 && !host_file_borrow(frame, RESULT));
    *tag = 2;
    rejected = xrt_host_file_read_publish(&read_home, binding, reader, backings, 1, 1);
    CHECK(rejected.managed == XRT_MANAGED_ARGUMENT && *tag == 2);
    *tag = 0;

    const XrtHostFileStagedResult owner_as_tag = {{frame, RESULT, READ_STAGE, offsets[RESULT]}, offsets[READ_STAGE]};
    rejected = xrt_host_file_read_publish(&owner_as_tag, binding, reader, backings, 1, 1);
    CHECK(rejected.managed == XRT_MANAGED_ARGUMENT);
    const XrtHostFileStagedResult wrong_stage = {read_home.location, offsets[OPEN_STAGE]};
    rejected = xrt_host_file_read_publish(&wrong_stage, binding, reader, backings, 1, 1);
    CHECK(rejected.managed == XRT_MANAGED_ARGUMENT);
    const XrtHostFileStagedResult aliased_home = {{frame, RESULT, RESULT, homes[2]}, offsets[RESULT]};
    rejected = xrt_host_file_read_publish(&aliased_home, binding, reader, backings, 1, 1);
    CHECK(rejected.managed == XRT_MANAGED_ARGUMENT);

    XrtManagedRoot *other_root = NULL;
    XrtManagedFrame *other_frame = NULL;
    managed_require(xrt_managed_root_new(billing, &other_root));
    managed_require(xrt_managed_frame_enter_layout(other_root, &spec, &other_frame));
    const XrtHostFileStagedResult foreign_home = {{other_frame, RESULT, READ_STAGE, homes[2]}, offsets[READ_STAGE]};
    rejected = xrt_host_file_read_publish(&foreign_home, binding, reader, backings, 1, 1);
    CHECK(rejected.managed == XRT_MANAGED_ARGUMENT);
    managed_require(xrt_managed_frame_leave(other_root, other_frame));
    managed_require(xrt_managed_root_destroy(&other_root));

    managed_require(xrt_array_backing_u8_create(frame, READ_STAGE, backings, NULL, 0, 0));
    const XrtManagedHeader *occupied = host_file_borrow(frame, READ_STAGE);
    rejected = xrt_host_file_read_publish(&read_home, binding, reader, backings, 1, 1);
    CHECK(rejected.managed == XRT_MANAGED_ARGUMENT && *tag == 0 && host_file_borrow(frame, READ_STAGE) == occupied);
    managed_require(xrt_managed_slot_drop(frame, READ_STAGE));
    XrtManagedHeader *pending = NULL;
    managed_require(xrt_managed_slot_prepare(frame, READ_STAGE, backings->arrays[XRT_BACKING_ARRAY_U8],
        sizeof(XrtArrayBacking), &pending));
    rejected = xrt_host_file_read_publish(&read_home, binding, reader, backings, 1, 1);
    CHECK(rejected.managed == XRT_MANAGED_ARGUMENT && *tag == 0);
    managed_require(xrt_managed_slot_abort(frame, READ_STAGE));

    /* All rejected homes left the real file at its original offset. */
    check_file_read_result(xrt_host_file_read_publish(&read_home, binding, reader, backings, 2, 1), true);
    CHECK(*tag == 1 && !host_file_borrow(frame, READ_STAGE));
    check_file_read_bytes(frame, RESULT, call->bytes, 2, 2);
    const XrtManagedHeader *prior = host_file_borrow(frame, RESULT);
    xrt_billing_owner_fail_after(billing, 0);
    rejected = xrt_host_file_read_publish(&read_home, binding, reader, backings, 8, 1);
    CHECK(rejected.managed == XRT_MANAGED_OOM && *tag == 1 && host_file_borrow(frame, RESULT) == prior);
    opened = xrt_host_file_open_read_publish(&opened_home, binding, path, 1);
    CHECK(opened.managed == XRT_MANAGED_OOM && *open_tag == 1 && host_file_borrow(frame, READER) == reader);
    xrt_billing_owner_fail_after(billing, SIZE_MAX);
    check_file_read_result(xrt_host_file_read_publish(&read_home, binding, reader, backings, 8, 1), true);
    CHECK(*tag == 1 && !host_file_borrow(frame, READ_STAGE));
    check_file_read_bytes(frame, RESULT, call->bytes + 2, call->length - 2, 8);
    check_file_read_result(xrt_host_file_read_publish(&read_home, binding, reader, backings, -1, 1), false);
    CHECK(*tag == 0 && !host_file_borrow(frame, RESULT) && !host_file_borrow(frame, READ_STAGE));
    check_file_read_result(xrt_host_file_read_publish(&read_home, binding, reader, backings, 8, 1), true);
    CHECK(*tag == 1 && !host_file_borrow(frame, READ_STAGE));
    check_file_read_bytes(frame, RESULT, NULL, 0, 8);

    char missing[96];
    int length = snprintf(missing, sizeof(missing), "%s/no-entry", call->path);
    CHECK(length > 0 && (size_t)length < sizeof(missing));
    managed_require(xrt_string_backing_create(frame, MISSING, backings, missing, (size_t)length, 0));
    const XrtStringBacking *missing_path = (const XrtStringBacking *)host_file_borrow(frame, MISSING);
    opened = xrt_host_file_open_read_publish(&opened_home, binding, missing_path, 1);
    CHECK(opened.managed == XRT_MANAGED_OK && opened.host == XR_OS_IO_OK && !opened.opened && *open_tag == 0);
    CHECK(!host_file_borrow(frame, READER) && !host_file_borrow(frame, OPEN_STAGE));
    managed_require(xrt_managed_frame_leave(root, frame));
    xrt_value_image_drop(&plans);
}

enum {
    READ_ABI_RECORD = 1, READ_ABI_FILE, READ_ABI_OTHER_FILE, READ_ABI_NULL_FILE, READ_ABI_NULL_OTHER,
    READ_ABI_BYTES, READ_ABI_NULL_BYTES, READ_ABI_INTS, READ_ABI_NULL_INTS,
};
static const size_t file_read_abi_owned[] = {0, offsetof(XrtHostFileOptional, owner)};
static const XrtValueImageType file_read_abi_types[] = {
    {.kind=XRT_VALUE_RECORD,.size=1,.alignment=1},
    {.kind=XRT_VALUE_CLASS,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.element=READ_ABI_RECORD,
        .class_storage=XR_NATIVE_DECLARATION_FILE_STORAGE},
    {.kind=XRT_VALUE_CLASS,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1,.element=READ_ABI_RECORD,
        .class_storage=XR_NATIVE_DECLARATION_FILE_STORAGE},
    {.kind=XRT_VALUE_NULLABLE,.size=sizeof(XrtHostFileOptional),.alignment=_Alignof(XrtHostFileOptional),
        .first_owned=1,.owned_count=1,.nullable_element=READ_ABI_FILE,.nullable_payload=offsetof(XrtHostFileOptional, owner)},
    {.kind=XRT_VALUE_NULLABLE,.size=sizeof(XrtHostFileOptional),.alignment=_Alignof(XrtHostFileOptional),
        .first_owned=1,.owned_count=1,.nullable_element=READ_ABI_OTHER_FILE,.nullable_payload=offsetof(XrtHostFileOptional, owner)},
    {.kind=XRT_VALUE_ARRAY_U8,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1},
    {.kind=XRT_VALUE_NULLABLE,.size=sizeof(XrtHostFileOptional),.alignment=_Alignof(XrtHostFileOptional),
        .first_owned=1,.owned_count=1,.nullable_element=READ_ABI_BYTES,.nullable_payload=offsetof(XrtHostFileOptional, owner)},
    {.kind=XRT_VALUE_ARRAY_I64,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1},
    {.kind=XRT_VALUE_NULLABLE,.size=sizeof(XrtHostFileOptional),.alignment=_Alignof(XrtHostFileOptional),
        .first_owned=1,.owned_count=1,.nullable_element=READ_ABI_INTS,.nullable_payload=offsetof(XrtHostFileOptional, owner)},
};
static const XrtValueImage file_read_abi_image = {.types=file_read_abi_types,.type_count=READ_ABI_NULL_INTS,
    .owned_offsets=file_read_abi_owned,.owned_count=2};
enum { FILE_ABI_RESULT, FILE_ABI_STAGE, FILE_ABI_PATH, FILE_ABI_SLOTS };
static const size_t file_read_abi_offsets[] = {offsetof(XrtHostFileOptional, owner),
    sizeof(XrtHostFileOptional) + offsetof(XrtHostFileOptional, owner), SIZE_MAX};
static const XrtManagedFrameSpec file_read_abi_frame = {
    FILE_ABI_SLOTS, 2 * sizeof(XrtHostFileOptional), _Alignof(XrtHostFileOptional), file_read_abi_offsets};

typedef enum HostFileReadAbiFault {
    READ_ABI_OPEN_ID, READ_ABI_OPEN_PATH, READ_ABI_OPEN_RESULT,
    READ_ABI_READ_ID, READ_ABI_READ_RESULT, READ_ABI_READ_FILE, READ_ABI_READ_STAGE, READ_ABI_READ_OOM,
} HostFileReadAbiFault;
typedef struct HostFileReadAbiCall {
    XrtModuleState *state;
    const XrtManagedHeader *reader, *path;
    HostFileReadAbiFault fault;
} HostFileReadAbiCall;

static void host_file_read_abi_fault(void *context) {
    const HostFileReadAbiCall *call = context;
    XrtManagedRoot *root = xrt_native_managed_root(1);
    XrtValueRoot *execution = xrt_value_root_current(1);
    xrt_native_values_use_state(execution, call->state, &file_read_abi_image);
    XrtManagedFrame *frame = NULL;
    managed_require(xrt_managed_frame_enter_layout(root, &file_read_abi_frame, &frame));
    XrtNativeFileResult output = {frame, FILE_ABI_RESULT, FILE_ABI_STAGE, 0};
    size_t temporary = file_read_abi_offsets[FILE_ABI_STAGE];
    if (call->fault <= READ_ABI_OPEN_RESULT) {
        const XrtNativeFileOpenRead open = {
            .output=output,.temporary_owner_offset=temporary,.image=&file_read_abi_image,.type=READ_ABI_FILE,
            .result_type=call->fault == READ_ABI_OPEN_RESULT ? READ_ABI_NULL_OTHER : READ_ABI_NULL_FILE,
            .path=call->fault == READ_ABI_OPEN_PATH ? call->reader : call->path,
        };
        uint32_t id = call->fault == READ_ABI_OPEN_ID ? XR_NATIVE_CALLABLE_IO_FILE_OPEN_WRITE : XR_NATIVE_CALLABLE_IO_FILE_OPEN;
        xrt_native_host_file_open_read(id, &open, 1);
    } else {
        if (call->fault == READ_ABI_READ_OOM) {
            XrtBillingOwner *billing = NULL;
            const XrtBackingTypes *types = NULL;
            managed_require(xrt_native_values_prepare(execution, &billing));
            managed_require(xrt_native_values_backings(execution, &types));
            xrt_billing_owner_fail_after(billing, 0);
        }
        const XrtNativeFileRead read = {
            .output=output,.temporary_owner_offset=call->fault == READ_ABI_READ_STAGE ? 0 : temporary,
            .image=&file_read_abi_image,.type=call->fault == READ_ABI_READ_FILE ? READ_ABI_OTHER_FILE : READ_ABI_FILE,
            .result_type=call->fault == READ_ABI_READ_RESULT ? READ_ABI_NULL_INTS : READ_ABI_NULL_BYTES,
            .file=call->reader,.max_bytes=1,
        };
        uint32_t id = call->fault == READ_ABI_READ_ID ? XR_NATIVE_CALLABLE_IO_FILE_WRITE : XR_NATIVE_CALLABLE_IO_FILE_READ;
        xrt_native_host_file_read(id, &read, 1);
    }
    CHECK(false);
}

static void host_file_read_native_abi(void *context) {
    const HostFileReadPublicationCall *call = context;
    XrtValueRoot *execution = xrt_value_root_current(1);
    XrtManagedRoot *root = xrt_native_managed_root(1);
    XrtBillingOwner *billing = NULL;
    managed_require(xrt_native_values_prepare(execution, &billing));
    const XrtModuleSpec module = {0};
    const XrtModuleSlotSpec module_slot = {0, READ_ABI_FILE, 0, 0, sizeof(void *), _Alignof(void *)};
    const XrtModuleLayoutSpec module_spec = {&file_read_abi_image, &module, 1, NULL, 0, &module_slot, 1,
        sizeof(void *), _Alignof(void *)};
    XrtModuleLayout *layout = NULL;
    XrtModuleState *state = NULL;
    managed_require(xrt_module_layout_new(billing, &module_spec, (XrtManagedLease){0}, &layout));
    managed_require(xrt_module_state_new(layout, billing, &state));
    xrt_module_layout_drop(&layout);
    xrt_native_values_use_state(execution, state, &file_read_abi_image);

    XrtManagedFrame *file_frame = NULL, *read_frame = NULL;
    void *file_payload = NULL, *read_payload = NULL;
    managed_require(xrt_managed_frame_enter_payload(root, &file_read_abi_frame, &file_frame, &file_payload));
    xrt_native_string_create(file_frame, FILE_ABI_PATH, call->path, strlen(call->path), 1);
    const XrtManagedHeader *path = host_file_borrow(file_frame, FILE_ABI_PATH);
    size_t temporary = file_read_abi_offsets[FILE_ABI_STAGE];
    const XrtNativeFileOpenRead open = {.output={file_frame, FILE_ABI_RESULT, FILE_ABI_STAGE, 0},
        .temporary_owner_offset=temporary,.image=&file_read_abi_image,.type=READ_ABI_FILE,
        .result_type=READ_ABI_NULL_FILE,.path=path};
    xrt_native_host_file_open_read(XR_NATIVE_CALLABLE_IO_FILE_OPEN, &open, 1);
    const XrtManagedHeader *reader = host_file_borrow(file_frame, FILE_ABI_RESULT);
    CHECK(*(uint8_t *)file_payload == 1 && reader && !host_file_borrow(file_frame, FILE_ABI_STAGE));

    XrtLocationOwner *location = locations("file-read-abi-fault.xr");
    for (HostFileReadAbiFault fault = READ_ABI_OPEN_ID; fault <= READ_ABI_READ_OOM; ++fault) {
        HostFileReadAbiCall rejected = {state, reader, path, fault};
        XrtExecutionFailure failure = {0};
        XrtValueRoot inner = {0};
        CHECK(xrt_run_embedded_owned(location, &failure, host_file_read_abi_fault, &rejected, &inner) ==
            XRT_EXECUTION_FAILURE);
        CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.source_id == 1 &&
            failure.detail.value.status == (fault == READ_ABI_READ_OOM ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_BAD_ARGUMENT));
        CHECK(xrt_value_root_empty(&inner));
    }
    xrt_locations_release(location);

    /* The nested failures borrow this same live reader and leave its cursor in place. */
    managed_require(xrt_managed_frame_enter_payload(root, &file_read_abi_frame, &read_frame, &read_payload));
    XrtNativeFileRead read = {.output={read_frame, FILE_ABI_RESULT, FILE_ABI_STAGE, 0},
        .temporary_owner_offset=temporary,.image=&file_read_abi_image,.type=READ_ABI_FILE,
        .result_type=READ_ABI_NULL_BYTES,.file=reader,.max_bytes=2};
    xrt_native_host_file_read(XR_NATIVE_CALLABLE_IO_FILE_READ, &read, 1);
    CHECK(*(uint8_t *)read_payload == 1 && !host_file_borrow(read_frame, FILE_ABI_STAGE));
    check_file_read_bytes(read_frame, FILE_ABI_RESULT, call->bytes, 2, 2);
    read.max_bytes = 8;
    xrt_native_host_file_read(XR_NATIVE_CALLABLE_IO_FILE_READ, &read, 1);
    check_file_read_bytes(read_frame, FILE_ABI_RESULT, call->bytes + 2, call->length - 2, 8);
    xrt_native_host_file_read(XR_NATIVE_CALLABLE_IO_FILE_READ, &read, 1);
    CHECK(*(uint8_t *)read_payload == 1);
    check_file_read_bytes(read_frame, FILE_ABI_RESULT, NULL, 0, 8);
    CHECK(xrt_native_host_file_close(XR_NATIVE_CALLABLE_IO_FILE_CLOSE, &file_read_abi_image, READ_ABI_FILE, reader, 1));
    xrt_native_host_file_read(XR_NATIVE_CALLABLE_IO_FILE_READ, &read, 1);
    CHECK(*(uint8_t *)read_payload == 0 && !host_file_borrow(read_frame, FILE_ABI_RESULT));

    managed_require(xrt_managed_frame_leave(root, read_frame));
    managed_require(xrt_managed_frame_leave(root, file_frame));
    xrt_module_state_drop(&state);
}

static void managed_host_file_read(void) {
    enum { PATH, READER, WRITER, RESULT, DIRECTORY_PATH, DIRECTORY, SLOT_COUNT };
    const uint8_t bytes[] = {0, 127, 128, 255, 10};
    char path[64];
#if defined(XR_OS_WINDOWS)
    unsigned long process = GetCurrentProcessId();
#else
    unsigned long process = (unsigned long)getpid();
#endif
    int length = snprintf(path, sizeof(path), "native-execution-read-file-%lu.dat", process);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    FILE *seed = fopen(path, "wbx");
    CHECK(seed && fwrite(bytes, 1, sizeof(bytes), seed) == sizeof(bytes) && fclose(seed) == 0);

    XrtBillingOwner *billing = NULL;
    CHECK(xrt_billing_owner_new(NULL, SIZE_MAX, &billing) == XRT_MANAGED_OK);
    XrtValueImagePlans *plans = NULL;
    CHECK(xrt_value_image_bind(billing, &host_file_image, (XrtManagedLease){0}, &plans) == XRT_MANAGED_OK);
    XrtBoundClass binding = {0};
    CHECK(xrt_value_image_class(plans, 2, &binding) == XRT_MANAGED_OK);
    XrtBackingTypes backings = {0};
    CHECK(xrt_backing_types_init(billing, &backings) == XRT_MANAGED_OK);
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *frame = NULL;
    CHECK(xrt_managed_root_new(billing, &root) == XRT_MANAGED_OK);
    CHECK(xrt_managed_frame_enter(root, SLOT_COUNT, &frame) == XRT_MANAGED_OK);
    CHECK(xrt_string_backing_create(frame, PATH, &backings, path, (size_t)length, 0) == XRT_MANAGED_OK);
    const XrtStringBacking *name = (const XrtStringBacking *)host_file_borrow(frame, PATH);
    XrOsIoPolicy policy = xrt_host_io_policy(billing);
    XrtHostFileOpen opened = xrt_host_file_open_read(frame, READER, binding, &policy, name);
    CHECK(opened.managed == XRT_MANAGED_OK && opened.host == XR_OS_IO_OK && opened.opened);
    opened = xrt_host_file_open_write(frame, WRITER, binding, &policy, name, true);
    CHECK(opened.managed == XRT_MANAGED_OK && opened.host == XR_OS_IO_OK && opened.opened);
    XrtManagedHeader *reader = host_file_borrow(frame, READER), *writer = host_file_borrow(frame, WRITER);
    XrtBillingStats before = xrt_billing_owner_stats(billing);
    xrt_billing_owner_fail_after(billing, 1);
    opened = xrt_host_file_open_read(frame, READER, binding, &policy, name);
    CHECK(opened.managed == XRT_MANAGED_OK && opened.host == XR_OS_IO_OUT_OF_MEMORY && !opened.opened);
    XrtBillingStats after = xrt_billing_owner_stats(billing);
    CHECK(after.bytes == before.bytes && after.blocks == before.blocks && after.allocations == before.allocations + 1);
    CHECK(host_file_borrow(frame, READER) == reader);
    xrt_billing_owner_fail_after(billing, SIZE_MAX);

    check_file_read_result(xrt_host_file_read(frame, RESULT, binding, writer, &backings, 0), true);
    check_file_read_bytes(frame, RESULT, NULL, 0, 0);
    XrtManagedHeader *prior = host_file_borrow(frame, RESULT);
    before = xrt_billing_owner_stats(billing);
    xrt_billing_owner_fail_after(billing, 0);
    XrtHostFileRead failed = xrt_host_file_read(frame, RESULT, binding, reader, &backings, 2);
    CHECK(failed.managed == XRT_MANAGED_OOM && failed.host == XR_OS_IO_OK && !failed.present);
    check_file_read_result(xrt_host_file_read(frame, RESULT, binding, reader, &backings, -1), false);
    check_file_read_result(xrt_host_file_read(frame, RESULT, binding, reader, &backings, (int64_t)INT32_MAX + 1), false);
    check_file_read_result(xrt_host_file_read(frame, RESULT, binding, writer, &backings, 2), false);
    CHECK(xrt_host_file_read(frame, RESULT, binding, &name->managed, &backings, 2).managed == XRT_MANAGED_ARGUMENT);
    after = xrt_billing_owner_stats(billing);
    CHECK(after.bytes == before.bytes && after.blocks == before.blocks && after.allocations == before.allocations);
    CHECK(host_file_borrow(frame, RESULT) == prior);
    xrt_billing_owner_fail_after(billing, SIZE_MAX);

    const XrtBackingTypes wrong_backing = {.arrays={[XRT_BACKING_ARRAY_U8]=backings.arrays[XRT_BACKING_ARRAY_I64]}};
    failed = xrt_host_file_read(frame, RESULT, binding, reader, &wrong_backing, 8);
    CHECK(failed.managed == XRT_MANAGED_ARGUMENT && !failed.present && host_file_borrow(frame, RESULT) == prior);
    check_file_read_result(xrt_host_file_read(frame, RESULT, binding, reader, &backings, 0), true);
    check_file_read_bytes(frame, RESULT, NULL, 0, 0);
    check_file_read_result(xrt_host_file_read(frame, RESULT, binding, reader, &backings, 2), true);
    check_file_read_bytes(frame, RESULT, bytes, 2, 2);
    check_file_read_result(xrt_host_file_read(frame, RESULT, binding, reader, &backings, 8), true);
    check_file_read_bytes(frame, RESULT, bytes + 2, sizeof(bytes) - 2, 8);
    check_file_read_result(xrt_host_file_read(frame, RESULT, binding, reader, &backings, 8), true);
    check_file_read_bytes(frame, RESULT, NULL, 0, 8);

    bool closed = false;
    CHECK(xrt_host_file_close(binding, reader, &closed) == XRT_MANAGED_OK && closed);
    prior = host_file_borrow(frame, RESULT);
    check_file_read_result(xrt_host_file_read(frame, RESULT, binding, reader, &backings, 0), false);
    check_file_read_result(xrt_host_file_read(frame, RESULT, binding, reader, &backings, 1), false);
    CHECK(host_file_borrow(frame, RESULT) == prior);

    CHECK(xrt_string_backing_create(frame, DIRECTORY_PATH, &backings, ".", 1, 0) == XRT_MANAGED_OK);
    const XrtStringBacking *directory = (const XrtStringBacking *)host_file_borrow(frame, DIRECTORY_PATH);
    opened = xrt_host_file_open_read(frame, DIRECTORY, binding, &policy, directory);
    CHECK(opened.managed == XRT_MANAGED_OK && opened.host == XR_OS_IO_OK);
#if defined(XR_OS_WINDOWS)
    CHECK(!opened.opened);
#else
    CHECK(opened.opened);
    before = xrt_billing_owner_stats(billing);
    check_file_read_result(xrt_host_file_read(frame, RESULT, binding, host_file_borrow(frame, DIRECTORY), &backings, 1), false);
    after = xrt_billing_owner_stats(billing);
    CHECK(after.bytes == before.bytes && after.blocks == before.blocks && after.allocations == before.allocations + 1);
    CHECK(host_file_borrow(frame, RESULT) == prior);
#endif

    CHECK(xrt_managed_frame_leave(root, frame) == XRT_MANAGED_OK);
    CHECK(xrt_managed_root_destroy(&root) == XRT_MANAGED_OK);
    xrt_backing_types_drop(&backings);
    xrt_value_image_drop(&plans);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1);
    xrt_billing_owner_drop(billing);
    HostFileReadPublicationCall publication = {path, bytes, sizeof(bytes)};
    XrtLocationOwner *owner = locations("host-file-read.xr");
    XrtValueRoot execution = {0};
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded_owned(owner, &failure, host_file_read_publication, &publication, &execution) ==
        XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&execution));
    CHECK(xrt_run_embedded_owned(owner, &failure, host_file_read_native_abi, &publication, &execution) ==
        XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&execution));
    xrt_locations_release(owner);
    CHECK(remove(path) == 0);
}

static void host_environment_fixture_set(const char *name, const char *value) {
#if defined(XR_OS_WINDOWS)
    wchar_t wide_name[64], wide_value[32];
    CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide_name, 64) > 0);
    if (value)
        CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, wide_value, 32) > 0);
    CHECK(SetEnvironmentVariableW(wide_name, value ? wide_value : NULL));
#else
    CHECK((value ? setenv(name, value, 1) : unsetenv(name)) == 0);
#endif
}

static XrtManagedHeader *host_environment_slot(XrtManagedFrame *frame, size_t slot) {
    XrtManagedHeader *owner = NULL;
    CHECK(xrt_managed_slot_borrow(frame, slot, &owner) == XRT_MANAGED_OK);
    return owner;
}

static void host_environment_value(const XrtManagedHeader *owner, const char *expected) {
    CHECK(xrt_backing_string_matches(owner));
    const XrtStringBacking *value = (const XrtStringBacking *)owner;
    CHECK(value->length == strlen(expected) && !memcmp(value->bytes, expected, value->length));
}

static void host_environment_publication(void *context) {
    const char *key = context;
    enum { RESULT, STAGE, NAME, SLOT_COUNT };
    static const size_t owned[] = {0, offsetof(XrtHostNullableOwner, owner)};
    static const XrtValueImageType types[] = {
        {.kind=XRT_VALUE_STRING,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1},
        {.kind=XRT_VALUE_NULLABLE,.size=sizeof(XrtHostNullableOwner),.alignment=_Alignof(XrtHostNullableOwner),
            .owned_count=1,.first_owned=1,.nullable_element=1,.nullable_payload=offsetof(XrtHostNullableOwner, owner)},
    };
    static const XrtValueImage image = {.types=types,.type_count=2,.owned_offsets=owned,.owned_count=2};
    const size_t owner = offsetof(XrtHostNullableOwner, owner);
    const size_t offsets[] = {owner, sizeof(XrtHostNullableOwner) + owner, SIZE_MAX};
    const XrtManagedFrameSpec spec = {SLOT_COUNT, 2 * sizeof(XrtHostNullableOwner),
        _Alignof(XrtHostNullableOwner), offsets};
    XrtManagedRoot *root = xrt_native_managed_root(1);
    XrtManagedFrame *frame = NULL;
    void *payload = NULL;
    managed_require(xrt_managed_frame_enter_payload(root, &spec, &frame, &payload));
    xrt_native_string_create(frame, NAME, key, strlen(key), 1);
    const XrtManagedHeader *name = host_environment_slot(frame, NAME);
    uint8_t *tag = payload;
    const XrtNativeEnvironmentGet call = {frame, RESULT, STAGE, 0, offsets[STAGE], &image, 2, name};

    host_environment_fixture_set(key, "present");
    xrt_native_host_environment_get(XR_NATIVE_CALLABLE_OS_GETENV, &call, 1);
    const XrtManagedHeader *prior = host_environment_slot(frame, RESULT);
    CHECK(*tag == 1 && !host_environment_slot(frame, STAGE));
    host_environment_value(prior, "present");
    const XrtHostNullableSlots wrong_stage = {frame, RESULT, STAGE, 0, offsets[RESULT]};
    XrtHostEnvironmentGet rejected = xrt_host_environment_get_publish(&wrong_stage, name, 1);
    CHECK(rejected.managed == XRT_MANAGED_ARGUMENT && *tag == 1 && host_environment_slot(frame, RESULT) == prior);
    XrtBillingOwner *billing = NULL;
    managed_require(xrt_native_values_prepare(xrt_value_root_current(1), &billing));
    const XrtHostNullableSlots slots = {frame, RESULT, STAGE, 0, offsets[STAGE]};
    XrtBillingStats before = xrt_billing_owner_stats(billing);
    xrt_billing_owner_fail_after(billing, 0);
    rejected = xrt_host_environment_get_publish(&slots, name, 1);
    xrt_billing_owner_fail_after(billing, SIZE_MAX);
    XrtBillingStats after = xrt_billing_owner_stats(billing);
    CHECK(rejected.host == XR_OS_IO_OUT_OF_MEMORY && *tag == 1 && host_environment_slot(frame, RESULT) == prior);
    CHECK(after.bytes == before.bytes && after.blocks == before.blocks && !host_environment_slot(frame, STAGE));

    host_environment_fixture_set(key, "");
    xrt_native_host_environment_get(XR_NATIVE_CALLABLE_OS_GETENV, &call, 1);
    CHECK(*tag == 1 && !host_environment_slot(frame, STAGE));
    host_environment_value(host_environment_slot(frame, RESULT), "");
    host_environment_fixture_set(key, NULL);
    xrt_native_host_environment_get(XR_NATIVE_CALLABLE_OS_GETENV, &call, 1);
    CHECK(*tag == 0 && !host_environment_slot(frame, RESULT) && !host_environment_slot(frame, STAGE));
    managed_require(xrt_managed_frame_leave(root, frame));
}

static void managed_host_environment(void) {
    enum { NAME, RESULT, PREFIX, INVALID, NONSTRING, SLOT_COUNT };
    char key[64];
#if defined(XR_OS_WINDOWS)
    unsigned long process = GetCurrentProcessId();
#else
    unsigned long process = (unsigned long)getpid();
#endif
    int key_length = snprintf(key, sizeof(key), "XRAY_NATIVE_ENV_TEST_%lu", process);
    CHECK(key_length > 0 && (size_t)key_length < sizeof(key));
    const char sample[] = "v\xc3\xa9\xe4\xb8\xad";
    XrtBillingOwner *billing = NULL;
    XrtBackingTypes backings = {0};
    XrtManagedRoot *root = NULL;
    XrtManagedFrame *frame = NULL;
    CHECK(xrt_billing_owner_new(NULL, SIZE_MAX, &billing) == XRT_MANAGED_OK);
    CHECK(xrt_backing_types_init(billing, &backings) == XRT_MANAGED_OK);
    CHECK(xrt_managed_root_new(billing, &root) == XRT_MANAGED_OK);
    CHECK(xrt_managed_frame_enter(root, SLOT_COUNT, &frame) == XRT_MANAGED_OK);
    CHECK(xrt_string_backing_create(frame, NAME, &backings, key, (size_t)key_length, 0) == XRT_MANAGED_OK);
    CHECK(xrt_string_backing_create(frame, RESULT, &backings, "existing", 8, 0) == XRT_MANAGED_OK);
    CHECK(xrt_array_backing_u8_create(frame, NONSTRING, &backings, NULL, 0, 0) == XRT_MANAGED_OK);
    const XrtManagedHeader *name = host_environment_slot(frame, NAME);
    const XrtManagedHeader *prior = host_environment_slot(frame, RESULT);
    XrtHostEnvironmentGet got = xrt_host_environment_get(frame, RESULT, billing, &backings, name);
    CHECK(got.managed == XRT_MANAGED_OK && got.host == XR_OS_IO_OK && !got.present);
    CHECK(host_environment_slot(frame, RESULT) == prior);
    got = xrt_host_environment_get(frame, RESULT, billing, &backings, host_environment_slot(frame, NONSTRING));
    CHECK(got.managed == XRT_MANAGED_ARGUMENT && !got.present && host_environment_slot(frame, RESULT) == prior);

    host_environment_fixture_set(key, sample);
    XrtBillingStats before = xrt_billing_owner_stats(billing);
    got = xrt_host_environment_get(frame, RESULT, billing, &backings, name);
    CHECK(got.managed == XRT_MANAGED_OK && got.host == XR_OS_IO_OK && got.present);
    prior = host_environment_slot(frame, RESULT);
    host_environment_value(prior, sample);
    XrtBillingStats retained = xrt_billing_owner_stats(billing);
    size_t allocations = retained.allocations - before.allocations;
    CHECK(allocations > 1);
    for (size_t allowed = 0; allowed < allocations; ++allowed) {
        xrt_billing_owner_fail_after(billing, allowed);
        got = xrt_host_environment_get(frame, RESULT, billing, &backings, name);
        CHECK(!got.present);
        if (allowed + 1 == allocations) {
            CHECK(got.managed == XRT_MANAGED_OOM && got.host == XR_OS_IO_OK);
        } else {
            CHECK(got.managed == XRT_MANAGED_OK && got.host == XR_OS_IO_OUT_OF_MEMORY);
        }
        XrtBillingStats after = xrt_billing_owner_stats(billing);
        CHECK(after.blocks == retained.blocks && after.bytes == retained.bytes);
        CHECK(host_environment_slot(frame, RESULT) == prior);
    }
    xrt_billing_owner_fail_after(billing, SIZE_MAX);

    char prefix[96];
    const char suffix[] = "=ignored";
    size_t prefix_length = (size_t)key_length + 1;
    memcpy(prefix, key, prefix_length);
    memcpy(prefix + prefix_length, suffix, sizeof(suffix) - 1);
    CHECK(xrt_string_backing_create(frame, PREFIX, &backings, prefix,
        prefix_length + sizeof(suffix) - 1, 0) == XRT_MANAGED_OK);
    got = xrt_host_environment_get(frame, RESULT, billing, &backings, host_environment_slot(frame, PREFIX));
    CHECK(got.managed == XRT_MANAGED_OK && got.host == XR_OS_IO_OK && got.present);
    prior = host_environment_slot(frame, RESULT);
    host_environment_fixture_set(key, "");
    host_environment_value(prior, sample);
    got = xrt_host_environment_get(frame, RESULT, billing, &backings, name);
    CHECK(got.managed == XRT_MANAGED_OK && got.host == XR_OS_IO_OK && got.present);
    prior = host_environment_slot(frame, RESULT);
    host_environment_value(prior, "");
    host_environment_fixture_set(key, NULL);
    got = xrt_host_environment_get(frame, RESULT, billing, &backings, name);
    CHECK(got.managed == XRT_MANAGED_OK && got.host == XR_OS_IO_OK && !got.present);
    CHECK(host_environment_slot(frame, RESULT) == prior);

    const char *invalid[] = {"", "invalid=name"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        CHECK(xrt_string_backing_create(frame, INVALID, &backings, invalid[i], strlen(invalid[i]), 0) == XRT_MANAGED_OK);
        got = xrt_host_environment_get(frame, RESULT, billing, &backings, host_environment_slot(frame, INVALID));
        CHECK(got.managed == XRT_MANAGED_OK && got.host == XR_OS_IO_OK && !got.present);
        CHECK(host_environment_slot(frame, RESULT) == prior);
    }
    CHECK(xrt_managed_frame_leave(root, frame) == XRT_MANAGED_OK);
    CHECK(xrt_managed_root_destroy(&root) == XRT_MANAGED_OK);
    xrt_backing_types_drop(&backings);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1);
    xrt_billing_owner_drop(billing);
    XrtLocationOwner *owner = locations("host-environment.xr");
    XrtValueRoot execution = {0};
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded_owned(owner, &failure, host_environment_publication, key, &execution) ==
        XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&execution));
    xrt_locations_release(owner);
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
    if (call->mode == 5) xrt_print_string(bytes->bytes,bytes->length,1);
    if (call->mode == 4 || call->mode >= 6) {
        XrtValueRoot inner_owner = {0}; XrtExecutionFailure failure = {0};
        ManagedCall inner = {.owner=&inner_owner,.locations=call->locations,.outer=root,
            .mode=call->mode == 6 ? 1u : call->mode == 7 ? 2u : call->mode == 8 ? 5u : 0u};
        SavedOutput saved = (SavedOutput)0;
        if (call->mode == 8) saved = disable_output();
        XrtExecutionStatus status = xrt_run_embedded_owned(call->locations,&failure,
            managed_root,&inner,&inner_owner);
        if (call->mode == 8) restore_output(saved);
        if (call->mode == 4) CHECK(status == XRT_EXECUTION_COMPLETED && inner.after == 1);
        else {
            CHECK(status == XRT_EXECUTION_FAILURE && !inner.after);
            if (call->mode == 6) CHECK(failure.kind == XRT_FAILURE_VALUE &&
                failure.detail.value.status == XR_XIR_VALUE_BAD_UTF8);
            if (call->mode == 7) CHECK(failure.kind == XRT_FAILURE_TYPED_PANIC &&
                failure.detail.panic.code == 422);
            if (call->mode == 8) CHECK(failure.kind == XRT_FAILURE_OUTPUT &&
                failure.detail.output.source_id == 1 && failure.detail.output.io.failed);
        }
        CHECK(xrt_value_root_empty(&inner_owner) && xrt_value_root_current(1) == call->owner &&
            xrt_native_managed_root(1) == root);
        XrtManagedHeader *preserved = NULL;
        managed_require(xrt_managed_slot_borrow(frame,1,&preserved));
        CHECK(preserved == joined && ((const XrtStringBacking *)preserved)->length == 10);
        managed_require(xrt_managed_slot_drop(frame,0));
        xrt_native_string_create(frame,0,"continued",9,1);
    }
    managed_require(xrt_managed_frame_leave(root,frame)); ++call->after;
}
static void managed_scopes(void) {
    XrtLocationOwner *map = locations("fixed-execution.xr");
    for (unsigned mode = 0; mode < 9; ++mode) {
        XrtValueRoot owner = {0}; XrtExecutionFailure failure = {0};
        ManagedCall call = {.owner=&owner,.locations=map,.mode=mode};
        SavedOutput saved = (SavedOutput)0;
        if (mode == 5) saved = disable_output();
        XrtExecutionStatus status = xrt_run_embedded_owned(map,&failure,managed_root,&call,&owner);
        if (mode == 5) restore_output(saved);
        CHECK(xrt_value_root_empty(&owner) && !xrt_value_root_active());
        if (mode == 1) CHECK(status == XRT_EXECUTION_FAILURE && !call.after && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_BAD_UTF8 && failure.detail.value.source_id == 1);
        else if (mode == 2) CHECK(status == XRT_EXECUTION_FAILURE && !call.after &&
            failure.kind == XRT_FAILURE_TYPED_PANIC && failure.detail.panic.code == 422);
        else if (mode == 5) CHECK(status == XRT_EXECUTION_FAILURE && !call.after &&
            failure.kind == XRT_FAILURE_OUTPUT && failure.detail.output.source_id == 1 &&
            failure.detail.output.operation == XRT_OUTPUT_WRITE && failure.detail.output.io.failed);
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
typedef struct MessageCall {
    XrtManagedHeader *taken;
} MessageCall;
static void message_root(void *context) {
    MessageCall *call = context;
    XrtManagedRoot *root = xrt_native_managed_root(1); XrtManagedFrame *frame = NULL;
    managed_require(xrt_managed_frame_enter(root, 1, &frame));
    xrt_native_string_create(frame, 0, "owned\0\xc3\xa9", 8, 1);
    managed_require(xrt_managed_slot_take(frame, 0, &call->taken));
    managed_require(xrt_managed_frame_leave(root, frame));
    xrt_message_panic_take(&call->taken, 434, 1);
}
static void managed_message_owner(void) {
    XrtLocationOwner *map = locations("message-owner.xr");
    for (size_t allowance = 0; allowance < 64; ++allowance) {
        XrtValueRoot owner = {0}; XrtBillingOwner *billing = NULL;
        XrtExecutionFailure failure = {0}; MessageCall call = {0};
        CHECK(xrt_native_values_prepare(&owner, &billing) == XRT_MANAGED_OK);
        CHECK(xrt_billing_owner_retain(billing));
#if defined(XR_NATIVE_VALUE_ALLOCATIONS)
        xrt_billing_owner_fail_after(billing, allowance);
#else
        (void)allowance;
#endif
        CHECK(xrt_run_embedded_owned(map, &failure, message_root, &call, &owner) == XRT_EXECUTION_FAILURE);
        CHECK(!call.taken && xrt_value_root_empty(&owner) && !xrt_value_root_active());
        bool completed = failure.kind == XRT_FAILURE_MESSAGE_PANIC;
        if (completed) {
            const XrtStringBacking *message = (const XrtStringBacking *)failure.detail.message.message;
            CHECK(failure.detail.message.code == 434 && failure.detail.message.source_id == 1 &&
                message && message->length == 8 && message->runes == 7 &&
                !memcmp(message->bytes, "owned\0\xc3\xa9", 8) &&
                xr_signed_rc_read(&message->managed.references) == 1);
        } else CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_OOM);
        xrt_execution_failure_drop(&failure); xrt_execution_failure_drop(&failure);
        CHECK(!failure.detail.message.message && xrt_billing_owner_stats(billing).blocks == 1);
        xrt_billing_owner_drop(billing);
        if (completed) { CHECK(allowance < 64); break; }
        CHECK(allowance + 1 < 64);
    }
    xrt_locations_release(map);
}
#include "execution_map_literals.inc.c"
#include "execution_environments.inc.c"
#include "execution_assertions.inc.c"
typedef struct StringBindingCall {
    XrtValueRoot *owner;
    XrtBillingOwner *billing;
    const XrtBackingTypes *foreign;
    size_t allowance, preparation_allocations;
    unsigned projections, completed;
    bool scan;
} StringBindingCall;
static void string_binding_root(void *context) {
    StringBindingCall *call = context;
    XrtManagedRoot *root = xrt_native_managed_root(1); XrtManagedFrame *frame = NULL;
    managed_require(xrt_managed_frame_enter(root, 4, &frame));
    size_t before = xrt_billing_owner_stats(call->billing).allocations;
    const XrtBackingTypes *types = call->foreign;
    if (!types) {
        if (call->scan) xrt_billing_owner_fail_after(call->billing, call->allowance);
        xrt_native_string_binding_prepare(1);
        call->preparation_allocations = xrt_billing_owner_stats(call->billing).allocations - before;
        CHECK(call->preparation_allocations);
        xrt_billing_owner_fail_after(call->billing, SIZE_MAX);
        managed_require(xrt_native_values_backings(call->owner, &types));
    }
    managed_require(xrt_string_backing_create(frame, 0, types, "ab", 2, 64));
    managed_require(xrt_string_backing_create(frame, 3, types, "z", 1, 0));
    before = xrt_billing_owner_stats(call->billing).allocations;
    xrt_billing_owner_fail_after(call->billing, 0);
    const XrtManagedHeader *left = NULL, *suffix = NULL;
    XrtPlace binding = {.frame=frame,.slot=0,.nulls=XRT_PLACE_NONNULL};
    managed_require(xrt_place_borrow(binding, &left));
    managed_require(xrt_place_borrow((XrtPlace){.frame=frame,.slot=3,.nulls=XRT_PLACE_NONNULL}, &suffix));
    ++call->projections;
    xrt_native_string_append_place(binding, frame, 2, (const XrtStringBacking *)suffix, 1);
    xrt_native_string_append_place(binding, frame, 2, (const XrtStringBacking *)left, 1);
    const XrtManagedHeader *after = NULL;
    managed_require(xrt_place_borrow(binding, &after));
    const XrtStringBacking *text = (const XrtStringBacking *)after;
    CHECK(after == left && text->length == 6 && !memcmp(text->bytes, "abzabz", 6) &&
        xr_signed_rc_read(&after->references) == 1);
    XrtBillingStats stats = xrt_billing_owner_stats(call->billing);
    CHECK(stats.allocations == before && !stats.fail_after);
    managed_require(xrt_managed_frame_leave(root, frame)); ++call->completed;
}
static void managed_string_bindings(void) {
    XrtLocationOwner *map = locations("string-binding.xr");
    XrtBillingOwner *foreign_billing = NULL; XrtBackingTypes foreign = {0};
    CHECK(xrt_billing_owner_new(NULL, SIZE_MAX, &foreign_billing) == XRT_MANAGED_OK);
    CHECK(xrt_backing_types_init(foreign_billing, &foreign) == XRT_MANAGED_OK);
    XrtBillingStats foreign_before = xrt_billing_owner_stats(foreign_billing);
    for (unsigned missing = 0; missing < 2; ++missing) {
        XrtValueRoot owner = {0}; XrtBillingOwner *billing = NULL; XrtExecutionFailure failure = {0};
        CHECK(xrt_native_values_prepare(&owner, &billing) == XRT_MANAGED_OK && xrt_billing_owner_retain(billing));
        StringBindingCall call = {.owner=&owner,.billing=billing,.foreign=missing ? &foreign : NULL};
        XrtExecutionStatus status = xrt_run_embedded_owned(map, &failure, string_binding_root, &call, &owner);
        CHECK(xrt_value_root_empty(&owner) && !xrt_value_root_active());
        if (missing) CHECK(status == XRT_EXECUTION_FAILURE && !call.completed && call.projections == 1 &&
            !call.preparation_allocations && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_BAD_ARGUMENT && failure.detail.value.source_id == 1);
        else CHECK(status == XRT_EXECUTION_COMPLETED && call.completed == 1 && call.preparation_allocations);
        CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
        XrtBillingStats stats = xrt_billing_owner_stats(foreign_billing);
        CHECK(stats.bytes == foreign_before.bytes && stats.blocks == foreign_before.blocks);
    }
#if defined(XR_NATIVE_VALUE_ALLOCATIONS)
    for (size_t allowance = 0; allowance < 64; ++allowance) {
        XrtValueRoot owner = {0}; XrtBillingOwner *billing = NULL; XrtExecutionFailure failure = {0};
        CHECK(xrt_native_values_prepare(&owner, &billing) == XRT_MANAGED_OK && xrt_billing_owner_retain(billing));
        StringBindingCall call = {.owner=&owner,.billing=billing,.allowance=allowance,.scan=true};
        XrtExecutionStatus status = xrt_run_embedded_owned(map, &failure, string_binding_root, &call, &owner);
        CHECK(xrt_value_root_empty(&owner) && !xrt_value_root_active() && xrt_billing_owner_stats(billing).blocks == 1);
        xrt_billing_owner_drop(billing);
        if (status == XRT_EXECUTION_COMPLETED) { CHECK(allowance && call.completed == 1); break; }
        CHECK(allowance + 1 < 64 && status == XRT_EXECUTION_FAILURE && !call.completed && !call.projections &&
            failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_OOM);
    }
#endif
    xrt_backing_types_drop(&foreign); xrt_billing_owner_drop(foreign_billing); xrt_locations_release(map);
}
static void state_string_root(void *context, XrtModuleState *state) {
    unsigned *completed = context;
    XrtManagedRoot *root = xrt_native_managed_root(1); XrtManagedFrame *frame = NULL;
    managed_require(xrt_managed_frame_enter(root,4,&frame));
    xrt_native_string_binding_prepare(1);
    const XrtBackingTypes *types = NULL;
    managed_require(xrt_native_values_backings(xrt_value_root_current(1), &types));
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
    xrt_native_string_create(frame,2,"!",1,1);
    XrtManagedHeader *suffix = NULL; managed_require(xrt_managed_slot_borrow(frame,2,&suffix));
    XrtPlace place = {.external=&source,.nulls=XRT_PLACE_NONNULL};
    xrt_native_string_append_place(place,frame,3,(const XrtStringBacking *)suffix,1);
    const XrtManagedHeader *current = NULL; managed_require(xrt_place_borrow(place,&current));
    CHECK(current != replacement && ((const XrtStringBacking *)current)->length == 4 &&
        !memcmp(((const XrtStringBacking *)current)->bytes,"new!",4));
    CHECK(((const XrtStringBacking *)replacement)->length == 3 && !memcmp(old->bytes,"state",5));
    XrtBillingOwner *billing = NULL;
    managed_require(xrt_native_values_prepare(xrt_value_root_current(1),&billing));
    xrt_billing_owner_fail_after(billing,0);
    size_t allocations = xrt_billing_owner_stats(billing).allocations;
    xrt_native_string_append_place(place,frame,3,(const XrtStringBacking *)current,1);
    const XrtManagedHeader *doubled = NULL; managed_require(xrt_place_borrow(place,&doubled));
    CHECK(doubled == current && ((const XrtStringBacking *)doubled)->length == 8 &&
        !memcmp(((const XrtStringBacking *)doubled)->bytes,"new!new!",8) &&
        xrt_billing_owner_stats(billing).allocations == allocations);
    managed_require(xrt_place_copy_replace((XrtPlace){.frame=frame,.slot=0,.nulls=XRT_PLACE_NONNULL},place,NULL));
    CHECK(xrt_string_backing_append_place(place,frame,3,types,(const XrtStringBacking *)suffix) == XRT_MANAGED_OOM);
    const XrtManagedHeader *preserved = NULL; managed_require(xrt_place_borrow(place,&preserved));
    CHECK(preserved == doubled && ((const XrtStringBacking *)preserved)->length == 8);
    XrtManagedHeader *escaped = NULL; managed_require(xrt_managed_slot_take(frame,0,&escaped));
    managed_require(xrt_managed_frame_leave(root,frame));
    xrt_module_state_clear(state);
    CHECK(((const XrtStringBacking *)escaped)->length == 8 &&
        !memcmp(((const XrtStringBacking *)escaped)->bytes,"new!new!",8) &&
        xr_signed_rc_read(&escaped->references) == 1);
    xrt_managed_drop(escaped); ++*completed;
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

static void checked_empty_array_root(void *context) {
    XrtBillingOwner *billing = context;
    XrtManagedRoot *root = NULL;
    xrt_billing_owner_fail_after(billing, 0);
    CHECK(xrt_native_managed_root_checked(&root) == XRT_MANAGED_OOM && !root);
    xrt_billing_owner_fail_after(billing, SIZE_MAX);
    managed_require(xrt_native_managed_root_checked(&root));
    CHECK(root && root == xrt_native_managed_root(1));
    CHECK(xrt_native_managed_root_checked(&root) == XRT_MANAGED_ARGUMENT);

    XrtManagedFrame *frame = NULL;
    managed_require(xrt_managed_frame_enter(root, 1, &frame));
    XrtManagedRoot *foreign = NULL;
    XrtManagedFrame *foreign_frame = NULL;
    managed_require(xrt_managed_root_new(billing, &foreign));
    managed_require(xrt_managed_frame_enter(foreign, 1, &foreign_frame));
    CHECK(xrt_native_array_empty_checked(foreign_frame, 0, XRT_BACKING_ARRAY_I64) == XRT_MANAGED_ARGUMENT);
    managed_require(xrt_managed_root_destroy(&foreign));

    const XrtBackingTypes *backings = NULL;
    managed_require(xrt_native_values_backings(xrt_value_root_active(), &backings));
    CHECK(backings);
    for (unsigned k = 0; k < XRT_BACKING_ARRAY_COUNT; ++k) {
        XrtBackingArrayKind kind = (XrtBackingArrayKind)k;
        XrtBillingStats before = xrt_billing_owner_stats(billing);
        XrtManagedHeader *owner = NULL;
        xrt_billing_owner_fail_after(billing, 0);
        CHECK(xrt_native_array_empty_checked(frame, 0, kind) == XRT_MANAGED_OOM);
        managed_require(xrt_managed_slot_borrow(frame, 0, &owner));
        CHECK(!owner && xrt_billing_owner_stats(billing).bytes == before.bytes);
        xrt_billing_owner_fail_after(billing, SIZE_MAX);
        managed_require(xrt_native_array_empty_checked(frame, 0, kind));
        managed_require(xrt_managed_slot_borrow(frame, 0, &owner));
        CHECK(xrt_backing_array_matches(owner, kind));
        const XrtArrayBacking *array = (const XrtArrayBacking *)owner;
        CHECK(!array->length && !array->capacity);
        CHECK(xrt_native_array_empty_checked(frame, 0, kind) == XRT_MANAGED_ARGUMENT);
        XrtManagedHeader *unchanged = NULL;
        managed_require(xrt_managed_slot_borrow(frame, 0, &unchanged));
        CHECK(unchanged == owner);
        managed_require(xrt_managed_slot_drop(frame, 0));
        CHECK(xrt_billing_owner_stats(billing).bytes == before.bytes);
    }

    static const size_t offsets[] = {0};
    static const XrtValueImageSlot fields[] = {{.type=1}};
    static const XrtValueImageType types[] = {
        {.kind=XRT_VALUE_STRING,.size=8,.alignment=8,.owned_count=1},
        {.kind=XRT_VALUE_TUPLE,.size=8,.alignment=8,.owned_count=1,.field_count=1},
        {.kind=XRT_VALUE_ARRAY_INLINE,.size=8,.alignment=8,.owned_count=1,.element=2}
    };
    static const XrtValueImage image = {.types=types,.type_count=3,.slots=fields,.slot_count=1,
        .owned_offsets=offsets,.owned_count=1};
    XrtBillingStats before = xrt_billing_owner_stats(billing);
    xrt_billing_owner_fail_after(billing, 1);
    CHECK(xrt_native_array_inline_empty_checked(frame, 0, &image, 3) == XRT_MANAGED_OOM);
    XrtManagedHeader *owner = NULL;
    managed_require(xrt_managed_slot_borrow(frame, 0, &owner));
    CHECK(!owner && xrt_billing_owner_stats(billing).bytes == before.bytes);
    xrt_billing_owner_fail_after(billing, SIZE_MAX);
    managed_require(xrt_native_array_inline_empty_checked(frame, 0, &image, 3));
    managed_require(xrt_managed_slot_borrow(frame, 0, &owner));
    CHECK(owner);
    const XrtArrayBacking *array = (const XrtArrayBacking *)owner;
    CHECK(!array->length && !array->capacity);
    CHECK(xrt_native_array_inline_empty_checked(frame, 0, &image, 1) == XRT_MANAGED_ARGUMENT);
    managed_require(xrt_managed_frame_leave(root, frame));
}

static void managed_checked_empty_arrays(void) {
    XrtManagedRoot *unbound = NULL;
    CHECK(xrt_native_managed_root_checked(&unbound) == XRT_MANAGED_ARGUMENT && !unbound);
    XrtLocationOwner *source = locations("array-empty.xr");
    XrtExecutionFailure failure = {0};
    XrtValueRoot value_root = {0};
    XrtBillingOwner *billing = NULL;
    managed_require(xrt_native_values_prepare(&value_root, &billing));
    CHECK(xrt_billing_owner_retain(billing));
    CHECK(xrt_run_embedded_owned(source, &failure, checked_empty_array_root, billing, &value_root) ==
        XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&value_root) && !xrt_value_root_active());
    CHECK(xrt_billing_owner_stats(billing).blocks == 1);
    xrt_billing_owner_drop(billing);
    xrt_locations_release(source);
}
int main(void) {
    managed_scopes();
    managed_checked_empty_arrays();
    managed_host_path_scratch();
    managed_metadata_retirement();
    managed_host_stat_control();
    managed_host_file();
    managed_host_file_read();
    managed_host_environment();
    managed_message_owner();
    managed_assertion_owner();
    managed_map_literals();
    managed_environments();
    managed_cells();
    managed_string_bindings();
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
