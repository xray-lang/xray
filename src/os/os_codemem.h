/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_codemem.h - Cross-platform anonymous and JIT-executable memory.
 *
 * Provides two layers:
 *
 *   1. Generic anonymous memory (mmap MAP_ANON / VirtualAlloc):
 *        xr_os_mem_alloc / xr_os_mem_protect / xr_os_mem_free
 *      Used for guard pages, trap pages, and similar non-code regions.
 *
 *   2. JIT executable memory (W^X aware):
 *        xr_os_codemem_alloc / xr_os_codemem_free
 *        xr_os_codemem_make_writable / xr_os_codemem_make_executable
 *        xr_os_codemem_flush_icache
 *      Hides the macOS Apple Silicon MAP_JIT + pthread_jit_write_protect_np
 *      contract, the Linux mprotect-flip contract, and the Windows
 *      VirtualProtect contract behind a uniform API.
 *
 * Invariants:
 *   - Memory returned by xr_os_codemem_alloc starts in a writable state;
 *     callers must call xr_os_codemem_make_executable + xr_os_codemem_flush_icache
 *     before invoking the code.
 *   - On Apple Silicon, make_writable / make_executable are per-thread (MAP_JIT)
 *     and cheap; on Linux/x86 they are per-page mprotect calls.
 */

#ifndef XR_OS_OS_CODEMEM_H
#define XR_OS_OS_CODEMEM_H

#include <stddef.h>

#include "../base/xdefs.h"
#include "../base/xplatform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XrMemProt {
    XR_MEM_PROT_NONE = 0,  // no access
    XR_MEM_PROT_R = 1,     // read only
    XR_MEM_PROT_RW = 2,    // read + write
    XR_MEM_PROT_RX = 3,    // read + execute
} XrMemProt;

// Cached system page size (>= 4096 in practice).
XR_FUNC size_t xr_os_page_size(void);

// Allocate `size` bytes of anonymous memory with initial `prot`.
// `size` is rounded up to a page boundary by the implementation.
// Returns NULL on failure.
XR_FUNC void *xr_os_mem_alloc(size_t size, XrMemProt prot);

// Change the protection of [ptr, ptr+size) to `prot`.
// Returns 0 on success, -1 on failure.
XR_FUNC int xr_os_mem_protect(void *ptr, size_t size, XrMemProt prot);

// Release memory previously returned by xr_os_mem_alloc / xr_os_codemem_alloc.
XR_FUNC void xr_os_mem_free(void *ptr, size_t size);

// ---- JIT executable memory ----

// Reserve a region of `size` bytes for JIT-emitted code.
// The returned region is initially writable. On Apple Silicon it is
// reserved with MAP_JIT and the calling thread is placed in write mode.
// Returns NULL on failure.
XR_FUNC void *xr_os_codemem_alloc(size_t size);

// Release a region previously returned by xr_os_codemem_alloc.
XR_FUNC void xr_os_codemem_free(void *ptr, size_t size);

// Switch [ptr, ptr+size) to writable (R+W). On Apple Silicon this is a
// thread-local toggle and `ptr` / `size` are advisory; on Linux it is
// a per-page mprotect.
XR_FUNC void xr_os_codemem_make_writable(void *ptr, size_t size);

// Switch [ptr, ptr+size) to executable (R+X). Same caveat as above.
XR_FUNC void xr_os_codemem_make_executable(void *ptr, size_t size);

// Flush the instruction cache for [ptr, ptr+size). No-op on x86_64.
XR_FUNC void xr_os_codemem_flush_icache(void *ptr, size_t size);

#ifdef __cplusplus
}
#endif

#endif  // XR_OS_OS_CODEMEM_H
