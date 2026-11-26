/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_host_directory.c - Directory ownership through execution retirement
 */
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_host_directory.h"
#include "runtime/native/xnative_values.h"
#include "xray_native_values.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "native directory owner:%d: %s\n", __LINE__, #test); \
    exit(1); \
} } while (0)

static XrtLocationOwner *locations;
static const XrOsPathSpan current_path = {".", 1};

static bool empty_name(const XrtHostDirectoryName *name) {
    return !name->text.bytes && !name->text.length && !name->billing;
}

#if defined(XR_OS_WINDOWS) || defined(XR_OS_LINUX) || defined(XR_OS_MACOS)
enum { NEXT_SOURCE = 1, CLOSE_SOURCE, POLL_SOURCE };
enum { RESULT_SLOT, STAGE_SLOT, SLOT_COUNT };

static XrtNativeDirectoryNext checked_receiving_frame(XrtHostNullableOwner **destination) {
    static const size_t owner = offsetof(XrtHostNullableOwner, owner);
    static const size_t owned[] = {0, offsetof(XrtHostNullableOwner, owner)};
    static const XrtValueImageType types[] = {
        {.kind=XRT_VALUE_STRING,.size=sizeof(void *),.alignment=_Alignof(void *),.owned_count=1},
        {.kind=XRT_VALUE_NULLABLE,.size=sizeof(XrtHostNullableOwner),.alignment=_Alignof(XrtHostNullableOwner),
            .owned_count=1,.first_owned=1,.nullable_element=1,.nullable_payload=offsetof(XrtHostNullableOwner, owner)}
    };
    static const XrtValueImage image = {.types=types,.type_count=2,.owned_offsets=owned,.owned_count=2};
    size_t offsets[] = {owner, sizeof(XrtHostNullableOwner)};
    const XrtManagedFrameSpec spec = {SLOT_COUNT, sizeof(XrtHostNullableOwner) + sizeof(XrtManagedHeader *),
        _Alignof(XrtHostNullableOwner), offsets};
    XrtManagedRoot *root = xrt_native_managed_root(NEXT_SOURCE);
    XrtManagedFrame *frame = NULL;
    void *payload = NULL;
    CHECK(xrt_managed_frame_enter_payload(root, &spec, &frame, &payload) == XRT_MANAGED_OK);
    *destination = payload;
    return (XrtNativeDirectoryNext){frame, RESULT_SLOT, STAGE_SLOT, 0, offsets[STAGE_SLOT], &image, 2, 0};
}

static void checked_empty_staging(const XrtNativeDirectoryNext *call) {
    XrtManagedHeader *staged = NULL;
    CHECK(xrt_managed_slot_borrow(call->frame, call->temporary, &staged) == XRT_MANAGED_OK && !staged);
}

typedef struct CheckedFailure {
    XrtNativeDirectoryStatus primary, secondary;
} CheckedFailure;

static void checked_invalid_read(void *context) {
    CheckedFailure *test = context;
    XrtHostNullableOwner *destination = NULL;
    XrtNativeDirectoryNext call = checked_receiving_frame(&destination);
    int64_t alive = 0;
    CHECK(xrt_host_directory_open(current_path, &alive) == XR_OS_IO_OK && alive > 0);
    test->primary = xrt_native_host_directory_next_checked(&call, NEXT_SOURCE);
    CHECK(test->primary && !destination->present && !destination->owner);
    checked_empty_staging(&call);
    CHECK(!xrt_native_host_directory_cleanup_close_checked(alive, CLOSE_SOURCE));
    test->secondary = xrt_native_host_directory_cleanup_close_checked(alive, CLOSE_SOURCE);
    CHECK(test->secondary);
    xrt_native_host_directory_cleanup_fail(test->primary, test->secondary);
}

static void exercise_checked_failures(void) {
    XrtValueRoot values = {0};
    XrtExecutionFailure failure = {0};
    CheckedFailure test = {0};
    CHECK(xrt_run_embedded_owned(locations, &failure, checked_invalid_read, &test, &values) == XRT_EXECUTION_FAILURE);
    CHECK(xrt_value_root_empty(&values) && failure.kind == XRT_FAILURE_VALUE);
    const XrtValueFailure *value = &failure.detail.value;
    CHECK(value->status == XR_XIR_VALUE_BAD_ARGUMENT && value->source_id == NEXT_SOURCE &&
        value->directory_close_source_id == CLOSE_SOURCE && value->directory_primary == test.primary &&
        value->directory_secondary == test.secondary);
    xrt_execution_failure_drop(&failure);
}
#endif

#if defined(XR_OS_WINDOWS)
#include "base/xwindows_utf8.h"

static bool cancelled, cancel_after_open, cancel_after_close;
static bool cancel_after_batch, cancel_after_end;
static unsigned opened_handles, live_handles, close_calls, failed_closes, polls;
static unsigned directory_queries, successful_batches, completed_queries;
static HANDLE failed_handle;
static int64_t busy_cleanup_token;
static XrOsPathSpan executable_path;

