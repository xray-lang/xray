/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_temp_managed.inc.c - Owned host publication through a real execution frame
 */
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_host_temp.h"
#include "runtime/native/xnative_string_backing.h"
#include "runtime/native/xnative_values.h"
#include "xray_native_values.h"

typedef enum TempManagedCase {
    TEMP_MANAGED_CREATE, TEMP_MANAGED_MISSING, TEMP_MANAGED_COLLISION, TEMP_MANAGED_NUL, TEMP_MANAGED_OOM,
    TEMP_MANAGED_CANCEL_BEFORE, TEMP_MANAGED_CANCEL_AFTER, TEMP_MANAGED_KEEP_FAILURE,
    TEMP_MANAGED_CANCEL_RETRY, TEMP_MANAGED_UNSUPPORTED
} TempManagedCase;

typedef struct TempManagedWitness {
    const char *path;
    size_t length;
    TempManagedCase mode;
    XrtHostTempRetry *retry;
    XrtBillingOwner *billing;
    unsigned current_checks;
    bool checked;
#if defined(XR_OS_WINDOWS)
    wchar_t created[32768];
    HANDLE retained_handle;
#endif
} TempManagedWitness;

static bool temp_managed_current(void *context) {
    TempManagedWitness *witness = context;
    ++witness->current_checks;
    return witness->mode != TEMP_MANAGED_CANCEL_BEFORE && !metadata_memory.cancelled;
}

