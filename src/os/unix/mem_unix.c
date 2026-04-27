/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmem_unix.c - POSIX implementation of xmem.h.
 *
 * mmap with MAP_PRIVATE | MAP_ANONYMOUS gives zero-filled
 * anonymous pages, which is the contract callers expect. We
 * translate the xray PROT bitset onto POSIX PROT_* and ignore
 * unsupported combinations rather than fail; a request for
 * EXEC|WRITE is legal on POSIX (W^X enforcement is a JIT
 * concern that lives outside this layer).
 */

#include "../os_mem.h"

#ifndef _WIN32

#include <sys/mman.h>

// Some BSDs spell it MAP_ANON; Linux/macOS expose both via
// glibc/SDK headers. Prefer MAP_ANONYMOUS where available, fall
// back to MAP_ANON otherwise.
#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

static int prot_to_posix(int prot) {
    int out = 0;
    if (prot & XR_MEM_PROT_READ)
        out |= PROT_READ;
    if (prot & XR_MEM_PROT_WRITE)
        out |= PROT_WRITE;
    if (prot & XR_MEM_PROT_EXEC)
        out |= PROT_EXEC;
    if (out == 0)
        out = PROT_NONE;
    return out;
}

void *xr_mem_map(size_t size, int prot) {
    void *p = mmap(NULL, size, prot_to_posix(prot), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    return p;
}

void xr_mem_unmap(void *ptr, size_t size) {
    if (ptr)
        munmap(ptr, size);
}

bool xr_mem_protect(void *ptr, size_t size, int prot) {
    return mprotect(ptr, size, prot_to_posix(prot)) == 0;
}

#endif  // !_WIN32