static HANDLE WINAPI tracked_open(LPCWSTR path, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE template_file) {
    HANDLE handle = CreateFileW(path, access, share, security, creation, flags, template_file);
    if (handle != INVALID_HANDLE_VALUE) {
        ++opened_handles;
        ++live_handles;
        if (cancel_after_open) cancelled = true;
    }
    return handle;
}

static BOOL WINAPI tracked_close(HANDLE handle) {
    ++close_calls;
    if (busy_cleanup_token) {
        int64_t token = busy_cleanup_token;
        busy_cleanup_token = 0;
        unsigned attempts = close_calls;
        bool closed = true;
        CHECK(xrt_host_directory_cleanup_close(token, &closed) == XR_OS_IO_BAD_ARGUMENT && closed);
        CHECK(close_calls == attempts);
    }
    if (failed_handle) CHECK(failed_handle == handle);
    if (failed_closes) {
        --failed_closes;
        failed_handle = handle;
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    CHECK(live_handles && CloseHandle(handle));
    failed_handle = NULL;
    --live_handles;
    if (cancel_after_close) cancelled = true;
    return TRUE;
}

static BOOL WINAPI tracked_information(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS kind,
    LPVOID output, DWORD bytes) {
    BOOL result = GetFileInformationByHandleEx(handle, kind, output, bytes);
    if (kind == FileIdBothDirectoryInfo || kind == FileIdBothDirectoryRestartInfo) {
        ++directory_queries;
        if (result) {
            ++successful_batches;
            if (cancel_after_batch) cancelled = true;
        } else if (GetLastError() == ERROR_NO_MORE_FILES) {
            ++completed_queries;
            if (cancel_after_end) cancelled = true;
        }
    }
    return result;
}

/* Exercise the actual platform implementation. A refusal immediately before
 * CloseHandle leaves the real handle held for a deterministic retry. */
#define CreateFileW tracked_open
#define CloseHandle tracked_close
#define GetFileInformationByHandleEx tracked_information
#include "os/win/directory_owned_win.c"
#undef GetFileInformationByHandleEx
#undef CloseHandle
#undef CreateFileW

static bool request_current(void *unused) {
    (void)unused;
    ++polls;
    return !cancelled;
}

static int64_t open_directory_path(XrOsPathSpan path) {
    int64_t token = -1;
    CHECK(xrt_host_directory_open(path, &token) == XR_OS_IO_OK && token > 0);
    return token;
}

static int64_t open_directory(void) { return open_directory_path(current_path); }

static void invalid_token(int64_t token) {
    bool closed = true;
    XrtHostDirectoryName name = {0};
    unsigned attempts = close_calls;
    unsigned queries = directory_queries;
    CHECK(xrt_host_directory_close(token, &closed) == XR_OS_IO_BAD_ARGUMENT && closed);
    CHECK(xrt_host_directory_cleanup_close(token, &closed) == XR_OS_IO_BAD_ARGUMENT && closed);
    CHECK(xrt_host_directory_next(token, &name) == XR_OS_IO_BAD_ARGUMENT && empty_name(&name));
    CHECK(close_calls == attempts);
    CHECK(directory_queries == queries);
}

static void foreign_root(void *context) {
    int64_t foreign = *(const int64_t *)context;
    invalid_token(foreign);
    CHECK(open_directory() > foreign && live_handles == 2);
}

static void generation(void *context) {
    int64_t *previous = context;
    invalid_token(*previous);
    invalid_token(0);
    int64_t first = open_directory();
    CHECK(first > *previous && live_handles == 1);
    *previous = first;

    XrtValueRoot nested = {0};
    XrtExecutionFailure nested_failure = {0};
    CHECK(xrt_run_embedded_owned(locations, &nested_failure, foreign_root, &first, &nested) ==
        XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&nested) && live_handles == 1);

    failed_closes = 1;
    bool closed = true;
    CHECK(xrt_host_directory_close(first, &closed) == XR_OS_IO_OK && !closed);
    CHECK(live_handles == 1 && failed_handle);
    CHECK(xrt_host_directory_close(first, &closed) == XR_OS_IO_OK && closed);
    CHECK(!live_handles && !failed_handle);
    invalid_token(first);
}

