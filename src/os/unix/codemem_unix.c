/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * codemem_unix.c - POSIX implementation of os_codemem.h.
 *
 * Layout: anonymous mmap + mprotect for both generic memory and JIT.
 * On Apple Silicon (arm64 macOS), JIT regions are allocated with MAP_JIT
 * and W^X is toggled per-thread via pthread_jit_write_protect_np().
 * On Linux/x86_64 and other POSIX targets, JIT regions live in normal
 * anonymous mappings and W^X is enforced per-page via mprotect().
 */

#include "../os_codemem.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(XR_OS_MACOS)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif

// -- helpers --

static int prot_to_posix(XrMemProt p) {
    switch (p) {
        case XR_MEM_PROT_NONE:
            return PROT_NONE;
        case XR_MEM_PROT_R:
            return PROT_READ;
        case XR_MEM_PROT_RW:
            return PROT_READ | PROT_WRITE;
        case XR_MEM_PROT_RX:
            return PROT_READ | PROT_EXEC;
    }
    return PROT_NONE;
}

static size_t round_to_page(size_t size, size_t ps) {
    return (size + ps - 1) & ~(ps - 1);
}

// -- public api --

size_t xr_os_page_size(void) {
    static size_t cached = 0;
    if (cached == 0) {
        long ps = sysconf(_SC_PAGESIZE);
        cached = (ps > 0) ? (size_t) ps : 4096;
    }
    return cached;
}

void *xr_os_mem_alloc(size_t size, XrMemProt prot) {
    if (size == 0) {
        return NULL;
    }
    size_t aligned = round_to_page(size, xr_os_page_size());
    void *p = mmap(NULL, aligned, prot_to_posix(prot), MAP_PRIVATE | MAP_ANON, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
}

int xr_os_mem_protect(void *ptr, size_t size, XrMemProt prot) {
    if (ptr == NULL || size == 0) {
        return -1;
    }
    return mprotect(ptr, size, prot_to_posix(prot));
}

void xr_os_mem_free(void *ptr, size_t size) {
    if (ptr == NULL || size == 0) {
        return;
    }
    size_t aligned = round_to_page(size, xr_os_page_size());
    munmap(ptr, aligned);
}

void *xr_os_codemem_alloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    size_t aligned = round_to_page(size, xr_os_page_size());

    int flags = MAP_PRIVATE | MAP_ANON;
    int prot;
#if defined(XR_OS_MACOS)
    flags |= MAP_JIT;
    // MAP_JIT pages must be reserved with all three permissions; the
    // pthread_jit_write_protect_np switch toggles between W and X at
    // runtime for the calling thread.
    prot = PROT_READ | PROT_WRITE | PROT_EXEC;
#else
    prot = PROT_READ | PROT_WRITE;
#endif

    void *mem = mmap(NULL, aligned, prot, flags, -1, 0);
    if (mem == MAP_FAILED) {
        return NULL;
    }

#if defined(XR_OS_MACOS)
    // Start in writable mode for the caller.
    pthread_jit_write_protect_np(0);
#endif

    return mem;
}

void xr_os_codemem_free(void *ptr, size_t size) {
    xr_os_mem_free(ptr, size);
}

void xr_os_codemem_make_writable(void *ptr, size_t size) {
    (void) ptr;
    (void) size;
#if defined(XR_OS_MACOS)
    pthread_jit_write_protect_np(0);
#else
    if (ptr == NULL || size == 0) {
        return;
    }
    size_t ps = xr_os_page_size();
    uintptr_t start = (uintptr_t) ptr & ~(ps - 1);
    size_t total = ((uintptr_t) ptr + size) - start;
    total = round_to_page(total, ps);
    mprotect((void *) start, total, PROT_READ | PROT_WRITE);
#endif
}

void xr_os_codemem_make_executable(void *ptr, size_t size) {
    (void) ptr;
    (void) size;
#if defined(XR_OS_MACOS)
    pthread_jit_write_protect_np(1);
#else
    if (ptr == NULL || size == 0) {
        return;
    }
    size_t ps = xr_os_page_size();
    uintptr_t start = (uintptr_t) ptr & ~(ps - 1);
    size_t total = ((uintptr_t) ptr + size) - start;
    total = round_to_page(total, ps);
    mprotect((void *) start, total, PROT_READ | PROT_EXEC);
#endif
}

void xr_os_codemem_flush_icache(void *ptr, size_t size) {
    if (ptr == NULL || size == 0) {
        return;
    }
#if defined(XR_OS_MACOS)
    sys_icache_invalidate(ptr, size);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char *) ptr, (char *) ptr + size);
#else
    (void) ptr;
    (void) size;
#endif
}
