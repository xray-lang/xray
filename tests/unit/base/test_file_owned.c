/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_file_owned.c - Real worker bytes, anchored owners and allocation cleanup
 */
#include "os/os_file_owned.h"
#include "os/xfullpath_owned.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "owned file %d: %s\n", __LINE__, #condition);                          \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef union FileBlock {
    long double alignment;
    void *pointer;
    size_t bytes;
} FileBlock;
static size_t file_live, file_bytes, file_attempts, file_fail_at = SIZE_MAX;
static XrOsIoStatus file_memory(void *context, size_t bytes, void **output) {
    CHECK(context == &file_live && output && !*output);
    if (file_attempts++ == file_fail_at)
        return XR_OS_IO_OUT_OF_MEMORY;
    if (bytes > SIZE_MAX - sizeof(FileBlock))
        return XR_OS_IO_OVERFLOW;
    FileBlock *block = xr_malloc(sizeof(*block) + bytes);
    if (!block)
        return XR_OS_IO_OUT_OF_MEMORY;
    block->bytes = bytes;
    ++file_live;
    file_bytes += bytes;
    *output = block + 1;
    return XR_OS_IO_OK;
}
static void file_memory_free(void *context, void *pointer) {
    CHECK(context == &file_live);
    if (!pointer)
        return;
    FileBlock *block = (FileBlock *) pointer - 1;
    CHECK(file_live && file_bytes >= block->bytes);
    --file_live;
    file_bytes -= block->bytes;
    xr_free(block);
}
#if defined(XR_OS_WINDOWS)
#include "os/win/file_owned_win.c"
#else
#include "os/unix/file_owned_unix.c"
#endif
#if defined(XR_OS_WINDOWS)
static bool file_current(void *context) {
    return !*(bool *) context;
}
static XrOsFileProgress file_terminal(XrOsFileOperation *operation) {
    ULONGLONG end = GetTickCount64() + 5000;
    XrOsFileProgress result = {0};
    do {
        CHECK(xr_os_io_file_poll(operation, &result) == XR_OS_IO_OK);
        if (result.terminal)
            return result;
        Sleep(0);
    } while (GetTickCount64() < end);
    CHECK(false);
    return result;
}
static void file_operation_done(XrOsFileOperation *operation) {
    bool closed = false;
    CHECK(xr_os_io_file_operation_close(operation, &closed) == XR_OS_IO_OK && closed &&
          !operation->state);
}
static void file_opened(const XrOsIoPolicy *memory, const XrOsFileQuery *query, const char *path,
                        bool writing, bool append, XrOsFileOwner *owner) {
    XrOsFileOperation operation = {0};
    XrOsFileRequest request = {.action = writing ? XR_OS_FILE_OPEN_WRITE : XR_OS_FILE_OPEN_READ,
                               .path = path,
                               .path_length = strlen(path),
                               .append = append,
                               .standard_stream = -1};
    CHECK(xr_os_io_file_open_begin(memory, query, &request, owner, &operation) == XR_OS_IO_OK);
    XrOsFileOperation premature = {0};
    XrOsFileRequest read = {.action = XR_OS_FILE_READ, .standard_stream = -1, .length = 1};
    CHECK(xr_os_io_file_begin(memory, query, &read, owner, &premature) == XR_OS_IO_BAD_ARGUMENT &&
          !premature.state);
    XrOsFileProgress result = file_terminal(&operation);
    CHECK(result.status == XR_OS_IO_OK && result.success);
    bool closed = true;
    CHECK(xr_os_io_file_close(owner, &closed) == XR_OS_IO_OK && !closed);
    XrOsFileOwner copied = *owner;
    closed = true;
    CHECK(xr_os_io_file_close(&copied, &closed) == XR_OS_IO_BAD_ARGUMENT && closed);
    XrOsFileOperation copied_operation = operation;
    XrOsFileProgress unchanged = {.count = 123};
    CHECK(xr_os_io_file_poll(&copied_operation, &unchanged) == XR_OS_IO_BAD_ARGUMENT &&
          unchanged.count == 123);
    file_operation_done(&operation);
}
static void file_written(const XrOsIoPolicy *memory, const XrOsFileQuery *query,
                         XrOsFileOwner *owner, const uint8_t *bytes, size_t length) {
    XrOsFileOperation operation = {0};
    XrOsFileRequest request = {
        .action = XR_OS_FILE_WRITE, .standard_stream = -1, .bytes = bytes, .length = length};
    CHECK(xr_os_io_file_begin(memory, query, &request, owner, &operation) == XR_OS_IO_OK);
    XrOsFileProgress result = file_terminal(&operation);
    CHECK(result.status == XR_OS_IO_OK && result.success && result.count == length);
    file_operation_done(&operation);
}
static void file_owner_done(XrOsFileOwner *owner) {
    bool closed = false;
    CHECK(xr_os_io_file_close(owner, &closed) == XR_OS_IO_OK && closed && !owner->state);
    closed = false;
    CHECK(xr_os_io_file_close(owner, &closed) == XR_OS_IO_OK && closed);
}
typedef struct FileReentry {
    const XrOsIoPolicy *memory;
    XrOsFileOwner *owner;
    XrOsFileOperation *operation;
    const char *path;
    bool observed, current;
} FileReentry;
static bool file_reentrant_current(void *context) {
    FileReentry *query = context;
    if (query->observed)
        return query->current;
    query->observed = true;
    bool closed = false;
    CHECK(xr_os_io_file_close(query->owner, &closed) == XR_OS_IO_BAD_ARGUMENT && !closed);
    CHECK(xr_os_io_file_operation_close(query->operation, &closed) == XR_OS_IO_BAD_ARGUMENT &&
          !closed);
    XrOsFileRequest open = {.action = XR_OS_FILE_OPEN_READ,
                            .path = query->path,
                            .path_length = strlen(query->path),
                            .standard_stream = -1};
    CHECK(xr_os_io_file_open_begin(query->memory, NULL, &open, query->owner, query->operation) ==
          XR_OS_IO_BAD_ARGUMENT);
    XrOsFileRequest read = {.action = XR_OS_FILE_READ, .standard_stream = -1, .length = 1};
    CHECK(xr_os_io_file_begin(query->memory, NULL, &read, query->owner, query->operation) ==
          XR_OS_IO_BAD_ARGUMENT);
    return query->current;
}
static void file_query_reentry(const XrOsIoPolicy *memory, const char *path) {
    XrOsFileOwner owner = {0};
    XrOsFileOperation operation = {0};
    FileReentry context = {memory, &owner, &operation, path, false, true};
    XrOsFileQuery query = {&context, file_reentrant_current};
    XrOsFileRequest request = {.action = XR_OS_FILE_OPEN_READ,
                               .path = path,
                               .path_length = strlen(path),
                               .standard_stream = -1};
    CHECK(xr_os_io_file_open_begin(memory, &query, &request, &owner, &operation) == XR_OS_IO_OK);
    CHECK(context.observed && file_terminal(&operation).success);
    file_operation_done(&operation);
    request = (XrOsFileRequest) {.action = XR_OS_FILE_READ, .standard_stream = -1, .length = 1};
    context.observed = false;
    CHECK(xr_os_io_file_begin(memory, &query, &request, &owner, &operation) == XR_OS_IO_OK);
    CHECK(context.observed && file_terminal(&operation).success);
    file_operation_done(&operation);
    context.observed = false;
    context.current = false;
    CHECK(xr_os_io_file_begin(memory, &query, &request, &owner, &operation) == XR_OS_IO_CANCELLED);
    CHECK(context.observed && !owner.busy && !operation.busy && !operation.state);
    file_owner_done(&owner);
    context.observed = false;
    request = (XrOsFileRequest) {.action = XR_OS_FILE_OPEN_READ,
                                 .path = path,
                                 .path_length = strlen(path),
                                 .standard_stream = -1};
    CHECK(xr_os_io_file_open_begin(memory, &query, &request, &owner, &operation) ==
          XR_OS_IO_CANCELLED);
    CHECK(context.observed && !owner.busy && !operation.busy && !owner.state && !operation.state);
    CHECK(!file_live && !file_bytes);
}
static void file_open_allocation_failures(const XrOsIoPolicy *memory, const XrOsFileQuery *query,
                                          const char *path) {
    for (size_t index = 0; index < 16; ++index) {
        XrOsFileOwner owner = {0};
        XrOsFileOperation operation = {0};
        XrOsFileRequest request = {.action = XR_OS_FILE_OPEN_READ,
                                   .path = path,
                                   .path_length = strlen(path),
                                   .standard_stream = -1};
        file_fail_at = file_attempts + index;
        XrOsIoStatus status = xr_os_io_file_open_begin(memory, query, &request, &owner, &operation);
        file_fail_at = SIZE_MAX;
        CHECK(status == XR_OS_IO_OK || status == XR_OS_IO_OUT_OF_MEMORY);
        if (status == XR_OS_IO_OK) {
            XrOsFileProgress progress = file_terminal(&operation);
            CHECK(progress.success && progress.status == XR_OS_IO_OK);
        }
        file_operation_done(&operation);
        file_owner_done(&owner);
        CHECK(!file_live && !file_bytes);
        if (status == XR_OS_IO_OK) {
            CHECK(index >= 4);
            return;
        }
    }
    CHECK(false);
}
static void file_kernel_execute(void) {
    size_t live = file_live, bytes = file_bytes;
    bool cancelled = false;
    XrOsIoPolicy memory = {&file_live, file_memory, file_memory_free};
    XrOsFileQuery query = {&cancelled, file_current};
    wchar_t temporary[32768], directory[32768], wide[32768];
    char path[98304];
    DWORD length = GetTempPathW(32768, temporary);
    CHECK(length && length < 32768);
    CHECK(swprintf(directory, 32768, L"%lsxray-file-%lu-%llu-\x4e2d", temporary,
                   GetCurrentProcessId(), GetTickCount64()) > 0);
    CHECK(CreateDirectoryW(directory, NULL));
    CHECK(swprintf(wide, 32768, L"%ls\\bytes-\x6587.bin", directory) > 0);
    int encoded = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, path,
                                      (int) sizeof(path), NULL, NULL);
    CHECK(encoded > 0);
    static const uint8_t expected[] = {0x0d, 0x0a, 0x1a, 0x00, 0xff, 0x42, 0x00, 0xe4, 0xb8, 0xad};
    uint8_t input[sizeof(expected)];
    memcpy(input, expected, sizeof(input));
    XrOsFileOwner owner = {0};
    file_opened(&memory, &query, path, true, false, &owner);
    file_written(&memory, &query, &owner, input, sizeof(input));
    file_owner_done(&owner);
    file_opened(&memory, &query, path, true, false, &owner);
    file_written(&memory, &query, &owner, input, 7);
    XrOsFileOperation operation = {0};
    XrOsFileRequest flush = {.action = XR_OS_FILE_FLUSH, .standard_stream = -1};
    CHECK(xr_os_io_file_begin(&memory, &query, &flush, &owner, &operation) == XR_OS_IO_OK);
    XrOsFileProgress result = file_terminal(&operation);
    CHECK(result.status == XR_OS_IO_OK && result.success);
    file_operation_done(&operation);
    file_owner_done(&owner);
    file_opened(&memory, &query, path, true, true, &owner);
    file_written(&memory, &query, &owner, input + 7, sizeof(input) - 7);
    file_owner_done(&owner);
    CHECK(!memcmp(input, expected, sizeof(input)));
    HANDLE exclusive =
        CreateFileW(wide, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(exclusive != INVALID_HANDLE_VALUE);
    uint8_t actual[sizeof(expected) + 1] = {0};
    DWORD got = 0;
    CHECK(ReadFile(exclusive, actual, sizeof(actual), &got, NULL) && got == sizeof(expected) &&
          !memcmp(actual, expected, sizeof(expected)));
    CHECK(CloseHandle(exclusive));
    file_opened(&memory, &query, path, false, false, &owner);
    size_t used = 0;
    do {
        XrOsFileRequest read = {.action = XR_OS_FILE_READ, .standard_stream = -1, .length = 4};
        CHECK(xr_os_io_file_begin(&memory, &query, &read, &owner, &operation) == XR_OS_IO_OK);
        result = file_terminal(&operation);
        CHECK(result.status == XR_OS_IO_OK && result.success && result.count <= 4 &&
              used + result.count <= sizeof(expected));
        if (result.count) {
            CHECK(result.bytes);
            memcpy(actual + used, result.bytes, result.count);
            used += result.count;
        }
        file_operation_done(&operation);
    } while (result.count);
    CHECK(used == sizeof(expected) && !memcmp(actual, expected, sizeof(expected)));
    file_owner_done(&owner);
    XrOsFileRequest request = {.action = XR_OS_FILE_OPEN_READ,
                               .path = path,
                               .path_length = strlen(path),
                               .standard_stream = -1};
    file_fail_at = file_attempts;
    CHECK(xr_os_io_file_open_begin(&memory, &query, &request, &owner, &operation) ==
          XR_OS_IO_OUT_OF_MEMORY);
    file_fail_at = SIZE_MAX;
    CHECK(!owner.state && !operation.state);
    cancelled = true;
    CHECK(xr_os_io_file_open_begin(&memory, &query, &request, &owner, &operation) ==
              XR_OS_IO_CANCELLED &&
          !owner.state && !operation.state);
    cancelled = false;
    char invalid[1];
    memset(invalid, 0xff, sizeof(invalid));
    request.path = invalid;
    request.path_length = sizeof(invalid);
    CHECK(xr_os_io_file_open_begin(&memory, &query, &request, &owner, &operation) ==
          XR_OS_IO_BAD_ARGUMENT);
    file_operation_done(&operation);
    file_owner_done(&owner);
    file_query_reentry(&memory, path);
    file_open_allocation_failures(&memory, &query, path);
    CHECK(DeleteFileW(wide) && RemoveDirectoryW(directory));
    CHECK(file_live == live && file_bytes == bytes);
}
#else
static void file_kernel_execute(void) {
    XrOsFileOwner owner = {0};
    XrOsFileOperation operation = {0};
    XrOsFileProgress progress = {.count = 123};
    bool closed = false;
    XrOsFileRequest request = {.action = XR_OS_FILE_READ, .standard_stream = 0, .length = 1};
    CHECK(xr_os_io_file_open_begin(NULL, NULL, &request, &owner, &operation) ==
          XR_OS_IO_UNSUPPORTED);
    CHECK(xr_os_io_file_begin(NULL, NULL, &request, &owner, &operation) == XR_OS_IO_UNSUPPORTED);
    CHECK(xr_os_io_file_poll(&operation, &progress) == XR_OS_IO_UNSUPPORTED &&
          progress.count == 123);
    CHECK(xr_os_io_file_cancel(&operation) == XR_OS_IO_UNSUPPORTED);
    CHECK(xr_os_io_file_operation_close(&operation, &closed) == XR_OS_IO_UNSUPPORTED && !closed);
    CHECK(xr_os_io_file_close(&owner, &closed) == XR_OS_IO_UNSUPPORTED && !closed && !owner.state &&
          !operation.state);
}
#endif