static void open_refusal(void *context) {
    (void)context;
    int64_t token = -1;
    CHECK(xrt_host_directory_open(executable_path, &token) == XR_OS_IO_OK && !token);
    CHECK(!live_handles);
    static const char bad[] = {'a', '\0', 'b'};
    token = -1;
    CHECK(xrt_host_directory_open((XrOsPathSpan){bad, sizeof(bad)}, &token) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(token == -1 && !live_handles);
}

static void quota(void *context) {
    (void)context;
    unsigned before = opened_handles;
    int64_t token = -1;
    CHECK(xrt_host_directory_open(current_path, &token) == XR_OS_IO_MEMORY_LIMIT);
    CHECK(token == -1 && opened_handles == before);
}

static void fail_retirement(void *context) {
    (void)context;
    (void)open_directory();
    (void)open_directory();
    failed_closes = 2;
}

static void unopened_cancel(void *context) {
    (void)context;
    int64_t token = -1;
    cancelled = true;
    unsigned before = opened_handles;
    CHECK(xrt_host_directory_open(current_path, &token) == XR_OS_IO_CANCELLED);
    CHECK(token == -1 && opened_handles == before && !live_handles);
}

static void unpublished_cancel(void *context) {
    (void)context;
    int64_t token = -1;
    cancel_after_open = true;
    failed_closes = 2;
    CHECK(xrt_host_directory_open(current_path, &token) == XR_OS_IO_CANCELLED);
    CHECK(token == -1 && live_handles == 1 && failed_handle);
    xrt_value_failure(XR_XIR_VALUE_CANCELLED, 0);
}

static void close_cancel(void *context) {
    (void)context;
    int64_t token = open_directory();
    bool closed = false;
    cancel_after_close = true;
    CHECK(xrt_host_directory_close(token, &closed) == XR_OS_IO_CANCELLED && !closed);
    CHECK(!live_handles);
    invalid_token(token);
    xrt_value_failure(XR_XIR_VALUE_CANCELLED, 0);
}

static void cancel_before_close(void *context) {
    (void)context;
    int64_t token = open_directory();
    bool closed = false;
    unsigned attempts = close_calls;
    cancelled = true;
    CHECK(xrt_host_directory_close(token, &closed) == XR_OS_IO_CANCELLED && !closed);
    CHECK(live_handles == 1 && close_calls == attempts);
    cancelled = false;
    CHECK(xrt_host_directory_close(token, &closed) == XR_OS_IO_OK && closed);
    CHECK(!live_handles && close_calls == attempts + 1);
}

typedef struct CleanupCancellation {
    bool before_close;
    unsigned polls_before_cleanup;
} CleanupCancellation;

static void cleanup_cancel(void *context) {
    CleanupCancellation *test = context;
    int64_t first = open_directory();
    int64_t second = open_directory();
    CHECK(first != second && live_handles == 2);
    test->polls_before_cleanup = polls;
    unsigned attempts = close_calls;
    cancelled = test->before_close;
    cancel_after_close = !test->before_close;
    busy_cleanup_token = second;
    xrt_native_host_directory_cleanup_close(second, 0);
    CHECK(cancelled && !busy_cleanup_token && polls == test->polls_before_cleanup && live_handles == 1);
    xrt_native_host_directory_cleanup_close(first, 0);
    CHECK(polls == test->polls_before_cleanup && !live_handles && close_calls == attempts + 2);
    xrt_native_host_directory_cleanup_poll(0);
    CHECK(false);
}

static void cleanup_os_refusal(void *context) {
    (void)context;
    int64_t token = open_directory();
    failed_closes = 1;
    unsigned attempts = close_calls, previous_polls = polls;
    CHECK(!xrt_native_host_directory_cleanup_close_checked(token, CLOSE_SOURCE));
    CHECK(live_handles == 1 && failed_handle && close_calls == attempts + 1 && polls == previous_polls);
    xrt_native_host_directory_cleanup_poll(0);
    CHECK(polls == previous_polls + 1 && close_calls == attempts + 1);
}

typedef struct CloseOrder {
    bool add_on_poll;
    int64_t added;
} CloseOrder;

static bool open_during_poll(void *context) {
    CloseOrder *order = context;
    if (order->add_on_poll) {
        order->add_on_poll = false;
        order->added = open_directory();
    }
    return true;
}

static void close_original_directory(void *context) {
    CloseOrder *order = context;
    int64_t original = open_directory();
    order->add_on_poll = true;
    bool closed = false;
    CHECK(xrt_host_directory_close(original, &closed) == XR_OS_IO_OK && closed);
    CHECK(order->added > original && live_handles == 1);

    // The callback added a real directory before the original close began.
    closed = false;
    CHECK(xrt_host_directory_close(order->added, &closed) == XR_OS_IO_OK && closed);
    CHECK(!live_handles);
    invalid_token(original);
}

enum { directory_file_count = 72, directory_unicode_units = 120 };
typedef struct DirectoryNames {
    XrOsPathSpan path;
    char unicode[3 * directory_unicode_units + 1];
    XrtHostDirectoryName retained[directory_file_count + 1];
} DirectoryNames;

static unsigned directory_name_index(const DirectoryNames *names, const XrOsDirectoryText *text) {
    if (text->length == sizeof(names->unicode) - 1) {
        CHECK(!memcmp(text->bytes, names->unicode, sizeof(names->unicode)));
        return directory_file_count;
    }
    CHECK(text->length == 7 && !memcmp(text->bytes, "item-", 5) && !text->bytes[7]);
    char tens = text->bytes[5], ones = text->bytes[6];
    CHECK(tens >= '0' && tens <= '9' && ones >= '0' && ones <= '9');
    unsigned index = (unsigned)(tens - '0') * 10 + (unsigned)(ones - '0');
    CHECK(index < directory_file_count);
    return index;
}

static void read_names(void *context) {
    DirectoryNames *names = context;
    int64_t token = open_directory_path(names->path);
    unsigned batches = successful_batches, completions = completed_queries;
    bool check_output = true;
    for (;;) {
        XrtHostDirectoryName name = {0};
        XrOsIoStatus status = xrt_host_directory_next(token, &name);
        if (status == XR_OS_IO_END) {
            CHECK(empty_name(&name));
            break;
        }
        CHECK(status == XR_OS_IO_OK && name.text.bytes && name.billing);
        unsigned index = directory_name_index(names, &name.text);
        CHECK(empty_name(&names->retained[index]));
        names->retained[index] = name;
        if (check_output) {
            CHECK(xrt_host_directory_next(token, &names->retained[index]) == XR_OS_IO_BAD_ARGUMENT);
            CHECK(names->retained[index].text.bytes == name.text.bytes &&
                names->retained[index].text.length == name.text.length && names->retained[index].billing == name.billing);
            check_output = false;
        }
    }

    CHECK(successful_batches >= batches + 2 && completed_queries == completions + 1);
    unsigned queries = directory_queries;
    XrtHostDirectoryName terminal = {0};
    CHECK(xrt_host_directory_next(token, &terminal) == XR_OS_IO_END && empty_name(&terminal));
    CHECK(directory_queries == queries);

    bool closed = false;
    CHECK(xrt_host_directory_close(token, &closed) == XR_OS_IO_OK && closed);
    invalid_token(token);
}

static void cancel_next(void *context) {
    const DirectoryNames *names = context;
    int64_t token = open_directory_path(names->path);
    XrtHostDirectoryName name = {0};
    unsigned batches = successful_batches;
    cancel_after_batch = true;
    CHECK(xrt_host_directory_next(token, &name) == XR_OS_IO_CANCELLED && empty_name(&name));
    CHECK(successful_batches == batches + 1 && live_handles == 1);
    cancel_after_batch = cancelled = false;
    unsigned queries = directory_queries;
    CHECK(xrt_host_directory_next(token, &name) == XR_OS_IO_CANCELLED && empty_name(&name));
    CHECK(directory_queries == queries);
}

static void cancel_next_at_end(void *context) {
    const DirectoryNames *names = context;
    int64_t token = open_directory_path(names->path);
    XrtHostDirectoryName name = {0};
    unsigned completions = completed_queries;
    cancel_after_end = true;
    CHECK(xrt_host_directory_next(token, &name) == XR_OS_IO_CANCELLED && empty_name(&name));
    CHECK(completed_queries == completions + 1 && live_handles == 1);

    cancel_after_end = cancelled = false;
    unsigned queries = directory_queries;
    CHECK(xrt_host_directory_next(token, &name) == XR_OS_IO_END && empty_name(&name));
    CHECK(directory_queries == queries);
}

static void next_allocation_failure(void *context) {
    const DirectoryNames *names = context;
    int64_t token = open_directory_path(names->path);
    XrtHostDirectoryName name = {0};
    CHECK(xrt_host_directory_next(token, &name) == XR_OS_IO_OK && name.billing);
    XrtBillingOwner *billing = name.billing;
    xrt_host_directory_name_drop(&name);
    CHECK(empty_name(&name));
    unsigned queries = directory_queries;
    xrt_billing_owner_fail_after(billing, 0);
    CHECK(xrt_host_directory_next(token, &name) == XR_OS_IO_OUT_OF_MEMORY && empty_name(&name));
    xrt_billing_owner_fail_after(billing, SIZE_MAX);
    CHECK(xrt_host_directory_next(token, &name) == XR_OS_IO_OUT_OF_MEMORY && empty_name(&name));
    CHECK(directory_queries == queries && live_handles == 1);
}

typedef struct CheckedRead {
    const DirectoryNames *names;
    XrtNativeDirectoryStatus failure;
} CheckedRead;

static XrtBillingOwner *checked_billing(void) {
    XrtBillingOwner *billing = NULL;
    CHECK(xrt_native_values_prepare(xrt_value_root_active(), &billing) == XRT_MANAGED_OK && billing);
    return billing;
}

static void checked_read_prepare_failure(void *context) {
    CheckedRead *test = context;
    XrtHostNullableOwner *destination = NULL;
    XrtNativeDirectoryNext call = checked_receiving_frame(&destination);
    call.token = open_directory_path(test->names->path);
    XrtBillingOwner *billing = checked_billing();
    unsigned queries = directory_queries;
    xrt_billing_owner_fail_after(billing, 0);
    test->failure = xrt_native_host_directory_next_checked(&call, NEXT_SOURCE);
    xrt_billing_owner_fail_after(billing, SIZE_MAX);
    CHECK(test->failure && directory_queries == queries && !destination->present && !destination->owner);
    checked_empty_staging(&call);
    CHECK(!xrt_native_host_directory_cleanup_close_checked(call.token, CLOSE_SOURCE) && !live_handles);
    xrt_native_host_directory_cleanup_fail(test->failure, 0);
}

static void checked_read_prepare_retry(void *context) {
    const DirectoryNames *names = context;
    XrtHostNullableOwner *destination = NULL;
    XrtNativeDirectoryNext call = checked_receiving_frame(&destination);
    call.token = open_directory_path(names->path);
    XrtBillingOwner *billing = checked_billing();
    XrtBillingStats before = xrt_billing_owner_stats(billing);
    unsigned queries = directory_queries;
    xrt_billing_owner_fail_after(billing, 1); /* Allocate the cache node, then fail to bind the image. */
    CHECK(xrt_native_host_directory_next_checked(&call, NEXT_SOURCE));
    xrt_billing_owner_fail_after(billing, SIZE_MAX);
    CHECK(directory_queries == queries && !destination->present && !destination->owner);
    checked_empty_staging(&call);
    XrtBillingStats after = xrt_billing_owner_stats(billing);
    CHECK(!xrt_native_host_directory_next_checked(&call, NEXT_SOURCE) && destination->present && destination->owner);
    CHECK(after.blocks == before.blocks && after.bytes == before.bytes);
    checked_empty_staging(&call);
    CHECK(!xrt_native_host_directory_cleanup_close_checked(call.token, CLOSE_SOURCE) && !live_handles);
}

static void checked_read_backing_failure(void *context) {
    CheckedRead *test = context;
    XrtHostNullableOwner *destination = NULL;
    XrtNativeDirectoryNext call = checked_receiving_frame(&destination);
    call.token = open_directory_path(test->names->path);
    CHECK(!xrt_native_host_directory_next_checked(&call, NEXT_SOURCE) && destination->present == 1);
    XrtManagedHeader *first = destination->owner;
    CHECK(first);
    checked_empty_staging(&call);
    XrtBillingOwner *billing = checked_billing();
    XrtBillingStats before = xrt_billing_owner_stats(billing);
    unsigned queries = directory_queries;
    XrtNativeDirectoryNext wrong_stage = call;
    wrong_stage.temporary_owner_offset = offsetof(XrtHostNullableOwner, owner);
    CHECK(xrt_native_host_directory_next_checked(&wrong_stage, NEXT_SOURCE) && directory_queries == queries);
    CHECK(destination->present == 1 && destination->owner == first);
    xrt_billing_owner_fail_after(billing, 1); /* The warm provider allocates the name first. */
    test->failure = xrt_native_host_directory_next_checked(&call, NEXT_SOURCE);
    xrt_billing_owner_fail_after(billing, SIZE_MAX);
    CHECK(test->failure && directory_queries == queries && destination->present == 1 && destination->owner == first);
    checked_empty_staging(&call);
    XrtBillingStats after = xrt_billing_owner_stats(billing);
    CHECK(after.blocks == before.blocks && after.bytes == before.bytes);
    unsigned remaining = 0;
    for (;;) {
        CHECK(!xrt_native_host_directory_next_checked(&call, NEXT_SOURCE));
        if (!destination->present) break;
        CHECK(destination->owner && ++remaining <= directory_file_count + 1);
    }
    CHECK(remaining == directory_file_count - 1 && !destination->owner); /* Two entries were consumed. */
    checked_empty_staging(&call);
    CHECK(!xrt_native_host_directory_cleanup_close_checked(call.token, CLOSE_SOURCE) && !live_handles);
    xrt_native_host_directory_cleanup_fail(test->failure, 0);
}

typedef struct CheckedPublication {
    const DirectoryNames *names;
    XrtManagedFrame *arm;
    XrtManagedOps *ops;
    bool injected;
} CheckedPublication;

static bool checked_publish_pending(void *context) {
    CheckedPublication *test = context;
    if (test->arm) {
        XrtManagedHeader *pending = NULL;
        /* Corrupt only the destination after preflight. The directory leaf
         * still exclusively owns the staging slot when publication refuses. */
        CHECK(xrt_managed_slot_prepare(test->arm, RESULT_SLOT, test->ops,
            sizeof(XrtStringBacking) + 2, &pending) == XRT_MANAGED_OK && pending);
        test->arm = NULL;
        test->injected = true;
    }
    return true;
}

static void checked_read_publish_failure(void *context) {
    CheckedPublication *test = context;
    XrtHostNullableOwner *destination = NULL;
    XrtNativeDirectoryNext call = checked_receiving_frame(&destination);
    call.token = open_directory_path(test->names->path);
    CHECK(!xrt_native_host_directory_next_checked(&call, NEXT_SOURCE) && destination->present && destination->owner);
    XrtManagedHeader *old = destination->owner;
    XrtBillingOwner *billing = checked_billing();
    XrtBillingStats before = xrt_billing_owner_stats(billing);
    const XrtBackingTypes *types = NULL;
    CHECK(xrt_native_values_backings(xrt_value_root_active(), &types) == XRT_MANAGED_OK && types->string == old->ops);
    test->arm = call.frame;
    test->ops = types->string;
    CHECK(xrt_native_host_directory_next_checked(&call, NEXT_SOURCE) && test->injected);
    CHECK(destination->present == 1 && destination->owner == old);
    checked_empty_staging(&call);
    CHECK(xrt_managed_slot_abort(call.frame, call.destination) == XRT_MANAGED_OK);
    XrtBillingStats after = xrt_billing_owner_stats(billing);
    CHECK(before.blocks == after.blocks && before.bytes == after.bytes);
    CHECK(!xrt_native_host_directory_cleanup_close_checked(call.token, CLOSE_SOURCE) && !live_handles);
}

static void checked_read_close_failure(void *context) {
    CheckedRead *test = context;
    XrtHostNullableOwner *destination = NULL;
    XrtNativeDirectoryNext call = checked_receiving_frame(&destination);
    call.token = open_directory_path(test->names->path);
    CHECK(!xrt_native_host_directory_next_checked(&call, NEXT_SOURCE) && destination->present && destination->owner);
    CHECK(!xrt_native_host_directory_cleanup_close_checked(call.token, CLOSE_SOURCE) && !live_handles);
    test->failure = xrt_native_host_directory_cleanup_close_checked(call.token, CLOSE_SOURCE);
    CHECK(test->failure);
    CHECK(xrt_managed_slot_drop(call.frame, call.destination) == XRT_MANAGED_OK && !destination->owner);
    destination->present = 0;
    xrt_native_host_directory_cleanup_fail(test->failure, 0);
}

static void checked_read_poll_failure(void *context) {
    CheckedRead *test = context;
    XrtHostNullableOwner *destination = NULL;
    XrtNativeDirectoryNext call = checked_receiving_frame(&destination);
    int64_t first = open_directory_path(test->names->path);
    call.token = open_directory_path(test->names->path);
    CHECK(!xrt_native_host_directory_next_checked(&call, NEXT_SOURCE) && destination->present && destination->owner);
    unsigned before = polls;
    cancel_after_close = true;
    CHECK(!xrt_native_host_directory_cleanup_close_checked(call.token, CLOSE_SOURCE) && cancelled && live_handles == 1);
    cancel_after_close = false;
    CHECK(!xrt_native_host_directory_cleanup_close_checked(first, CLOSE_SOURCE) && !live_handles && polls == before);
    test->failure = xrt_native_host_directory_cleanup_poll_checked(POLL_SOURCE);
    CHECK(test->failure && polls == before + 1);
    CHECK(xrt_managed_slot_drop(call.frame, call.destination) == XRT_MANAGED_OK && !destination->owner);
    destination->present = 0;
    xrt_native_host_directory_cleanup_fail(test->failure, 0);
}

static void exercise_checked_reads(const DirectoryNames *names) {
    static const struct { XrtSynchronousRoot invoke; XrXirValueStatus status; uint32_t source; } cases[] = {
        {checked_read_prepare_failure, XR_XIR_VALUE_OOM, NEXT_SOURCE},
        {checked_read_backing_failure, XR_XIR_VALUE_OOM, NEXT_SOURCE},
        {checked_read_close_failure, XR_XIR_VALUE_BAD_ARGUMENT, CLOSE_SOURCE},
        {checked_read_poll_failure, XR_XIR_VALUE_CANCELLED, POLL_SOURCE}
    };
    XrtNativeDirectoryStatus prepare_failure = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrtValueRoot values = {0};
        XrtExecutionFailure failure = {0};
        CheckedRead test = {.names=names};
        const XrtHostExecutionControl control = {request_current, NULL};
        CHECK(xrt_run_embedded_owned_controlled(locations, &failure, cases[i].invoke, &test, &values, &control) ==
            XRT_EXECUTION_FAILURE);
        const XrtValueFailure *value = &failure.detail.value;
        CHECK(xrt_value_root_empty(&values) && !live_handles && failure.kind == XRT_FAILURE_VALUE &&
            value->status == cases[i].status && value->source_id == cases[i].source &&
            test.failure && value->directory_primary == test.failure && !value->directory_secondary);
        if (i == 0) prepare_failure = test.failure;
        else CHECK(test.failure != prepare_failure);
        cancelled = false;
        xrt_execution_failure_drop(&failure);
    }
}

