/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * signal_win.c - Persistent console INT publication into an owned mailbox
 */
#include "../os_signal.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <limits.h>
#include <stdatomic.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && UINT_MAX == UINT32_MAX,
    "signal mailbox publication requires always-lock-free 32-bit integers");
typedef struct SignalSlot {
    _Atomic(uint32_t) word;
    XrOsSignalOwner *owner;
    uint64_t token;
    uint32_t generation;
    bool closing;
} SignalSlot;
static SignalSlot signal_int_slot;
static uint32_t signal_epoch;
static SRWLOCK signal_owner_lock = SRWLOCK_INIT;

/* Console callbacks run on an OS-created thread. This path accesses only the
 * static atomic word; a replacement makes a stale publication CAS fail. */
static BOOL WINAPI signal_console_handler(DWORD event) {
    if (event != CTRL_C_EVENT) return FALSE;
    uint32_t observed = atomic_load_explicit(&signal_int_slot.word, memory_order_relaxed);
    if (!observed) return FALSE;
    if (!(observed & 1u))
        (void)atomic_compare_exchange_strong_explicit(&signal_int_slot.word, &observed, observed | 1u,
            memory_order_relaxed, memory_order_relaxed);
    return observed != 0;
}
static bool signal_empty(const XrOsSignalOwner *owner) {
    return !owner->token && !owner->generation && !owner->signal;
}
static bool signal_matches(const XrOsSignalOwner *owner) {
    return signal_int_slot.owner == owner && owner->signal == 2 && owner->token &&
        owner->token == signal_int_slot.token && owner->generation == signal_int_slot.generation;
}
static XrOsIoStatus signal_windows_error(DWORD error) {
    if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY) return XR_OS_IO_OUT_OF_MEMORY;
    if (error == ERROR_INVALID_PARAMETER) return XR_OS_IO_BAD_ARGUMENT;
    return XR_OS_IO_IO;
}
static void signal_commit(XrOsSignalOwner *owner, uint32_t generation) {
    uint64_t token = ((uint64_t)generation << 32) | 2u;
    signal_int_slot.owner = owner;
    signal_int_slot.token = token;
    signal_int_slot.generation = generation;
    signal_int_slot.closing = false;
    *owner = (XrOsSignalOwner){token, generation, 2};
}

XR_FUNC XrOsIoStatus xr_os_io_signal_open(int64_t signal, XrOsSignalOwner *owner, bool *installed) {
    if (!owner || !installed) return XR_OS_IO_BAD_ARGUMENT;
    AcquireSRWLockExclusive(&signal_owner_lock);
    XrOsIoStatus status = XR_OS_IO_OK;
    bool opened = false;
    if (!signal_empty(owner)) {
        if (signal != 2 || !signal_matches(owner) || signal_int_slot.closing) status = XR_OS_IO_BAD_ARGUMENT;
        else if (signal_epoch == (UINT32_MAX >> 1)) status = XR_OS_IO_OVERFLOW;
        else {
            uint32_t generation = ++signal_epoch;
            signal_commit(owner, generation);
            atomic_store_explicit(&signal_int_slot.word, generation << 1, memory_order_release);
            opened = true;
        }
    } else if (signal == 2 && !signal_int_slot.owner) {
        DWORD process = 0;
        DWORD count = GetConsoleProcessList(&process, 1);
        if (!count) {
            DWORD error = GetLastError();
            if (error != ERROR_INVALID_HANDLE) status = signal_windows_error(error);
        } else if (signal_epoch == (UINT32_MAX >> 1)) status = XR_OS_IO_OVERFLOW;
        else {
            uint32_t generation = ++signal_epoch;
            atomic_store_explicit(&signal_int_slot.word, generation << 1, memory_order_release);
            if (!SetConsoleCtrlHandler(signal_console_handler, TRUE)) {
                DWORD error = GetLastError();
                atomic_store_explicit(&signal_int_slot.word, 0, memory_order_release);
                if (error != ERROR_INVALID_HANDLE) status = signal_windows_error(error);
            } else { signal_commit(owner, generation); opened = true; }
        }
    }
    if (status == XR_OS_IO_OK) *installed = opened;
    ReleaseSRWLockExclusive(&signal_owner_lock);
    return status;
}
XR_FUNC XrOsIoStatus xr_os_io_signal_take(XrOsSignalOwner *owner, bool *pending) {
    if (!owner || !pending) return XR_OS_IO_BAD_ARGUMENT;
    AcquireSRWLockExclusive(&signal_owner_lock);
    XrOsIoStatus status = XR_OS_IO_BAD_ARGUMENT;
    if (signal_matches(owner) && !signal_int_slot.closing &&
        atomic_load_explicit(&signal_int_slot.word, memory_order_acquire) >> 1 == owner->generation) {
        uint32_t prior = atomic_fetch_and_explicit(&signal_int_slot.word, ~UINT32_C(1), memory_order_acq_rel);
        *pending = (prior & 1u) != 0;
        status = XR_OS_IO_OK;
    }
    ReleaseSRWLockExclusive(&signal_owner_lock);
    return status;
}
XR_FUNC XrOsIoStatus xr_os_io_signal_close(XrOsSignalOwner *owner) {
    if (!owner) return XR_OS_IO_BAD_ARGUMENT;
    AcquireSRWLockExclusive(&signal_owner_lock);
    XrOsIoStatus status = XR_OS_IO_OK;
    if (!signal_empty(owner)) {
        if (!signal_matches(owner)) status = XR_OS_IO_BAD_ARGUMENT;
        else {
            signal_int_slot.closing = true;
            atomic_store_explicit(&signal_int_slot.word, 0, memory_order_release);
            if (!SetConsoleCtrlHandler(signal_console_handler, FALSE)) status = signal_windows_error(GetLastError());
            else {
                signal_int_slot.owner = NULL;
                signal_int_slot.token = 0; signal_int_slot.generation = 0; signal_int_slot.closing = false;
                *owner = (XrOsSignalOwner){0};
            }
        }
    }
    ReleaseSRWLockExclusive(&signal_owner_lock);
    return status;
}
