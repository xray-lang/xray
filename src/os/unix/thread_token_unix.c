/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * thread_token_unix.c - Real pthread exit retirement without historical IDs
 */
#include "../os_thread_token.h"
#include "../../base/xmalloc.h"
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <limits.h>

typedef struct ThreadTokenRecord {
    struct ThreadTokenRecord *next;
    int64_t token;
    _Atomic(bool) exited;
} ThreadTokenRecord;
static pthread_mutex_t token_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t token_key;
static ThreadTokenRecord *token_records;
static uint64_t token_sequence;
static bool token_key_ready, token_closing;
static XR_THREAD_LOCAL bool token_entered, token_exiting;

/* The key destructor is an actual thread-exit boundary. It neither allocates
 * nor enters a caller's runtime. Later host TLS destructors cannot reregister
 * this terminating thread under another identity. Queries reclaim its node. */
static void token_thread_exit(void *pointer) {
    ThreadTokenRecord *record = pointer;
    token_exiting = true;
    atomic_store_explicit(&record->exited, true, memory_order_release);
}
static bool token_current(const XrOsThreadTokenQuery *query) {
    return !query || query->current(query->context);
}
static XrOsIoStatus token_posix_error(int error) {
    if (error == ENOMEM) return XR_OS_IO_OUT_OF_MEMORY;
    if (error == EAGAIN) return XR_OS_IO_OVERFLOW;
    return XR_OS_IO_IO;
}
static XrOsIoStatus token_begin(const XrOsThreadTokenQuery *query) {
    if (token_entered || token_exiting || (query && !query->current)) return XR_OS_IO_BAD_ARGUMENT;
    token_entered = true;
    if (!token_current(query)) { token_entered = false; return XR_OS_IO_CANCELLED; }
    int error = pthread_mutex_lock(&token_lock);
    if (error) { token_entered = false; return token_posix_error(error); }
    return XR_OS_IO_OK;
}
static XrOsIoStatus token_end(XrOsIoStatus status) {
    int error = pthread_mutex_unlock(&token_lock);
    token_entered = false;
    return error ? token_posix_error(error) : status;
}
static XrOsIoStatus token_retire(const XrOsThreadTokenQuery *query) {
    ThreadTokenRecord **link = &token_records;
    while (*link) {
        if (!token_current(query)) return XR_OS_IO_CANCELLED;
        ThreadTokenRecord *record = *link;
        if (!atomic_load_explicit(&record->exited, memory_order_acquire)) { link = &record->next; continue; }
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
    ThreadTokenRecord *selected = token_key_ready ? pthread_getspecific(token_key) : NULL;
    if (selected) {
        bool owned = false;
        for (ThreadTokenRecord *record = token_records; record; record = record->next) {
            if (!token_current(query)) return token_end(XR_OS_IO_CANCELLED);
            if (record == selected) { owned = true; break; }
        }
        if (!owned) return token_end(XR_OS_IO_IO);
    }
    if (!selected) {
        if (token_sequence >= (uint64_t)INT64_MAX) return token_end(XR_OS_IO_OVERFLOW);
        if (!token_key_ready) {
            int error = pthread_key_create(&token_key, token_thread_exit);
            if (error) return token_end(token_posix_error(error));
            token_key_ready = true;
        }
        selected = xr_calloc(1, sizeof(*selected));
        if (!selected) return token_end(XR_OS_IO_OUT_OF_MEMORY);
        atomic_init(&selected->exited, false);
        if (!token_current(query)) { xr_free(selected); return token_end(XR_OS_IO_CANCELLED); }
        int error = pthread_setspecific(token_key, selected);
        if (error) { xr_free(selected); return token_end(token_posix_error(error)); }
        selected->token = (int64_t)++token_sequence;
        selected->next = token_records; token_records = selected;
    }
    if (!token_current(query)) return token_end(XR_OS_IO_CANCELLED);
    int64_t value = selected->token;
    status = token_end(XR_OS_IO_OK);
    if (status == XR_OS_IO_OK) *output = value;
    return status;
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
        if (record->token == token) {
            alive = !atomic_load_explicit(&record->exited, memory_order_acquire);
            break;
        }
    }
    if (!token_current(query)) return token_end(XR_OS_IO_CANCELLED);
    status = token_end(XR_OS_IO_OK);
    if (status == XR_OS_IO_OK) *output = alive;
    return status;
}
XR_FUNC XrOsIoStatus xr_os_io_thread_token_shutdown(void) {
    XrOsIoStatus status = token_begin(NULL);
    if (status != XR_OS_IO_OK) return status;
    status = token_retire(NULL);
    if (status != XR_OS_IO_OK) return token_end(status);
    if (!token_closing) {
        ThreadTokenRecord *current = token_key_ready ? pthread_getspecific(token_key) : NULL;
        for (ThreadTokenRecord *record = token_records; record; record = record->next)
            if (record != current && !atomic_load_explicit(&record->exited, memory_order_acquire))
                return token_end(XR_OS_IO_BAD_ARGUMENT);
        token_closing = true;
    }
    if (token_key_ready) {
        int error = pthread_setspecific(token_key, NULL);
        if (!error) error = pthread_key_delete(token_key);
        if (error) return token_end(token_posix_error(error));
        token_key_ready = false;
    }
    while (token_records) {
        ThreadTokenRecord *record = token_records;
        token_records = record->next;
        xr_free(record);
    }
    return token_end(XR_OS_IO_OK);
}