static void directory_files(const wchar_t *root, const wchar_t *unicode, bool create) {
    for (unsigned index = 0; index <= directory_file_count; ++index) {
        wchar_t item[16];
        const wchar_t *name = unicode;
        if (index < directory_file_count) {
            CHECK(swprintf_s(item, _countof(item), L"item-%02u", index) > 0);
            name = item;
        }
        wchar_t child[1024];
        CHECK(swprintf_s(child, _countof(child), L"%ls\\%ls", root, name) > 0);
        if (create) {
            HANDLE file = CreateFileW(child, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            CHECK(file != INVALID_HANDLE_VALUE && CloseHandle(file));
        } else CHECK(DeleteFileW(child));
    }
}

static void exercise_next(void) {
    wchar_t temp[MAX_PATH], scratch[MAX_PATH], root[512];
    DWORD length = GetTempPathW(_countof(temp), temp);
    CHECK(length > 0 && length < _countof(temp));
    CHECK(GetTempFileNameW(temp, L"xrd", 0, scratch) && DeleteFileW(scratch));
    if (scratch[0] == L'\\' && scratch[1] == L'\\')
        CHECK(swprintf_s(root, _countof(root), L"\\\\?\\UNC\\%ls", scratch + 2) > 0);
    else CHECK(swprintf_s(root, _countof(root), L"\\\\?\\%ls", scratch) > 0);
    CHECK(CreateDirectoryW(root, NULL));
    char utf8[2048];
    CHECK(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, root, -1, utf8, sizeof(utf8), NULL, NULL) > 0);
    DirectoryNames names = {.path={utf8, strlen(utf8)}};
    wchar_t unicode[directory_unicode_units + 1] = {0};
    for (unsigned index = 0; index < directory_unicode_units; ++index) {
        unicode[index] = L'\x4e2d';
        memcpy(names.unicode + 3 * index, "\xe4\xb8\xad", 3);
    }
    directory_files(root, unicode, true);

    XrtValueRoot values = {0};
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded_owned(locations, &failure, read_names, &names, &values) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&values) && !live_handles);
    for (unsigned index = 0; index <= directory_file_count; ++index) {
        XrtHostDirectoryName *name = &names.retained[index];
        CHECK(name->text.bytes && name->billing && directory_name_index(&names, &name->text) == index);
        xrt_host_directory_name_drop(name);
        CHECK(empty_name(name));
    }
    xrt_host_directory_name_drop(&names.retained[0]);

    cancelled = false;
    const XrtHostExecutionControl control = {request_current, NULL};
    CHECK(xrt_run_embedded_owned_controlled(locations, &failure, cancel_next, &names, &values, &control) ==
        XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&values) && !live_handles);
    CHECK(xrt_run_embedded_owned(locations, &failure, next_allocation_failure, &names, &values) ==
        XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&values) && !live_handles);
    CHECK(xrt_run_embedded_owned(locations, &failure, checked_read_prepare_retry, &names, &values) ==
        XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&values) && !live_handles);
    CheckedPublication publish = {.names=&names};
    const XrtHostExecutionControl publication = {checked_publish_pending,&publish};
    CHECK(xrt_run_embedded_owned_controlled(locations, &failure, checked_read_publish_failure, &publish,
        &values, &publication) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&values) && !live_handles);
    exercise_checked_reads(&names);
    directory_files(root, unicode, false);
    CHECK(xrt_run_embedded_owned_controlled(locations, &failure, cancel_next_at_end, &names, &values, &control) ==
        XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&values) && !live_handles);
    CHECK(RemoveDirectoryW(root));
    puts("Directory next: real Unicode names across batches, EOF, independent lifetime, cancellation and allocation");
}