static int temp_managed_operation(TempManagedWitness *witness) {
    enum { RESULT, SCRATCH, PATH, SLOT_COUNT };
    const size_t owner = offsetof(XrtHostNullableOwner, owner);
    const size_t offsets[] = {owner, sizeof(XrtHostNullableOwner) + owner, SIZE_MAX};
    const XrtManagedFrameSpec spec = {SLOT_COUNT, 2 * sizeof(XrtHostNullableOwner),
        _Alignof(XrtHostNullableOwner), offsets};
    XrtManagedRoot *root = xrt_native_managed_root(1);
    XrtManagedFrame *frame = NULL;
    void *payload = NULL;
    CHECK(xrt_managed_frame_enter_payload(root, &spec, &frame, &payload) == XRT_MANAGED_OK);
    const size_t length = witness->length ? witness->length : strlen(witness->path);
    xrt_native_string_create(frame, PATH, witness->path, length, 1);
    xrt_native_string_create(frame, RESULT, "before", 6, 1);
    uint8_t *tag = payload;
    *tag = 1;
    const XrtHostNullableSlots output = {frame, RESULT, SCRATCH, 0, offsets[SCRATCH]};
    XrtPlace result_place = {.frame=frame,.slot=RESULT,.nulls=XRT_PLACE_NULLABLE};
    XrtPlace path_place = {.frame=frame,.slot=PATH,.nulls=XRT_PLACE_NONNULL};
    const XrtManagedHeader *old = NULL, *path = NULL;
    CHECK(xrt_place_borrow(result_place, &old) == XRT_MANAGED_OK && old);
    CHECK(xrt_place_borrow(path_place, &path) == XRT_MANAGED_OK && path);
    XrtBillingOwner *billing = NULL;
    CHECK(xrt_native_values_prepare(xrt_value_root_current(1), &billing) == XRT_MANAGED_OK);
    const XrtBillingStats before = xrt_billing_owner_stats(billing);
    const bool repeat = witness->mode == TEMP_MANAGED_KEEP_FAILURE || witness->mode == TEMP_MANAGED_CANCEL_RETRY;

#if defined(XR_OS_WINDOWS)
    const unsigned calls = temp_create_calls;
    if (witness->mode == TEMP_MANAGED_CREATE) {
        const XrtHostNullableSlots wrong = {frame, RESULT, SCRATCH, 0, offsets[RESULT]};
        XrtHostTempResult rejected = xrt_host_temp_directory_publish(&wrong, path, NULL, NULL, 1, &witness->retry);
        CHECK(rejected.managed == XRT_MANAGED_ARGUMENT && !witness->retry && temp_create_calls == calls);
    }
    if (witness->mode == TEMP_MANAGED_OOM) temp_fail_preparation = billing;
    if (witness->mode == TEMP_MANAGED_CANCEL_AFTER || witness->mode == TEMP_MANAGED_CANCEL_RETRY) temp_cancel_created = true;
    if (witness->mode == TEMP_MANAGED_UNSUPPORTED) temp_missing_api = true;
    if (repeat) {
        temp_fail_parent_on_create = witness->mode == TEMP_MANAGED_KEEP_FAILURE;
        temp_fail_delete = temp_fail_parent_on_create ? 2 : 3;
        if (temp_fail_parent_on_create) {
            CHECK(xrt_billing_owner_retain(billing));
            witness->billing = billing;
        }
    }
#endif
    XrtHostTempResult result = xrt_host_temp_directory_publish(&output, path,
        temp_managed_current, witness, 1, &witness->retry);
#if defined(XR_OS_WINDOWS)
    xrt_billing_owner_fail_after(billing, SIZE_MAX);
    temp_fail_preparation = NULL;
    temp_cancel_created = temp_fail_parent_on_create = temp_missing_api = false;
    metadata_memory.cancelled = false;
#endif
    const XrtManagedHeader *published = NULL, *scratch = NULL;
    CHECK(xrt_place_borrow(result_place, &published) == XRT_MANAGED_OK);
    CHECK(xrt_place_borrow((XrtPlace){.frame=frame,.slot=SCRATCH,.nulls=XRT_PLACE_NULLABLE},
        &scratch) == XRT_MANAGED_OK && !scratch);
    CHECK(result.present == (witness->mode == TEMP_MANAGED_CREATE));
    XrtManagedStatus managed = witness->mode == TEMP_MANAGED_OOM ? XRT_MANAGED_OOM : XRT_MANAGED_OK;
    const XrOsIoStatus expected[] = {XR_OS_IO_OK, XR_OS_IO_OK, XR_OS_IO_OK, XR_OS_IO_BAD_ARGUMENT, XR_OS_IO_OUT_OF_MEMORY,
        XR_OS_IO_CANCELLED, XR_OS_IO_CANCELLED, XR_OS_IO_IO, XR_OS_IO_CANCELLED, XR_OS_IO_UNSUPPORTED};
    CHECK(result.managed == managed && result.host == expected[witness->mode]);
    CHECK(result.rollback == (repeat ? XR_OS_IO_IO : XR_OS_IO_OK));
    CHECK((witness->retry != NULL) == repeat);
    if (witness->mode == TEMP_MANAGED_MISSING || witness->mode == TEMP_MANAGED_COLLISION) {
        CHECK(!*tag && !published);
    } else if (result.present) {
#if defined(XR_OS_WINDOWS)
        CHECK(*tag == 1 && xrt_backing_string_matches(published));
        const XrtStringBacking *created = (const XrtStringBacking *)published;
        const size_t prefix = strlen(witness->path);
        CHECK(created->length > prefix && !memcmp(created->bytes, witness->path, prefix));
        CHECK(temp_locator(created->bytes, witness->created));
        CHECK(GetFileAttributesW(witness->created) != INVALID_FILE_ATTRIBUTES);
        DWORD flags = 0;
        CHECK(!GetHandleInformation(temp_parent, &flags) && !GetHandleInformation(temp_child, &flags));
#endif
    } else CHECK(*tag == 1 && published == old);
    if (result.host != XR_OS_IO_OK && !witness->retry) {
        const XrtBillingStats after = xrt_billing_owner_stats(billing);
        CHECK(after.blocks == before.blocks && after.bytes == before.bytes);
    }
    if (witness->mode == TEMP_MANAGED_UNSUPPORTED) CHECK(!witness->current_checks);
#if defined(XR_OS_WINDOWS)
    unsigned attempted = 0;
    if (witness->mode == TEMP_MANAGED_COLLISION) attempted = 32;
    if (result.present || witness->mode == TEMP_MANAGED_CANCEL_AFTER || witness->retry) attempted = 1;
    CHECK(temp_create_calls == calls + attempted);
    if (witness->mode == TEMP_MANAGED_CANCEL_AFTER)
        CHECK(temp_cancelled_path[0] && GetFileAttributesW(temp_cancelled_path) == INVALID_FILE_ATTRIBUTES);
    if (witness->retry) {
        witness->retained_handle = temp_child;
        DWORD selected = GetFinalPathNameByHandleW(temp_child, witness->created, 32768, FILE_NAME_NORMALIZED);
        CHECK(selected && selected < 32768 && GetFileAttributesW(witness->created) != INVALID_FILE_ATTRIBUTES);
    }
#else
    CHECK(!xr_os_io_temp_available());
#endif
    CHECK(xrt_managed_frame_leave(root, frame) == XRT_MANAGED_OK);
    return 1;
}

