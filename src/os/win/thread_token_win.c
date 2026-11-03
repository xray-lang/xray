/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * thread_token_win.c - Process-owned live thread handles and non-reused tokens
 */
#include "../os_thread_token.h"
#include "../../base/xmalloc.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <limits.h>

typedef struct ThreadTokenRecord {
    struct ThreadTokenRecord *next;
    HANDLE handle;
    DWORD thread;
    int64_t token;
} ThreadTokenRecord;
static SRWLOCK token_lock = SRWLOCK_INIT;
static ThreadTokenRecord *token_records;
static uint64_t token_sequence;
static bool token_closing;
static XR_THREAD_LOCAL bool token_entered;

static bool token_current(const XrOsThreadTokenQuery *query) {
    return !query || query->current(query->context);
}
static XrOsIoStatus token_begin(const XrOsThreadTokenQuery *query) {
    if (token_entered || (query && !query->current)) return XR_OS_IO_BAD_ARGUMENT;
    token_entered = true;
    if (!token_current(query)) { token_entered = false; return XR_OS_IO_CANCELLED; }
    AcquireSRWLockExclusive(&token_lock);
    return XR_OS_IO_OK;
}
static XrOsIoStatus token_end(XrOsIoStatus status) {
    ReleaseSRWLockExclusive(&token_lock);
    token_entered = false;
    return status;
}
static XrOsIoStatus token_windows_error(DWORD error) {
    return error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY ?
        XR_OS_IO_OUT_OF_MEMORY : XR_OS_IO_IO;
}
/* A numeric thread ID never proves incarnation. The retained thread object
 * must still be unsignaled before that ID can select its existing token. */
static XrOsIoStatus token_retire(const XrOsThreadTokenQuery *query) {
    ThreadTokenRecord **link = &token_records;
    while (*link) {
        if (!token_current(query)) return XR_OS_IO_CANCELLED;
        ThreadTokenRecord *record = *link;
        DWORD state = WaitForSingleObject(record->handle, 0);
        if (state == WAIT_TIMEOUT) { link = &record->next; continue; }
        if (state != WAIT_OBJECT_0) return token_windows_error(GetLastError());
        if (!CloseHandle(record->handle)) return token_windows_error(GetLastError());
        *link = record->next;
        xr_free(record);
    }
    return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xr_os_io_thread_token_current(const XrOsThreadTokenQuery *query,
    int64_t *output) {
    if (!output) return XR_OS_IO_BAD_ARGUMENT;
    XrOsIoStatus status = token_begin(query);
    if (status != XR_OS_IO_OK) return status;
    if (token_closing) return token_end(XR_OS_IO_BAD_ARGUMENT);
    status = token_retire(query);
    if (status != XR_OS_IO_OK) return token_end(status);
    DWORD thread = GetCurrentThreadId();
    ThreadTokenRecord *selected = NULL;
    for (ThreadTokenRecord *record = token_records; record; record = record->next) {
        if (!token_current(query)) return token_end(XR_OS_IO_CANCELLED);
        if (record->thread == thread) { selected = record; break; }
    }
    if (!selected) {
        if (token_sequence >= (uint64_t)INT64_MAX) return token_end(XR_OS_IO_OVERFLOW);
        selected = xr_calloc(1, sizeof(*selected));
        if (!selected) return token_end(XR_OS_IO_OUT_OF_MEMORY);
        if (!token_current(query)) { xr_free(selected); return token_end(XR_OS_IO_CANCELLED); }
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                &selected->handle, SYNCHRONIZE, FALSE, 0)) {
            DWORD error = GetLastError(); xr_free(selected); return token_end(token_windows_error(error));
        }
        if (!selected->handle) { xr_free(selected); return token_end(XR_OS_IO_IO); }
        selected->thread = thread;
        selected->token = (int64_t)++token_sequence;
        selected->next = token_records; token_records = selected;
    }
    if (!token_current(query)) return token_end(XR_OS_IO_CANCELLED);
    *output = selected->token;
    return token_end(XR_OS_IO_OK);
}
XR_FUNC XrOsIoStatus xr_os_io_thread_token_alive(const XrOsThreadTokenQuery *query,
    int64_t token, bool *output) {
    if (!output) return XR_OS_IO_BAD_ARGUMENT;
    XrOsIoStatus status = token_begin(query);
    if (status != XR_OS_IO_OK) return status;
    if (token_closing) return token_end(XR_OS_IO_BAD_ARGUMENT);
    status = token_retire(query);
    if (status != XR_OS_IO_OK) return token_end(status);
    bool alive = false;
    for (ThreadTokenRecord *record = token_records; record; record = record->next) {
        if (!token_current(query)) return token_end(XR_OS_IO_CANCELLED);
        if (record->token != token) continue;
        DWORD state = WaitForSingleObject(record->handle, 0);
        if (state != WAIT_TIMEOUT && state != WAIT_OBJECT_0)
            return token_end(token_windows_error(GetLastError()));
        alive = state == WAIT_TIMEOUT;
        break;
    }
    if (!token_current(query)) return token_end(XR_OS_IO_CANCELLED);
    *output = alive;
    return token_end(XR_OS_IO_OK);
}
XR_FUNC XrOsIoStatus xr_os_io_thread_token_shutdown(void) {
    XrOsIoStatus status = token_begin(NULL);
    if (status != XR_OS_IO_OK) return status;
    if (!token_closing) {
        DWORD current = GetCurrentThreadId();
        for (ThreadTokenRecord *record = token_records; record; record = record->next) {
            DWORD state = WaitForSingleObject(record->handle, 0);
            if (state == WAIT_TIMEOUT && record->thread != current) return token_end(XR_OS_IO_BAD_ARGUMENT);
            if (state != WAIT_TIMEOUT && state != WAIT_OBJECT_0)
                return token_end(token_windows_error(GetLastError()));
        }
        token_closing = true;
    }
    while (token_records) {
        ThreadTokenRecord *record = token_records;
        if (!CloseHandle(record->handle)) return token_end(token_windows_error(GetLastError()));
        token_records = record->next;
        xr_free(record);
    }
    return token_end(XR_OS_IO_OK);
}