static void exercises(void) {
    XrtValueRoot root = {0};
    XrtExecutionFailure failure = {0};
    int64_t previous = 0;
    for (unsigned run = 0; run < 2; ++run) {
        CHECK(xrt_run_embedded_owned(locations, &failure, generation, &previous, &root) ==
            XRT_EXECUTION_COMPLETED);
        CHECK(xrt_value_root_empty(&root));
    }

    CHECK(xrt_run_embedded_owned(locations, &failure, open_refusal, NULL, &root) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&root));

    XrtValueRoot limited = {.byte_limit=1};
    CHECK(xrt_run_embedded_owned(locations, &failure, quota, NULL, &limited) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&limited));

    CHECK(xrt_run_embedded_owned(locations, &failure, fail_retirement, NULL, &root) == XRT_EXECUTION_FAILURE);
    CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_IO);
    CHECK(!xrt_value_root_empty(&root) && live_handles == 2 && failed_handle);
    CHECK(xrt_run_embedded_owned(locations, &failure, open_refusal, NULL, &root) == XRT_EXECUTION_INVALID);
    unsigned before = opened_handles;
    CHECK(xrt_value_root_drain(&root) == XR_OS_IO_IO && live_handles == 2 && failed_handle);
    CHECK(xrt_value_root_drain(&root) == XR_OS_IO_OK && !live_handles && !failed_handle);
    CHECK(xrt_value_root_empty(&root) && opened_handles == before);
    xrt_execution_failure_drop(&failure);

    const XrtHostExecutionControl control = {request_current, NULL};
    CHECK(xrt_run_embedded_owned_controlled(locations, &failure, unopened_cancel, NULL, &root, &control) ==
        XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&root) && !live_handles);

    cancelled = false;
    CHECK(xrt_run_embedded_owned_controlled(locations, &failure, unpublished_cancel, NULL, &root, &control) ==
        XRT_EXECUTION_FAILURE);
    CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_CANCELLED &&
        live_handles == 1 && failed_handle);
    unsigned previous_polls = polls;
    CHECK(xrt_value_root_drain(&root) == XR_OS_IO_OK && !live_handles && polls == previous_polls);
    CHECK(xrt_value_root_empty(&root));
    xrt_execution_failure_drop(&failure);

    cancel_after_open = cancelled = false;
    CHECK(xrt_run_embedded_owned_controlled(locations, &failure, cancel_before_close, NULL,
        &root, &control) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&root));

    unsigned attempts = close_calls;
    CHECK(xrt_run_embedded_owned_controlled(locations, &failure, close_cancel, NULL, &root, &control) ==
        XRT_EXECUTION_FAILURE);
    CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_CANCELLED);
    CHECK(xrt_value_root_empty(&root) && !live_handles && close_calls == attempts + 1);
    xrt_execution_failure_drop(&failure);
    cancel_after_close = cancelled = false;

    for (unsigned moment = 0; moment < 2; ++moment) {
        CleanupCancellation test = {.before_close=moment != 0};
        CHECK(xrt_run_embedded_owned_controlled(locations, &failure, cleanup_cancel, &test,
            &root, &control) == XRT_EXECUTION_FAILURE);
        CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_CANCELLED);
        CHECK(polls == test.polls_before_cleanup + 1 && !live_handles && xrt_value_root_empty(&root));
        xrt_execution_failure_drop(&failure);
        cancel_after_close = cancelled = false;
    }
    attempts = close_calls;
    CHECK(xrt_run_embedded_owned_controlled(locations, &failure, cleanup_os_refusal, NULL,
        &root, &control) == XRT_EXECUTION_COMPLETED);
    CHECK(close_calls == attempts + 2 && !live_handles && !failed_handle && xrt_value_root_empty(&root));

    CloseOrder order = {0};
    const XrtHostExecutionControl reorder = {open_during_poll, &order};
    CHECK(xrt_run_embedded_owned_controlled(locations, &failure, close_original_directory, &order,
        &root, &reorder) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&root));
    puts("Directory tokens: real cursor, foreign/stale refusal, cancellation and close-only retirement");
}
#elif defined(XR_OS_LINUX) || defined(XR_OS_MACOS)
typedef struct UnixDirectoryCase {
    XrtHostDirectoryName received;
    char *expected;
} UnixDirectoryCase;

