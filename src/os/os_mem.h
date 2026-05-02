/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmem.h - Cross-platform anonymous virtual-memory allocation.
 *
 * KEY CONCEPT:
 *   Wraps the OS page allocator behind a single nanosecond-cheap
 *   API used by the GC and any other code that needs page-aligned
 *   anonymous memory. The implementation maps onto:
 *
 *     POSIX:  mmap(MAP_PRIVATE | MAP_ANONYMOUS) + munmap + mprotect
 *     Windows: VirtualAlloc(MEM_RESERVE | MEM_COMMIT) +
 *              VirtualFree(MEM_RELEASE) + VirtualProtect
 *
 *   Protection flags use an OR-able xray-specific bitset that is
 *   translated to the platform-native flags inside the impl. This
 *   keeps PROT_READ / PAGE_READWRITE / etc. out of public headers.
 *
 * SCOPE:
 *   - General-purpose anonymous memory (GC large objects, etc.)
 *   - JIT executable pages with their macOS MAP_JIT / pthread W^X
 *     fast-toggle semantics are handled separately by
 *     src/jit/xm_code_alloc.c, which keeps the platform-specific
 *     toggle logic next to the JIT itself.
 *
 * RELATED MODULES:
 *   - base/xdefs.h   for visibility macros
 */

#ifndef XMEM_H
#define XMEM_H

#include "../base/xdefs.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Page protection bits. Combine with bitwise-or; OS-specific
// translation happens inside xmem_unix.c / xmem_win.c.
typedef enum {
    XR_MEM_PROT_NONE = 0,
    XR_MEM_PROT_READ = 1 << 0,
    XR_MEM_PROT_WRITE = 1 << 1,
    XR_MEM_PROT_EXEC = 1 << 2,
} XrMemProt;

// Allocate `size` bytes of anonymous, page-aligned memory with the
// given initial protection. Returns NULL on failure. The pointer
// must later be released with xr_mem_unmap.
XR_FUNC void *xr_mem_map(size_t size, int prot);

// Release a region previously returned by xr_mem_map. `size` must
// match the original allocation on POSIX; Windows ignores it.
XR_FUNC void xr_mem_unmap(void *ptr, size_t size);

// Change the protection bits on an existing region. Returns true
// on success. On Windows, the previous protection is discarded.
XR_FUNC bool xr_mem_protect(void *ptr, size_t size, int prot);

#ifdef __cplusplus
}
#endif

#endif  // XMEM_H
