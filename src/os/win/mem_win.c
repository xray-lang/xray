/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmem_win.c - Windows implementation of xmem.h.
 *
 * VirtualAlloc with MEM_RESERVE | MEM_COMMIT gives zero-filled
 * anonymous pages — the same contract as POSIX MAP_ANONYMOUS.
 * The Windows protection-flag space is much smaller than POSIX:
 * there are no individual READ/WRITE/EXEC bits, only a fixed
 * set of combinations (PAGE_NOACCESS, PAGE_READONLY,
 * PAGE_READWRITE, PAGE_EXECUTE, PAGE_EXECUTE_READ,
 * PAGE_EXECUTE_READWRITE). prot_to_win() picks the strongest
 * matching combination so xray callers never see a "permission
 * downgrade" they didn't request.
 *
 * Note: VirtualFree(MEM_RELEASE) requires size = 0; the size
 * parameter on xr_mem_unmap is therefore ignored on Windows.
 * Callers tracking allocation size for accounting still pass
 * the original size — this matches POSIX semantics.
 */

#include "../os_mem.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static DWORD prot_to_win(int prot) {
    bool r = (prot & XR_MEM_PROT_READ) != 0;
    bool w = (prot & XR_MEM_PROT_WRITE) != 0;
    bool x = (prot & XR_MEM_PROT_EXEC) != 0;
    if (!r && !w && !x)
        return PAGE_NOACCESS;
    if (x && w)
        return PAGE_EXECUTE_READWRITE;
    if (x && r)
        return PAGE_EXECUTE_READ;
    if (x)
        return PAGE_EXECUTE;
    if (w)
        return PAGE_READWRITE;
    return PAGE_READONLY;
}

void *xr_mem_map(size_t size, int prot) {
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, prot_to_win(prot));
}

void xr_mem_unmap(void *ptr, size_t size) {
    (void) size;
    if (ptr)
        VirtualFree(ptr, 0, MEM_RELEASE);
}

bool xr_mem_protect(void *ptr, size_t size, int prot) {
    DWORD old;
    return VirtualProtect(ptr, size, prot_to_win(prot), &old) != 0;
}

#endif  // _WIN32