static void unix_directory(void *context) {
    UnixDirectoryCase *test = context;
    int64_t token = 0;
    CHECK(xrt_host_directory_open(current_path, &token) == XR_OS_IO_OK && token > 0);
    CHECK(xrt_host_directory_next(token, &test->received) == XR_OS_IO_OK);
    size_t length = test->received.text.length;
    CHECK(length && strlen(test->received.text.bytes) == length);
    test->expected = malloc(length + 1);
    CHECK(test->expected);
    memcpy(test->expected, test->received.text.bytes, length + 1);

    bool closed = false;
    CHECK(xrt_host_directory_close(token, &closed) == XR_OS_IO_OK && closed);
    XrtHostDirectoryName stale = {0};
    CHECK(xrt_host_directory_next(token, &stale) == XR_OS_IO_BAD_ARGUMENT && empty_name(&stale));
    CHECK(xrt_host_directory_close(token, &closed) == XR_OS_IO_BAD_ARGUMENT && closed);
    XrtHostNullableOwner *destination = NULL;
    XrtNativeDirectoryNext call = checked_receiving_frame(&destination);
    CHECK(xrt_host_directory_open(current_path, &call.token) == XR_OS_IO_OK && call.token > token);
    CHECK(!xrt_native_host_directory_next_checked(&call, NEXT_SOURCE) && destination->present && destination->owner);
    const XrtStringBacking *first = (const XrtStringBacking *)destination->owner;
    CHECK(first->length == length && !strcmp(first->bytes, test->expected));
    do {
        checked_empty_staging(&call);
        CHECK(!xrt_native_host_directory_next_checked(&call, NEXT_SOURCE));
    } while (destination->present);
    CHECK(!destination->owner);
    checked_empty_staging(&call);
    CHECK(!xrt_value_root_empty(xrt_value_root_active()));
}
#else
static void unsupported(void *context) {
    (void)context;
    int64_t token = -1;
    bool closed = true;
    XrtHostDirectoryName name = {0};
    CHECK(xrt_host_directory_open(current_path, &token) == XR_OS_IO_UNSUPPORTED && token == -1);
    CHECK(xrt_host_directory_next(1, &name) == XR_OS_IO_UNSUPPORTED && empty_name(&name));
    CHECK(xrt_host_directory_close(1, &closed) == XR_OS_IO_BAD_ARGUMENT && closed);
    CHECK(xrt_value_root_empty(xrt_value_root_active()));
}
#endif