static void temp_managed_execution(void *context) {
    TempManagedWitness *witness = context;
    witness->checked = temp_managed_operation(witness) != 0;
    if (witness->checked && witness->retry) xrt_value_failure(XR_XIR_VALUE_IO, 1);
}

static int temp_managed_run(TempManagedWitness *witness) {
    const XrtSourceLocation location = {"temp.xr", 7, 3, 7, 3, 11};
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(&location, 1, &locations) == XRT_LOCATION_OK);
    XrtExecutionFailure failure = {0};
    XrtExecutionStatus outcome = xrt_run_embedded(locations, &failure, temp_managed_execution, witness);
    xrt_locations_release(locations);
    CHECK(witness->checked);
    if (witness->retry) {
        CHECK(outcome == XRT_EXECUTION_FAILURE && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_IO && failure.detail.value.source_id == 1);
    } else CHECK(outcome == XRT_EXECUTION_COMPLETED);
    xrt_execution_failure_drop(&failure);
    return 1;
}

#if defined(XR_OS_WINDOWS)
static int temp_managed_retry(TempManagedWitness *witness) {
    CHECK(witness->retry);
    DWORD flags = 0;
    CHECK(GetHandleInformation(witness->retained_handle, &flags));
    XrtHostTempRetry *saved = witness->retry;
    CHECK(xrt_host_temp_retry(&witness->retry) == XR_OS_IO_IO && witness->retry == saved);
    CHECK(GetHandleInformation(witness->retained_handle, &flags));
    CHECK(GetFileAttributesW(witness->created) != INVALID_FILE_ATTRIBUTES);
    CHECK(xrt_host_temp_retry(&witness->retry) == XR_OS_IO_OK && !witness->retry);
    CHECK(!GetHandleInformation(witness->retained_handle, &flags));
    CHECK(GetFileAttributesW(witness->created) == INVALID_FILE_ATTRIBUTES);
    CHECK(xrt_host_temp_retry(&witness->retry) == XR_OS_IO_OK);
    if (witness->billing) {
        CHECK(xrt_billing_owner_stats(witness->billing).blocks == 1);
        xrt_billing_owner_drop(witness->billing);
    }
    return 1;
}

static int temp_managed_windows(const char *root) {
    char long_root[65536], missing[65536], with_nul[65536];
    CHECK(xr_win_utf16_to_utf8(temp_long_root_path, wcslen(temp_long_root_path), long_root, sizeof(long_root)));
    int missing_length = snprintf(missing, sizeof(missing), "%s\\missing-temp-root", root);
    CHECK(missing_length > 0 && (size_t)missing_length < sizeof(missing));
    const char tail[] = "\0ignored";
    const size_t root_length = strlen(root);
    CHECK(root_length <= sizeof(with_nul) - sizeof(tail));
    memcpy(with_nul, root, root_length);
    memcpy(with_nul + root_length, tail, sizeof(tail));
    wchar_t collision[32768];
    CHECK(wide_join(collision, paths.child, L"xr-00000000000000000000000000000000") &&
        directory_operation(collision, true));
    TempManagedWitness witness = {0};
    for (TempManagedCase mode = TEMP_MANAGED_CREATE; mode <= TEMP_MANAGED_UNSUPPORTED; ++mode) {
        witness = (TempManagedWitness){.mode=mode,.path=root};
        if (mode == TEMP_MANAGED_CREATE) witness.path = long_root;
        if (mode == TEMP_MANAGED_MISSING) witness.path = missing;
        if (mode == TEMP_MANAGED_NUL) {
            witness.path = with_nul;
            witness.length = root_length + sizeof(tail) - 1;
        }
        if (mode == TEMP_MANAGED_COLLISION) temp_entropy_mode = 2;
        const int passed = temp_managed_run(&witness);
        temp_entropy_mode = 0;
        CHECK(passed);
        if (mode == TEMP_MANAGED_CREATE) {
            CHECK(GetFileAttributesW(witness.created) != INVALID_FILE_ATTRIBUTES);
            CHECK(RemoveDirectoryW(witness.created));
        }
        if (witness.retry) CHECK(temp_managed_retry(&witness));
    }
    CHECK(directory_operation(collision, false));
    puts("managed temp directory: Unicode publication, cancellation and caller-owned rollback across execution retirement");
    return 1;
}
#else
static int temp_managed_unix(void) {
    TempManagedWitness witness = {.path="/tmp",.mode=TEMP_MANAGED_UNSUPPORTED};
    return temp_managed_run(&witness);
}
#endif