static bool file_path_current(void *context) {
    CHECK(context == &file_live);
    return true;
}
static void file_fullpath(void) {
    XrOsIoPolicy memory = {&file_live, file_memory, file_memory_free};
    char *output = NULL;
#if defined(XR_OS_WINDOWS)
    wchar_t wide[32768];
    char expected[98304];
    DWORD count = GetCurrentDirectoryW(32768, wide);
    CHECK(count && count < 32768);
    CHECK(xr_win_utf16_to_utf8(wide, count, expected, sizeof(expected)));
    size_t length = strlen(expected);
    if (length && expected[length - 1] != '\\')
        expected[length++] = '\\';
    memcpy(expected + length, "leaf", 5);
    size_t first_attempt = file_attempts;
    CHECK(xr_os_io_fullpath_owned(&memory, ".\\absent-parent\\..\\leaf", file_path_current,
                                  &output) == XR_OS_IO_OK);
    CHECK(output && !strcmp(output, expected));
    size_t allocations = file_attempts - first_attempt;
    file_memory_free(memory.context, output);
    output = NULL;
    CHECK(allocations && !file_live && !file_bytes);
    for (size_t index = 0; index < allocations; ++index) {
        file_fail_at = file_attempts + index;
        CHECK(xr_os_io_fullpath_owned(&memory, ".\\leaf", file_path_current, &output) ==
              XR_OS_IO_OUT_OF_MEMORY);
        CHECK(!output && !file_live && !file_bytes);
        file_fail_at = SIZE_MAX;
    }
    CHECK(xr_os_io_fullpath_owned(&memory, "\xc0\x80", file_path_current, &output) ==
          XR_OS_IO_BAD_ARGUMENT);
    CHECK(!output && !file_live && !file_bytes);
#else
    CHECK(xr_os_io_fullpath_owned(&memory, "/tmp/absent", file_path_current, &output) ==
          XR_OS_IO_UNSUPPORTED);
    CHECK(!output && !file_live && !file_bytes);
#endif
}
int main(void) {
    file_kernel_execute();
    file_fullpath();
    CHECK(!file_live && !file_bytes);
    puts("owned file bytes, cleanup and lexical absolute paths PASS");
    return 0;
}