int main(int argc, char **argv) {
    CHECK(argc >= 1);
    const XrtSourceLocation source[] = {
        {"directory.xr", 12, 2, 8, 0, 0},
        {"directory.xr", 12, 5, 9, 0, 0},
        {"directory.xr", 12, 6, 1, 0, 0}
    };
    CHECK(xrt_locations_create(source, sizeof(source) / sizeof(source[0]), &locations) == XRT_LOCATION_OK);
    int64_t token = -1;
    CHECK(xrt_host_directory_open(current_path, &token) == XR_OS_IO_BAD_ARGUMENT && token == -1);
#if defined(XR_OS_WINDOWS)
    executable_path = (XrOsPathSpan){argv[0], strlen(argv[0])};
    exercises();
    exercise_next();
#elif defined(XR_OS_LINUX) || defined(XR_OS_MACOS)
    (void)argv;
    XrtValueRoot root = {0};
    XrtExecutionFailure failure = {0};
    UnixDirectoryCase test = {0};
    CHECK(xrt_run_embedded_owned(locations, &failure, unix_directory, &test, &root) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&root));
    CHECK(!strcmp(test.received.text.bytes, test.expected));
    xrt_host_directory_name_drop(&test.received);
    CHECK(empty_name(&test.received));
    free(test.expected);
    puts("Directory tokens: Unix cursor, stale refusal, and retained name after root retirement");
#else
    (void)argv;
    XrtValueRoot root = {0};
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded_owned(locations, &failure, unsupported, NULL, &root) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_value_root_empty(&root));
    puts("Directory tokens: unsupported Unix open creates no owner");
#endif
#if defined(XR_OS_WINDOWS) || defined(XR_OS_LINUX) || defined(XR_OS_MACOS)
    exercise_checked_failures();
#endif
    xrt_locations_release(locations);
    return 0;
}
