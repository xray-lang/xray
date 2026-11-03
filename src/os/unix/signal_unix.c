/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * signal_unix.c - Persistent POSIX signal publication and owned disposition restore
 */
#include "../os_signal.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && UINT_MAX == UINT32_MAX,
    "signal mailbox publication requires always-lock-free 32-bit integers");
typedef struct SignalSlot {
    _Atomic(uint32_t) word;
    XrOsSignalOwner *owner;
    uint64_t token;
    uint32_t generation;
    int native_signal;
    bool closing;
    struct sigaction previous;
} SignalSlot;
static SignalSlot signal_int_slot, signal_term_slot;
static uint32_t signal_epoch;
static pthread_mutex_t signal_owner_lock = PTHREAD_MUTEX_INITIALIZER;

static void signal_pending_handler(int signal) {
    _Atomic(uint32_t) *word = signal == SIGINT ? &signal_int_slot.word :
        signal == SIGTERM ? &signal_term_slot.word : NULL;
    if (!word) return;
    uint32_t observed = atomic_load_explicit(word, memory_order_relaxed);
    if (observed && !(observed & 1u))
        (void)atomic_compare_exchange_strong_explicit(word, &observed, observed | 1u,
            memory_order_relaxed, memory_order_relaxed);
}
static bool signal_empty(const XrOsSignalOwner *owner) {
    return !owner->token && !owner->generation && !owner->signal;
}
static SignalSlot *signal_slot(int64_t signal) {
    return signal == 2 ? &signal_int_slot : signal == 15 ? &signal_term_slot : NULL;
}
static bool signal_matches(const SignalSlot *slot, const XrOsSignalOwner *owner) {
    return slot && slot->owner == owner && owner->token && owner->token == slot->token &&
        owner->generation == slot->generation;
}
static XrOsIoStatus signal_unix_error(int error) {
    if (error == ENOMEM) return XR_OS_IO_OUT_OF_MEMORY;
    if (error == EINVAL) return XR_OS_IO_BAD_ARGUMENT;
    return XR_OS_IO_IO;
}
static void signal_commit(SignalSlot *slot, XrOsSignalOwner *owner, int32_t signal, uint32_t generation) {
    uint64_t token = ((uint64_t)generation << 32) | (uint32_t)signal;
    slot->owner = owner; slot->token = token; slot->generation = generation; slot->closing = false;
    slot->native_signal = signal == 2 ? SIGINT : SIGTERM;
    *owner = (XrOsSignalOwner){token, generation, signal};
}
XR_FUNC XrOsIoStatus xr_os_io_signal_open(int64_t signal, XrOsSignalOwner *owner, bool *installed) {
    if (!owner || !installed) return XR_OS_IO_BAD_ARGUMENT;
    if (pthread_mutex_lock(&signal_owner_lock) != 0) return XR_OS_IO_IO;
    XrOsIoStatus status = XR_OS_IO_OK; bool opened = false;
    SignalSlot *slot = signal_slot(signal);
    if (!signal_empty(owner)) {
        if (!slot || owner->signal != signal || !signal_matches(slot, owner) || slot->closing) status = XR_OS_IO_BAD_ARGUMENT;
        else if (signal_epoch == (UINT32_MAX >> 1)) status = XR_OS_IO_OVERFLOW;
        else {
            uint32_t generation = ++signal_epoch;
            signal_commit(slot, owner, (int32_t)signal, generation);
            atomic_store_explicit(&slot->word, generation << 1, memory_order_release);
            opened = true;
        }
    } else if (slot && !slot->owner) {
        if (signal_epoch == (UINT32_MAX >> 1)) status = XR_OS_IO_OVERFLOW;
        else {
            struct sigaction action, previous;
            memset(&action, 0, sizeof(action));
            action.sa_handler = signal_pending_handler;
            if (sigemptyset(&action.sa_mask) != 0) status = signal_unix_error(errno);
            else {
                uint32_t generation = ++signal_epoch;
                atomic_store_explicit(&slot->word, generation << 1, memory_order_release);
                if (sigaction(signal == 2 ? SIGINT : SIGTERM, &action, &previous) != 0) {
                    int error = errno;
                    atomic_store_explicit(&slot->word, 0, memory_order_release);
                    status = signal_unix_error(error);
                } else {
                    slot->previous = previous;
                    signal_commit(slot, owner, (int32_t)signal, generation);
                    opened = true;
                }
            }
        }
    }
    if (status == XR_OS_IO_OK) *installed = opened;
    (void)pthread_mutex_unlock(&signal_owner_lock);
    return status;
}
XR_FUNC XrOsIoStatus xr_os_io_signal_take(XrOsSignalOwner *owner, bool *pending) {
    if (!owner || !pending) return XR_OS_IO_BAD_ARGUMENT;
    if (pthread_mutex_lock(&signal_owner_lock) != 0) return XR_OS_IO_IO;
    XrOsIoStatus status = XR_OS_IO_BAD_ARGUMENT;
    SignalSlot *slot = signal_slot(owner->signal);
    if (signal_matches(slot, owner) && !slot->closing &&
        atomic_load_explicit(&slot->word, memory_order_acquire) >> 1 == owner->generation) {
        uint32_t prior = atomic_fetch_and_explicit(&slot->word, ~UINT32_C(1), memory_order_acq_rel);
        *pending = (prior & 1u) != 0; status = XR_OS_IO_OK;
    }
    (void)pthread_mutex_unlock(&signal_owner_lock);
    return status;
}
XR_FUNC XrOsIoStatus xr_os_io_signal_close(XrOsSignalOwner *owner) {
    if (!owner) return XR_OS_IO_BAD_ARGUMENT;
    if (pthread_mutex_lock(&signal_owner_lock) != 0) return XR_OS_IO_IO;
    XrOsIoStatus status = XR_OS_IO_OK;
    if (!signal_empty(owner)) {
        SignalSlot *slot = signal_slot(owner->signal);
        if (!signal_matches(slot, owner)) status = XR_OS_IO_BAD_ARGUMENT;
        else {
            slot->closing = true;
            atomic_store_explicit(&slot->word, 0, memory_order_release);
            struct sigaction actual;
            if (sigaction(slot->native_signal, NULL, &actual) != 0) status = signal_unix_error(errno);
            else if ((actual.sa_flags & SA_SIGINFO) || actual.sa_handler != signal_pending_handler) status = XR_OS_IO_IO;
            else if (sigaction(slot->native_signal, &slot->previous, NULL) != 0) status = signal_unix_error(errno);
            else {
                slot->owner = NULL; slot->token = 0; slot->generation = 0; slot->closing = false;
                *owner = (XrOsSignalOwner){0};
            }
        }
    }
    (void)pthread_mutex_unlock(&signal_owner_lock);
    return status;
}
