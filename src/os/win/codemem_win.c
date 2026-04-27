/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * codemem_win.c - Windows implementation of os_codemem.h.
 *
 * Layout: VirtualAlloc with MEM_RESERVE|MEM_COMMIT, VirtualProtect for
 * permission flips, FlushInstructionCache for icache sync. Apple
 * Silicon's MAP_JIT does not exist on Windows; W^X is enforced strictly
 * per-region via VirtualProtect.
 */

#include "../os_codemem.h"

#include <stddef.h>
#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// -- helpers --

static DWORD prot_to_win(XrMemProt p) {
    switch (p) {
        case XR_MEM_PROT_NONE:
            return PAGE_NOACCESS;
        case XR_MEM_PROT_R:
            return PAGE_READONLY;
        case XR_MEM_PROT_RW:
            return PAGE_READWRITE;
        case XR_MEM_PROT_RX:
            return PAGE_EXECUTE_READ;
    }
    return PAGE_NOACCESS;
}

static size_t cached_page_size = 0;

// -- public api --

size_t xr_os_page_size(void) {
    if (cached_page_size == 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        cached_page_size = (size_t) si.dwPageSize;
        if (cached_page_size == 0) {
            cached_page_size = 4096;
        }
    }
    return cached_page_size;
}

void *xr_os_mem_alloc(size_t size, XrMemProt prot) {
    if (size == 0) {
        return NULL;
    }
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, prot_to_win(prot));
}

int xr_os_mem_protect(void *ptr, size_t size, XrMemProt prot) {
    if (ptr == NULL || size == 0) {
        return -1;
    }
    DWORD old;
    return VirtualProtect(ptr, size, prot_to_win(prot), &old) ? 0 : -1;
}

void xr_os_mem_free(void *ptr, size_t size) {
    (void) size;
    if (ptr != NULL) {
        VirtualFree(ptr, 0, MEM_RELEASE);
    }
}

void *xr_os_codemem_alloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void xr_os_codemem_free(void *ptr, size_t size) {
    xr_os_mem_free(ptr, size);
}

void xr_os_codemem_make_writable(void *ptr, size_t size) {
    if (ptr == NULL || size == 0) {
        return;
    }
    DWORD old;
    VirtualProtect(ptr, size, PAGE_READWRITE, &old);
}

void xr_os_codemem_make_executable(void *ptr, size_t size) {
    if (ptr == NULL || size == 0) {
        return;
    }
    DWORD old;
    VirtualProtect(ptr, size, PAGE_EXECUTE_READ, &old);
}

void xr_os_codemem_flush_icache(void *ptr, size_t size) {
    if (ptr == NULL || size == 0) {
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), ptr, size);
}
